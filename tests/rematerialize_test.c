/* M3 phase 3: deterministic re-materialise-and-retry tests
 * for DISJOINT-page publish conflicts.
 *
 * Scenario A (disjoint success): parent gen 1; writer1 commits gen 2 touching
 * pages 1-2; writer2's frames touch pages 4-5 (disjoint), CAS fails against
 * gen 1; otsardb_wal_publisher_retry_publish re-materialises the local image at
 * gen 2 and re-publishes the SAME frames -> gen 3 containing BOTH commits;
 * recovery yields both writers' pages.
 *
 * Scenario B (overlap refused): writer2's frames touch page 2 (overlap with
 * writer1). The caller-side local overlap check reports overlap; the
 * precondition forbids retry_publish, and the function's defence-in-depth
 * returns -1 without publishing — HEAD stays at gen 2 and the local image is
 * untouched.
 *
 * The publisher is used directly (initialised against the test stores) with
 * no VFS attachment, like the other unit tests.
 */

#include "commit_manifest.h"
#include "commit_publish.h"
#include "db_metadata.h"
#include "journal.h"
#include "memory_store.h"
#include "recovery.h"
#include "sqlite3.h"
#include "sqlite_wal.h"
#include "wal_batch.h"
#include "wal_publisher.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#define otsardb_unlink _unlink
#else
#include <unistd.h>
#define otsardb_unlink unlink
#endif

#define TEST_PAGE_SIZE 4096u
#define TEST_DB_PAGES 6u
#define TEST_ROOT "mw/remat"
#define TEST_DB_ID "db-remat"

/* A page's content is a single repeating byte 0x40 + page_no, so every page
 * carries a distinct, verifiable pattern. */
static unsigned char page_marker(uint32_t page_no) {
    return (unsigned char)(0x40u + (page_no & 0x3fu));
}

static void publish_commit(otsardb_object_store *store,
                           const char *root,
                           const char *db_id,
                           const char *commit_id,
                           uint64_t parent_generation,
                           const char *parent_commit_id,
                           const char *parent_manifest_sha256,
                           const char *expected_head_etag,
                           const uint32_t *page_nos,
                           uint32_t page_count,
                           otsardb_commit_result *out_result) {
    unsigned char *pages = (unsigned char *)malloc((size_t)page_count * TEST_PAGE_SIZE);
    assert(pages != NULL);
    for (uint32_t i = 0; i < page_count; ++i) {
        memset(pages + (size_t)i * TEST_PAGE_SIZE, page_marker(page_nos[i]), TEST_PAGE_SIZE);
    }

    unsigned char *batch = NULL;
    size_t batch_size = 0;
    assert(otsardb_wal_batch_encode(TEST_PAGE_SIZE,
                                  TEST_DB_PAGES,
                                  page_nos,
                                  pages,
                                  page_count,
                                  &batch,
                                  &batch_size));
    free(pages);

    otsardb_segment_input input;
    memset(&input, 0, sizeof(input));
    input.payload = batch;
    input.payload_size = batch_size;
    input.pages = page_nos;
    input.page_count = page_count;

    otsardb_commit_request request;
    memset(&request, 0, sizeof(request));
    request.database_root = root;
    request.database_id = db_id;
    request.commit_id = commit_id;
    request.parent_generation = parent_generation;
    request.parent_commit_id = parent_generation == 0 ? NULL : parent_commit_id;
    request.parent_manifest_sha256 = parent_generation == 0 ? NULL : parent_manifest_sha256;
    request.page_size = TEST_PAGE_SIZE;
    request.database_size_pages = TEST_DB_PAGES;
    request.segments = &input;
    request.segment_count = 1;
    request.create_head_if_missing = parent_generation == 0;
    request.expected_head_etag = expected_head_etag;

    otsardb_journal_result rc = otsardb_publish_commit(store, &request, out_result);
    free(batch);
    assert(rc == OTSARDB_JOURNAL_OK);
    assert(out_result->head.generation == parent_generation + 1u);
}

/* Read the current remote HEAD. */
static otsardb_head read_head(otsardb_object_store *store, const char *root) {
    otsardb_head head;
    char *etag = NULL;
    assert(otsardb_journal_read_head(store, root, &head, &etag) == OTSARDB_JOURNAL_OK);
    free(etag);
    return head;
}

/* Local page-overlap check, mirroring otsardb_publish_commit_detect_conflict
 * (the parallel M3 phase 2 function): walk the manifest chain from the
 * current remote HEAD down to (exclusive) stop_generation and intersect each
 * interleaved manifest's per-segment page sets with `pages` (sorted).
 * Returns 1 on overlap (including unknown page sets), 0 on disjoint. */
static int chain_overlaps_pages(otsardb_object_store *store,
                                const char *root,
                                uint64_t stop_generation,
                                const uint32_t *pages,
                                uint32_t page_count) {
    otsardb_head head = read_head(store, root);
    uint64_t generation = head.generation;
    char commit_id[OTSARDB_COMMIT_ID_MAX + 1];
    snprintf(commit_id, sizeof(commit_id), "%s", head.commit_id);

    while (generation > stop_generation) {
        char key[OTSARDB_KEY_MAX + 1];
        int key_len = snprintf(key, sizeof(key), "%s/commits/%s.json", root, commit_id);
        assert(key_len > 0 && (size_t)key_len < sizeof(key));

        otsardb_store_object object = {0};
        assert(otsardb_store_get(store, key, &object) == OTSARDB_STORE_OK);
        otsardb_commit_manifest manifest;
        assert(otsardb_commit_manifest_decode_json((const char *)object.data,
                                                 object.size,
                                                 &manifest));
        assert(manifest.generation == generation);
        otsardb_store_release_object(store, &object);

        for (uint32_t s = 0; s < manifest.segment_count; ++s) {
            const otsardb_manifest_segment_ref *ref = &manifest.segments[s];
            if (ref->page_count == 0 || !ref->pages) {
                /* Unknown page set: conservative overlap. */
                otsardb_commit_manifest_free(&manifest);
                return 1;
            }
            uint32_t i = 0;
            uint32_t j = 0;
            while (i < page_count && j < ref->page_count) {
                if (pages[i] < ref->pages[j]) {
                    ++i;
                } else if (ref->pages[j] < pages[i]) {
                    ++j;
                } else {
                    otsardb_commit_manifest_free(&manifest);
                    return 1;
                }
            }
        }

        if (manifest.parent_generation == 0) {
            otsardb_commit_manifest_free(&manifest);
            break;
        }
        snprintf(commit_id, sizeof(commit_id), "%s", manifest.parent_commit_id);
        generation = manifest.parent_generation;
        otsardb_commit_manifest_free(&manifest);
    }
    return 0;
}

static void read_page(const char *path, uint32_t page_no, unsigned char *out_page) {
    FILE *file = fopen(path, "rb");
    assert(file != NULL);
    uint64_t offset = ((uint64_t)page_no - 1u) * TEST_PAGE_SIZE;
    assert(offset <= (uint64_t)LONG_MAX);
    assert(fseek(file, (long)offset, SEEK_SET) == 0);
    assert(fread(out_page, 1, TEST_PAGE_SIZE, file) == TEST_PAGE_SIZE);
    fclose(file);
}

static void assert_page_content(const char *path, uint32_t page_no, unsigned char marker) {
    unsigned char *page = (unsigned char *)malloc(TEST_PAGE_SIZE);
    assert(page != NULL);
    read_page(path, page_no, page);
    for (uint32_t i = 0; i < TEST_PAGE_SIZE; ++i) {
        assert(page[i] == marker);
    }
    free(page);
}

/* Build a synthetic local WAL file carrying page images for the retry frames.
 * The retry path only re-reads page bytes at frame_offset + 24 and does not
 * re-validate WAL framing, so the header/frame headers may be zeroed. */
static void build_wal_file(const char *wal_path,
                           const uint32_t *page_nos,
                           uint32_t frame_count,
                           otsardb_sqlite_wal_frame_ref *out_frames) {
    uint64_t frame_size = (uint64_t)OTSARDB_SQLITE_WAL_FRAME_HEADER_SIZE + TEST_PAGE_SIZE;
    uint64_t total = (uint64_t)OTSARDB_SQLITE_WAL_HEADER_SIZE + (uint64_t)frame_count * frame_size;
    unsigned char *buf = (unsigned char *)calloc(1, (size_t)total);
    assert(buf != NULL);

    for (uint32_t i = 0; i < frame_count; ++i) {
        out_frames[i].page_no = page_nos[i];
        out_frames[i].frame_offset =
            (uint64_t)OTSARDB_SQLITE_WAL_HEADER_SIZE + (uint64_t)i * frame_size;
        unsigned char *page = buf + out_frames[i].frame_offset +
                               (uint64_t)OTSARDB_SQLITE_WAL_FRAME_HEADER_SIZE;
        memset(page, page_marker(page_nos[i]), TEST_PAGE_SIZE);
    }

    FILE *file = fopen(wal_path, "wb");
    assert(file != NULL);
    assert(fwrite(buf, 1, (size_t)total, file) == (size_t)total);
    assert(fclose(file) == 0);
    free(buf);
}

/* Shared two-writer setup used by both scenarios:
 * - genesis commit gen 1 touching page 3;
 * - the local execution image is materialised at gen 1 (the attempt's
 * parent) into local_db_path BEFORE writer1 commits;
 * - writer1 commits gen 2 touching pages 1-2;
 * - writer2's attempt against gen 1 CAS-fails (its commit_id, segments and
 * manifest are uploaded as orphans before the HEAD CAS rejects it).
 * The publisher caches gen 1 as its parent before writer1 commits. */
static otsardb_object_store *setup_two_writers(otsardb_wal_publisher *publisher,
                                             const uint32_t *w2_pages,
                                             uint32_t w2_page_count,
                                             const char *local_db_path,
                                             otsardb_commit_result *out_w1) {
    otsardb_object_store *store = otsardb_test_memory_store_create();
    assert(store != NULL);

    otsardb_db_metadata metadata;
    memset(&metadata, 0, sizeof(metadata));
    metadata.format_version = 1;
    metadata.page_size = TEST_PAGE_SIZE;
    snprintf(metadata.database_id, sizeof(metadata.database_id), "%s", TEST_DB_ID);
    assert(otsardb_db_metadata_create(store, TEST_ROOT, &metadata) == OTSARDB_METADATA_OK);

    const uint32_t genesis_pages[] = {3};
    const uint32_t w1_pages[] = {1, 2};

    otsardb_commit_result genesis;
    publish_commit(store, TEST_ROOT, TEST_DB_ID, "g1",
                   0, NULL, NULL, NULL,
                   genesis_pages, 1, &genesis);

    /* The local execution image the transaction runs against: materialised
     * at the attempt's parent (gen 1, only page 3 written). */
    assert(otsardb_recovery_restore_store(store, TEST_ROOT, local_db_path));

    /* Writer2's publisher caches gen 1 as its execution parent. */
    assert(otsardb_wal_publisher_init(publisher,
                                    store,
                                    TEST_ROOT,
                                    TEST_DB_ID,
                                    OTSARDB_WRITE_STRATEGY_DEFAULT));
    assert(publisher->strategy == OTSARDB_WRITE_STRATEGY_CAS);
    assert(otsardb_wal_publisher_refresh_head(publisher) == SQLITE_OK);
    assert(publisher->current_generation == 1);
    assert(strcmp(publisher->current_commit_id, "g1") == 0);
    assert(publisher->current_head_etag[0] != '\0');

    publish_commit(store, TEST_ROOT, TEST_DB_ID, "w1",
                   1, "g1", genesis.head.commit_sha256, genesis.head_etag,
                   w1_pages, 2, out_w1);
    assert(out_w1->head.generation == 2);

    /* Writer2's publish attempt against its cached parent: CAS fails because
     * writer1 won the gen-2 slot. */
    unsigned char *w2_pages_bytes = (unsigned char *)malloc((size_t)w2_page_count * TEST_PAGE_SIZE);
    assert(w2_pages_bytes != NULL);
    for (uint32_t i = 0; i < w2_page_count; ++i) {
        memset(w2_pages_bytes + (size_t)i * TEST_PAGE_SIZE,
               page_marker(w2_pages[i]),
               TEST_PAGE_SIZE);
    }
    unsigned char *w2_batch = NULL;
    size_t w2_batch_size = 0;
    assert(otsardb_wal_batch_encode(TEST_PAGE_SIZE,
                                  TEST_DB_PAGES,
                                  w2_pages,
                                  w2_pages_bytes,
                                  w2_page_count,
                                  &w2_batch,
                                  &w2_batch_size));
    free(w2_pages_bytes);

    otsardb_segment_input w2_input;
    memset(&w2_input, 0, sizeof(w2_input));
    w2_input.payload = w2_batch;
    w2_input.payload_size = w2_batch_size;
    w2_input.pages = w2_pages;
    w2_input.page_count = w2_page_count;

    otsardb_commit_request w2_request;
    memset(&w2_request, 0, sizeof(w2_request));
    w2_request.database_root = TEST_ROOT;
    w2_request.database_id = TEST_DB_ID;
    w2_request.commit_id = "w2-cas-fail";
    w2_request.parent_commit_id = "g1";
    w2_request.parent_manifest_sha256 = genesis.head.commit_sha256;
    w2_request.parent_generation = 1;
    w2_request.page_size = TEST_PAGE_SIZE;
    w2_request.database_size_pages = TEST_DB_PAGES;
    w2_request.segments = &w2_input;
    w2_request.segment_count = 1;
    w2_request.expected_head_etag = publisher->current_head_etag;

    otsardb_commit_result w2_result;
    assert(otsardb_publish_commit(store, &w2_request, &w2_result) ==
           OTSARDB_JOURNAL_CONFLICT);
    free(w2_batch);
    return store;
}

int main(void) {
    const char *local_path = "remat-local.db";
    const char *wal_path = "remat-local.db-wal";
    const char *recovered_path = "remat-recovered.db";

    /* ---------------- Scenario A: disjoint pages converge ---------------- */
    otsardb_unlink(local_path);
    otsardb_unlink(wal_path);
    otsardb_unlink(recovered_path);

    const uint32_t w2_pages[] = {4, 5};
    otsardb_wal_publisher publisher;
    otsardb_commit_result w1;
    otsardb_object_store *store = setup_two_writers(&publisher, w2_pages, 2, local_path, &w1);

    /* Caller-side classification: the interleaved commits (gen 2, pages 1-2)
     * do not intersect writer2's pages 4-5 -> disjoint, retry is sound. */
    assert(chain_overlaps_pages(store, TEST_ROOT, 1, w2_pages, 2) == 0);

    /* The local image is the attempt's parent (gen 1, only page 3 written):
     * page 3 carries the genesis marker, page 2 is still zero (writer1's
     * gen-2 page is NOT present in the image the transaction ran against). */
    assert_page_content(local_path, 3, page_marker(3));
    unsigned char *page2 = (unsigned char *)malloc(TEST_PAGE_SIZE);
    assert(page2 != NULL);
    read_page(local_path, 2, page2); /* zero: writer1's gen-2 page not present */
    for (uint32_t i = 0; i < TEST_PAGE_SIZE; ++i) assert(page2[i] == 0);
    free(page2);

    /* The fsynced WAL next to the local image carries writer2's frames. */
    otsardb_sqlite_wal_frame_ref w2_frames[2];
    build_wal_file(wal_path, w2_pages, 2, w2_frames);
    assert(w2_frames[0].page_no == 4 && w2_frames[1].page_no == 5);

    /* Re-materialise-and-retry: converges to gen 3 with BOTH commits. */
    assert(otsardb_wal_publisher_retry_publish(&publisher,
                                             local_path,
                                             TEST_PAGE_SIZE,
                                             TEST_DB_PAGES,
                                             w2_frames,
                                             2) == 1);

    otsardb_head after_retry = read_head(store, TEST_ROOT);
    assert(after_retry.generation == 3);
    assert(strcmp(after_retry.commit_id, "w1") != 0);

    /* The retry commit chains onto writer1 (convergence, not overwrite). */
    char head_manifest_key[OTSARDB_KEY_MAX + 1];
    int key_len = snprintf(head_manifest_key,
                           sizeof(head_manifest_key),
                           "%s/commits/%s.json",
                           TEST_ROOT,
                           after_retry.commit_id);
    assert(key_len > 0 && (size_t)key_len < sizeof(head_manifest_key));
    otsardb_store_object head_manifest_object = {0};
    assert(otsardb_store_get(store, head_manifest_key, &head_manifest_object) == OTSARDB_STORE_OK);
    otsardb_commit_manifest head_manifest;
    assert(otsardb_commit_manifest_decode_json((const char *)head_manifest_object.data,
                                             head_manifest_object.size,
                                             &head_manifest));
    otsardb_store_release_object(store, &head_manifest_object);
    assert(head_manifest.generation == 3);
    assert(strcmp(head_manifest.parent_commit_id, "w1") == 0);
    assert(head_manifest.segment_count == 1);
    assert(head_manifest.segments[0].page_count == 2);
    assert(head_manifest.segments[0].pages[0] == 4);
    assert(head_manifest.segments[0].pages[1] == 5);
    otsardb_commit_manifest_free(&head_manifest);

    /* Recovery from zero local state yields BOTH writers' data. */
    assert(otsardb_recovery_restore_store(store, TEST_ROOT, recovered_path));
    assert_page_content(recovered_path, 1, page_marker(1)); /* writer1 */
    assert_page_content(recovered_path, 2, page_marker(2)); /* writer1 */
    assert_page_content(recovered_path, 3, page_marker(3)); /* genesis */
    assert_page_content(recovered_path, 4, page_marker(4)); /* writer2 retry */
    assert_page_content(recovered_path, 5, page_marker(5)); /* writer2 retry */

    /* The publisher now caches the retried HEAD. A repeat call finds the
     * remote HEAD unchanged (same parent) -> 0, nothing to do. */
    assert(publisher.current_generation == 3);
    assert(otsardb_wal_publisher_retry_publish(&publisher,
                                             local_path,
                                             TEST_PAGE_SIZE,
                                             TEST_DB_PAGES,
                                             w2_frames,
                                             2) == 0);
    assert(read_head(store, TEST_ROOT).generation == 3);

    otsardb_unlink(local_path);
    otsardb_unlink(wal_path);
    otsardb_unlink(recovered_path);
    otsardb_test_memory_store_destroy(store);

    /* ---------------- Scenario B: overlapping pages refused --------------- */
    const char *overlap_local = "remat-overlap-local.db";
    const char *overlap_wal = "remat-overlap-local.db-wal";
    otsardb_unlink(overlap_local);
    otsardb_unlink(overlap_wal);

    const uint32_t w2_overlap_pages[] = {2, 5}; /* page 2 collides with w1 */
    otsardb_wal_publisher overlap_publisher;
    otsardb_commit_result w1_overlap;
    otsardb_object_store *overlap_store =
        setup_two_writers(&overlap_publisher, w2_overlap_pages, 2, overlap_local, &w1_overlap);

    /* Caller-side classification: interleaved writer1 touched page 2 ->
     * genuine overlap; the precondition forbids calling retry_publish. */
    assert(chain_overlaps_pages(overlap_store, TEST_ROOT, 1, w2_overlap_pages, 2) == 1);

    otsardb_sqlite_wal_frame_ref overlap_frames[2];
    build_wal_file(overlap_wal, w2_overlap_pages, 2, overlap_frames);

    /* Defence in depth: even if a caller violates the precondition, the
     * retry refuses (-1) before re-materialising or publishing. */
    assert(otsardb_wal_publisher_retry_publish(&overlap_publisher,
                                             overlap_local,
                                             TEST_PAGE_SIZE,
                                             TEST_DB_PAGES,
                                             overlap_frames,
                                             2) == -1);

    /* HEAD stays at writer1's gen 2; the local image was not re-materialised
     * (page 2 is still zero from the gen-1 image, not writer1's marker). */
    otsardb_head after_refused = read_head(overlap_store, TEST_ROOT);
    assert(after_refused.generation == 2);
    assert(strcmp(after_refused.commit_id, "w1") == 0);
    unsigned char *refused_page2 = (unsigned char *)malloc(TEST_PAGE_SIZE);
    assert(refused_page2 != NULL);
    read_page(overlap_local, 2, refused_page2);
    for (uint32_t i = 0; i < TEST_PAGE_SIZE; ++i) assert(refused_page2[i] == 0);
    free(refused_page2);

    otsardb_unlink(overlap_local);
    otsardb_unlink(overlap_wal);
    otsardb_test_memory_store_destroy(overlap_store);

    puts("re-materialise-and-retry tests passed");
    return 0;
}
