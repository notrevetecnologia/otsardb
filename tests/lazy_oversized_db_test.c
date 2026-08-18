/* tests/lazy_oversized_db_test.c — regression test for ,
 * audit finding LH-3 (SUSPECTED -> CONFIRMED): `(uint32_t)` truncation of
 * `database_size_pages` for databases above 2^32 pages.
 *
 * Pre-fix, `otsardb_lazy_restore_store` accepted a HEAD manifest whose
 * `database_size_pages` was >= 2^32-1, then:
 * - the uint32 clamp in `otsardb_lazy_hydrate_range` truncated the page
 * count, so every page beyond the wrap was NEVER hydrated (silent page
 * loss); and
 * - when the truncated value was exactly UINT32_MAX, the
 * `page <= last_page` hydrate loops wrapped (`++page` -> 0) and ran
 * forever (hang).
 *
 * Fix: fail closed at restore — a database with >= 2^32-1 pages cannot be
 * served by the uint32-numbered lazy hydrator (>= 16 TiB at 4 KiB), so the
 * open refuses the image instead of hydrating a wrong or stuck one.
 *
 * This test crafts a valid hash-linked single-commit chain whose HEAD
 * manifest declares database_size_pages = UINT32_MAX (the exact hang case)
 * and = UINT32_MAX + 1 (the wrap/skip case) and asserts restore FAILS
 * CLOSED (returns 0, no hydrator left behind). A control chain with a
 * modest page count must still restore and hydrate.
 *
 * Deterministic: pure unit test over the memory store, no network, no
 * sleeps; the pre-fix code either returns success (assert fails) or hangs
 * (test watchdog / timeout catches it).
 *
 * Wiring (coordinator — , NOT applied here; CMakeLists.txt is
 * out of scope for this change set):
 * add_executable(otsardb_lazy_oversized_db_test
 * tests/lazy_oversized_db_test.c
 * tests/memory_store.c)
 * target_include_directories(otsardb_lazy_oversized_db_test
 * PRIVATE include src/core src/storage tests)
 * target_link_libraries(otsardb_lazy_oversized_db_test PRIVATE otsardb_core)
 * add_test(NAME otsardb_lazy_oversized_db
 * COMMAND otsardb_lazy_oversized_db_test)
 * set_tests_properties(otsardb_lazy_oversized_db PROPERTIES TIMEOUT 60)
 */

#include "commit_manifest.h"
#include "db_metadata.h"
#include "journal.h"
#include "lazy_hydration.h"
#include "memory_store.h"
#include "object_store.h"
#include "segment.h"
#include "wal_batch.h"

#include <assert.h>
#include <limits.h>
#include <stdint.h>
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

#define OD_PAGE_SIZE 512u
#define OD_SMALL_DB_PAGES 64u
#define OD_IMAGE "lazy-oversized-image.db"

static void cleanup_files(void) {
    otsardb_unlink(OD_IMAGE);
}

static void craft_chain(otsardb_object_store *store,
                        const char *root,
                        const char *db_id,
                        const char *commit_id,
                        const uint32_t *page_nos,
                        uint32_t page_count,
                        const unsigned char *page_bufs,
                        uint64_t manifest_db_size_pages) {
    /* The segment itself is built with the REAL (small) batch; only the
     * manifest's database_size_pages field is overridden — restore fails
     * closed on it before any hydrate, so the segment is never fetched. */
    char *batch = NULL;
    size_t batch_size = 0;
    assert(otsardb_wal_batch_encode(OD_PAGE_SIZE, OD_SMALL_DB_PAGES, page_nos,
                                  page_bufs, page_count, (unsigned char **)&batch,
                                  &batch_size));
    unsigned char *segment_data = NULL;
    size_t segment_size = 0;
    char segment_sha[OTSARDB_SEGMENT_SHA256_HEX_SIZE + 1];
    otsardb_segment_frame_info frames;
    memset(&frames, 0, sizeof(frames));
    assert(otsardb_segment_encode_with_frames(OTSARDB_SEGMENT_WAL_FRAMES,
                                            commit_id,
                                            batch,
                                            batch_size,
                                            &segment_data,
                                            &segment_size,
                                            segment_sha,
                                            1,
                                            &frames));
    free(batch);

    otsardb_db_metadata metadata;
    memset(&metadata, 0, sizeof(metadata));
    metadata.format_version = 1;
    metadata.page_size = OD_PAGE_SIZE;
    snprintf(metadata.database_id, sizeof(metadata.database_id), "%s", db_id);
    assert(otsardb_db_metadata_create(store, root, &metadata) == OTSARDB_METADATA_OK);

    otsardb_commit_manifest manifest;
    memset(&manifest, 0, sizeof(manifest));
    manifest.format_version = 4;
    manifest.generation = 1;
    manifest.parent_generation = 0;
    manifest.page_size = OD_PAGE_SIZE;
    manifest.database_size_pages = manifest_db_size_pages;
    snprintf(manifest.database_id, sizeof(manifest.database_id), "%s", db_id);
    snprintf(manifest.commit_id, sizeof(manifest.commit_id), "%s", commit_id);
    manifest.segment_count = 1;
    snprintf(manifest.segments[0].key,
             sizeof(manifest.segments[0].key),
             "segments/%s.otsseg", commit_id);
    snprintf(manifest.segments[0].sha256,
             sizeof(manifest.segments[0].sha256),
             "%s", segment_sha);
    manifest.segments[0].size = segment_size;
    manifest.segments[0].pages = (uint32_t *)page_nos;
    manifest.segments[0].page_count = page_count;
    manifest.segments[0].frame_offsets = frames.offsets;
    char *manifest_json = NULL;
    size_t manifest_size = 0;
    char manifest_sha[OTSARDB_MANIFEST_SHA256_HEX_SIZE + 1];
    assert(otsardb_commit_manifest_encode_json(&manifest,
                                             &manifest_json,
                                             &manifest_size,
                                             manifest_sha));

    char seg_key[OTSARDB_KEY_MAX + 1];
    snprintf(seg_key, sizeof(seg_key), "%s/%s", root, manifest.segments[0].key);
    otsardb_store_put_options plain = {0};
    assert(otsardb_store_put(store, seg_key, segment_data, segment_size,
                           &plain, NULL) == OTSARDB_STORE_OK);
    char man_key[OTSARDB_KEY_MAX + 1];
    snprintf(man_key, sizeof(man_key), "%s/commits/%s.json", root, commit_id);
    assert(otsardb_store_put(store, man_key, manifest_json, manifest_size,
                           &plain, NULL) == OTSARDB_STORE_OK);

    otsardb_head head;
    memset(&head, 0, sizeof(head));
    head.format_version = 1;
    head.generation = 1;
    snprintf(head.database_id, sizeof(head.database_id), "%s", db_id);
    snprintf(head.commit_id, sizeof(head.commit_id), "%s", commit_id);
    snprintf(head.commit_key, sizeof(head.commit_key), "%s", man_key);
    snprintf(head.commit_sha256, sizeof(head.commit_sha256), "%s", manifest_sha);
    char *head_json = NULL;
    size_t head_size = 0;
    assert(otsardb_head_encode_json(&head, &head_json, &head_size));
    char head_key[OTSARDB_KEY_MAX + 1];
    snprintf(head_key, sizeof(head_key), "%s/HEAD.json", root);
    assert(otsardb_store_put(store, head_key, head_json, head_size,
                           &plain, NULL) == OTSARDB_STORE_OK);

    otsardb_segment_frame_info_release(&frames);
    free(head_json);
    free(manifest_json);
    free(segment_data);
}

static otsardb_head read_head(otsardb_object_store *store, const char *root) {
    otsardb_head head;
    char *etag = NULL;
    assert(otsardb_journal_read_head(store, root, &head, &etag) == OTSARDB_JOURNAL_OK);
    free(etag);
    return head;
}

static void assert_restore_fails_closed(otsardb_object_store *store,
                                        const char *root,
                                        const char *db_id,
                                        const char *commit_id,
                                        uint64_t db_size_pages) {
    uint32_t pages[4] = {1, 2, 3, 4};
    unsigned char *page_bufs = (unsigned char *)malloc(4 * OD_PAGE_SIZE);
    assert(page_bufs);
    for (uint32_t i = 0; i < 4; ++i) {
        memset(page_bufs + (size_t)i * OD_PAGE_SIZE, (unsigned char)(i + 1u),
               OD_PAGE_SIZE);
    }
    craft_chain(store, root, db_id, commit_id, pages, 4, page_bufs,
                db_size_pages);
    free(page_bufs);

    otsardb_head head = read_head(store, root);
    otsardb_lazy_hydrator *h = NULL;
    assert(!otsardb_lazy_restore_store(store, root, OD_IMAGE, &head, &h));
    assert(h == NULL); /* no hydrator may be left behind */
    otsardb_unlink(OD_IMAGE);
}

int main(void) {
    cleanup_files();

    otsardb_object_store *store = otsardb_test_memory_store_create();
    assert(store != NULL);

    /* Control: a modest database still restores and hydrates. */
    {
        uint32_t pages[4] = {1, 2, 3, 4};
        unsigned char *page_bufs = (unsigned char *)malloc(4 * OD_PAGE_SIZE);
        assert(page_bufs);
        for (uint32_t i = 0; i < 4; ++i) {
            memset(page_bufs + (size_t)i * OD_PAGE_SIZE, (unsigned char)(i + 1u),
                   OD_PAGE_SIZE);
        }
        craft_chain(store, "lazy/oversized-control", "od-control-db",
                    "od-control-0001", pages, 4, page_bufs, OD_SMALL_DB_PAGES);
        free(page_bufs);
        otsardb_head head = read_head(store, "lazy/oversized-control");
        otsardb_lazy_hydrator *h = NULL;
        assert(otsardb_lazy_restore_store(store, "lazy/oversized-control",
                                        OD_IMAGE, &head, &h));
        assert(h != NULL);
        assert(otsardb_lazy_hydrate_range(h, 1, 4) == 1);
        otsardb_lazy_destroy(h);
        otsardb_unlink(OD_IMAGE);
    }

    /* The exact hang case: database_size_pages == UINT32_MAX (the uint32
     * clamp yields last_page == UINT32_MAX and the hydrate loop wraps).
     * Must fail closed at restore, never hang. */
    assert_restore_fails_closed(store, "lazy/oversized-max", "od-max-db",
                                "od-max-0001", (uint64_t)UINT32_MAX);

    /* The wrap/skip case: database_size_pages == 2^32 (truncates to 0 and
     * silently skips every page). Must fail closed. */
    assert_restore_fails_closed(store, "lazy/oversized-2p32", "od-2p32-db",
                                "od-2p32-0001", (uint64_t)UINT32_MAX + 1ull);

    otsardb_test_memory_store_destroy(store);
    cleanup_files();

    printf("lazy oversized database test: all assertions passed\n");
    return 0;
}
