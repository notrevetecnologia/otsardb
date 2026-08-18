/* checksum_rebase_test.c — regression tests for 
 * 0152, failure-mode audit H3 (rolling-checksum re-base silently dropped).
 *
 * refresh_head (wal_publisher.c) re-bases the rolling checksum on the freshly
 * read head manifest so the next commit can XOR-in its pages. Before 0152 a
 * failure to fetch/decode that manifest was silently swallowed: the sync
 * continued with checksum_known = 0 and published a checksum-less commit on a
 * chain that HAD carried checksums — silently degrading recovery's image gate
 * for that chain segment (audit H3/B5).
 *
 * After 0152:
 * - a re-base that cannot be applied (manifest GET/decode failure) on a
 * chain the publisher KNOWS was checksum-bearing FAILS the sync closed
 * (refresh_head returns SQLITE_IOERR_READ; no commit is published);
 * - a manifest that genuinely carries no checksum (the 0063/legacy
 * checksum-less path) keeps the documented degradation: warning logged,
 * sync continues checksum-less;
 * - on a chain whose checksum status is UNKNOWN (first refresh of a fresh
 * publisher) a failed re-base stays best-effort (warning only);
 * - an UNCHANGED head whose checksum is already held never re-fetches the
 * manifest, so a transient redundant GET cannot degrade a healthy chain.
 *
 * Deterministic, no network: the publisher is driven against the memory store
 * (wrapped in the fault store); the "newer head" is injected directly as a
 * valid manifest + HEAD (like the same-generation swap), and the
 * manifest GET is failed at a precise index (refresh_head does exactly two
 * GETs: HEAD then manifest).
 *
 * Wiring (coordinator): NOT applied here; CMakeLists.txt is
 * out of scope for this change set):
 * add_executable(otsardb_checksum_rebase_test
 * tests/checksum_rebase_test.c
 * tests/memory_store.c tests/fault_store.c)
 * target_include_directories(otsardb_checksum_rebase_test
 * PRIVATE include src/core src/crypto src/storage tests)
 * target_link_libraries(otsardb_checksum_rebase_test
 * PRIVATE otsardb_core)
 * add_test(NAME otsardb_checksum_rebase
 * COMMAND otsardb_checksum_rebase_test)
 * set_tests_properties(otsardb_checksum_rebase
 * PROPERTIES TIMEOUT 300)
 */
#include "commit_manifest.h"
#include "checksum.h"
#include "fault_store.h"
#include "journal.h"
#include "memory_store.h"
#include "object_store.h"
#include "sqlite3.h"
#include "sqlite_wal.h"
#include "wal_publisher.h"

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

#define TEST_PAGE_SIZE 4096u
#define DB_ID "0152-checksum-rebase-db"
#define INJECTED_COMMIT_ID "0152-injected-gen2"
#define INJECTED_CHECKSUM "0123456789abcdef"

static const char *WAL_PATH = "otsardb-0152-rebase.wal";

/* ---- synthetic WAL construction (SQLite WAL format) --------------------- */

static uint32_t get_be32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void put_be32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static void wal_checksum(const unsigned char *data, size_t size,
                         uint32_t *s0, uint32_t *s1) {
    for (size_t i = 0; i + 8 <= size; i += 8) {
        uint32_t w0 = (uint32_t)data[i] | ((uint32_t)data[i + 1] << 8) |
                      ((uint32_t)data[i + 2] << 16) | ((uint32_t)data[i + 3] << 24);
        uint32_t w1 = (uint32_t)data[i + 4] | ((uint32_t)data[i + 5] << 8) |
                      ((uint32_t)data[i + 6] << 16) | ((uint32_t)data[i + 7] << 24);
        *s0 += w0 + *s1;
        *s1 += w1 + *s0;
    }
}

static void build_wal_header(unsigned char *hdr, uint32_t page_size,
                             uint32_t salt1, uint32_t salt2) {
    memset(hdr, 0, OTSARDB_SQLITE_WAL_HEADER_SIZE);
    put_be32(hdr, 0x377f0682u);
    put_be32(hdr + 4, 3007000u);
    put_be32(hdr + 8, page_size);
    put_be32(hdr + 12, 1u);
    put_be32(hdr + 16, salt1);
    put_be32(hdr + 20, salt2);
    uint32_t s0 = 0;
    uint32_t s1 = 0;
    wal_checksum(hdr, OTSARDB_SQLITE_WAL_HEADER_SIZE - 8u, &s0, &s1);
    put_be32(hdr + 24, s0);
    put_be32(hdr + 28, s1);
}

static void build_wal_frame(unsigned char *frame, uint32_t page_no,
                            uint32_t n_truncate, const unsigned char *page,
                            uint32_t page_size, uint32_t salt1, uint32_t salt2,
                            uint32_t *s0, uint32_t *s1) {
    memset(frame, 0, OTSARDB_SQLITE_WAL_FRAME_HEADER_SIZE + page_size);
    put_be32(frame, page_no);
    put_be32(frame + 4, n_truncate);
    put_be32(frame + 8, salt1);
    put_be32(frame + 12, salt2);
    memcpy(frame + OTSARDB_SQLITE_WAL_FRAME_HEADER_SIZE, page, page_size);
    wal_checksum(frame, 8, s0, s1);
    wal_checksum(frame + OTSARDB_SQLITE_WAL_FRAME_HEADER_SIZE, page_size, s0, s1);
    put_be32(frame + 16, *s0);
    put_be32(frame + 20, *s1);
}

/* One WAL file, one WAL generation, two commits (genesis pages 1..3, child
 * pages 7..9). sync1 publishes the genesis only. */
static void build_synthetic_wal(uint32_t salt1, uint32_t salt2,
                                size_t *out_sync1_size, size_t *out_full_size) {
    size_t frame_size = (size_t)OTSARDB_SQLITE_WAL_FRAME_HEADER_SIZE + TEST_PAGE_SIZE;
    size_t full_size = (size_t)OTSARDB_SQLITE_WAL_HEADER_SIZE + 6u * frame_size;
    unsigned char *wal = (unsigned char *)malloc(full_size);
    assert(wal != NULL);
    build_wal_header(wal, TEST_PAGE_SIZE, salt1, salt2);

    unsigned char *page = (unsigned char *)malloc(TEST_PAGE_SIZE);
    assert(page != NULL);
    uint32_t s0 = 0;
    uint32_t s1 = 0;
    wal_checksum(wal, OTSARDB_SQLITE_WAL_HEADER_SIZE - 8u, &s0, &s1);
    for (uint32_t i = 0; i < 6u; ++i) {
        uint32_t page_no = i < 3u ? i + 1u : i + 4u;
        memset(page, (int)(page_no * 7u), TEST_PAGE_SIZE);
        uint32_t n_truncate = i == 2u ? 3u : (i == 5u ? 9u : 0u);
        build_wal_frame(wal + OTSARDB_SQLITE_WAL_HEADER_SIZE + i * frame_size,
                        page_no, n_truncate, page, TEST_PAGE_SIZE,
                        salt1, salt2, &s0, &s1);
    }
    free(page);

    FILE *f = fopen(WAL_PATH, "wb");
    assert(f != NULL);
    assert(fwrite(wal, 1, full_size, f) == full_size);
    assert(fclose(f) == 0);
    free(wal);

    *out_sync1_size = (size_t)OTSARDB_SQLITE_WAL_HEADER_SIZE + 3u * frame_size;
    *out_full_size = full_size;
}

typedef struct wal_file_reader {
    FILE *file;
} wal_file_reader;

static int wal_file_read_at(void *ctx, uint64_t offset, void *buffer, size_t size) {
    wal_file_reader *reader = (wal_file_reader *)ctx;
    if (!reader || !reader->file || offset > (uint64_t)LONG_MAX) return 0;
    if (fseek(reader->file, (long)offset, SEEK_SET) != 0) return 0;
    return fread(buffer, 1, size, reader->file) == size;
}

static int drive_sync(size_t wal_size, otsardb_wal_publisher *publisher) {
    wal_file_reader reader = {NULL};
    reader.file = fopen(WAL_PATH, "rb");
    assert(reader.file != NULL);
    int rc = otsardb_wal_publisher_sync_reader(WAL_PATH,
                                             (uint64_t)wal_size,
                                             wal_file_read_at,
                                             &reader,
                                             NULL,
                                             NULL,
                                             publisher);
    fclose(reader.file);
    return rc;
}

/* ---- store helpers ------------------------------------------------------ */

static uint64_t head_generation(otsardb_object_store *store, const char *root) {
    otsardb_head head;
    char *etag = NULL;
    otsardb_journal_result rc = otsardb_journal_read_head(store, root, &head, &etag);
    free(etag);
    assert(rc == OTSARDB_JOURNAL_OK);
    return head.generation;
}

static uint64_t parse_checksum(const char *hex) {
    uint64_t out = 0;
    assert(otsardb_checksum_parse(hex, &out));
    return out;
}

/* Inject a valid generation-2 head + manifest directly on the store. The
 * manifest references `parent_commit_id` / `parent_manifest_sha256` (gen1)
 * and carries `post_apply_checksum` when with_checksum != 0. Returns the
 * manifest's SHA-256 (as written into HEAD). */
static void inject_gen2(otsardb_object_store *store,
                        const char *root,
                        uint32_t head_format_version,
                        const char *parent_commit_id,
                        const char *parent_manifest_sha256,
                        int with_checksum) {
    otsardb_commit_manifest manifest;
    memset(&manifest, 0, sizeof(manifest));
    manifest.format_version = 4;
    manifest.generation = 2;
    manifest.parent_generation = 1;
    manifest.page_size = TEST_PAGE_SIZE;
    manifest.database_size_pages = 9;
    snprintf(manifest.database_id, sizeof(manifest.database_id), "%s", DB_ID);
    snprintf(manifest.commit_id, sizeof(manifest.commit_id), "%s", INJECTED_COMMIT_ID);
    snprintf(manifest.parent_commit_id, sizeof(manifest.parent_commit_id), "%s",
             parent_commit_id);
    snprintf(manifest.parent_manifest_sha256, sizeof(manifest.parent_manifest_sha256),
             "%s", parent_manifest_sha256);
    manifest.segment_count = 1;
    snprintf(manifest.segments[0].key, sizeof(manifest.segments[0].key),
             "segments/0152-injected");
    memset(manifest.segments[0].sha256, 'a', OTSARDB_MANIFEST_SHA256_HEX_SIZE);
    manifest.segments[0].sha256[OTSARDB_MANIFEST_SHA256_HEX_SIZE] = '\0';
    manifest.segments[0].size = TEST_PAGE_SIZE;
    if (with_checksum) {
        snprintf(manifest.post_apply_checksum, sizeof(manifest.post_apply_checksum),
                 "%s", INJECTED_CHECKSUM);
    }

    char *json = NULL;
    size_t json_size = 0;
    char sha[OTSARDB_MANIFEST_SHA256_HEX_SIZE + 1];
    assert(otsardb_commit_manifest_encode_json(&manifest, &json, &json_size, sha));

    char manifest_key[OTSARDB_KEY_MAX + 1];
    int n = snprintf(manifest_key, sizeof(manifest_key), "%s/commits/%s.json",
                     root, INJECTED_COMMIT_ID);
    assert(n > 0 && (size_t)n < sizeof(manifest_key));
    assert(otsardb_store_put(store, manifest_key, json, json_size, NULL, NULL) ==
           OTSARDB_STORE_OK);
    free(json);

    otsardb_head head;
    memset(&head, 0, sizeof(head));
    head.format_version = head_format_version;
    head.generation = 2;
    snprintf(head.database_id, sizeof(head.database_id), "%s", DB_ID);
    snprintf(head.commit_id, sizeof(head.commit_id), "%s", INJECTED_COMMIT_ID);
    snprintf(head.commit_key, sizeof(head.commit_key), "%s/commits/%s.json",
             root, INJECTED_COMMIT_ID);
    snprintf(head.commit_sha256, sizeof(head.commit_sha256), "%s", sha);

    char *head_json = NULL;
    size_t head_json_size = 0;
    assert(otsardb_head_encode_json(&head, &head_json, &head_json_size));
    char head_key[OTSARDB_KEY_MAX + 1];
    n = snprintf(head_key, sizeof(head_key), "%s/HEAD.json", root);
    assert(n > 0 && (size_t)n < sizeof(head_key));
    assert(otsardb_store_put(store, head_key, head_json, head_json_size, NULL, NULL) ==
           OTSARDB_STORE_OK);
    free(head_json);
}

static void expect_head_sha(const char *sha) {
    assert(sha && strlen(sha) == OTSARDB_MANIFEST_SHA256_HEX_SIZE);
}

/* H3-MAIN: a checksum-bearing chain whose head CHANGED, with the re-base
 * manifest GET failing, must fail the sync closed (SQLITE_IOERR_READ, no
 * publish). H3-CONTROL: same chain, manifest readable, the re-base applies
 * and the sync continues (SQLITE_OK, checksum re-armed). */
static void test_rebase_fail_closed_and_control(void) {
    printf("[H3] MAIN+CONTROL: ");
    otsardb_unlink(WAL_PATH);
    otsardb_object_store *inner = otsardb_test_memory_store_create();
    assert(inner != NULL);
    otsardb_object_store *fault = otsardb_test_fault_store_create(inner);
    assert(fault != NULL);

    otsardb_wal_publisher publisher;
    assert(otsardb_wal_publisher_init(&publisher, fault, "152/h3-a", DB_ID,
                                    OTSARDB_WRITE_STRATEGY_DEFAULT));
    assert(publisher.strategy == OTSARDB_WRITE_STRATEGY_CAS);

    size_t sync1_size = 0;
    size_t full_size = 0;
    build_synthetic_wal(0x11111111u, 0x22222222u, &sync1_size, &full_size);
    assert(drive_sync(sync1_size, &publisher) == 0);
    assert(head_generation(inner, "152/h3-a") == 1);
    assert(publisher.checksum_known == 1);
    expect_head_sha(publisher.current_manifest_sha256);

    /* The chain advances to an injected gen2 (checksum-bearing). */
    otsardb_head h1;
    char *etag = NULL;
    assert(otsardb_journal_read_head(inner, "152/h3-a", &h1, &etag) == OTSARDB_JOURNAL_OK);
    free(etag);
    inject_gen2(inner, "152/h3-a", h1.format_version,
                publisher.current_commit_id, publisher.current_manifest_sha256, 1);
    assert(head_generation(inner, "152/h3-a") == 2);

    /* H3-MAIN: refresh_head does exactly two GETs (HEAD then manifest);
     * failing GET #2 is the manifest fetch for the re-base. */
    size_t puts_before = otsardb_test_fault_store_put_count(fault);
    otsardb_test_fault_store_fail_get(fault, 2, OTSARDB_STORE_ERROR);
    int main_rc = otsardb_wal_publisher_refresh_head(&publisher);
    fprintf(stderr, "\n  H3-MAIN refresh_head rc=%d (SQLITE_IOERR_READ=%d)\n",
            main_rc, SQLITE_IOERR_READ);
    assert(main_rc == SQLITE_IOERR_READ);
    assert(publisher.checksum_known == 0);
    /* The sync did not publish anything: no PUTs beyond the genesis publish
     * and HEAD stays at the injected gen2. */
    assert(otsardb_test_fault_store_put_count(fault) == puts_before);
    assert(head_generation(inner, "152/h3-a") == 2);

    /* H3-CONTROL: manifest readable again — the SAME refresh succeeds, the
     * re-base is applied and the rolling checksum is re-armed. */
    otsardb_test_fault_store_clear(fault);
    int ctl_rc = otsardb_wal_publisher_refresh_head(&publisher);
    fprintf(stderr, "  H3-CONTROL refresh_head rc=%d checksum_known=%d\n",
            ctl_rc, publisher.checksum_known);
    assert(ctl_rc == SQLITE_OK);
    assert(publisher.checksum_known == 1);
    assert(publisher.state_checksum == parse_checksum(INJECTED_CHECKSUM));

    otsardb_test_fault_store_destroy(fault);
    otsardb_test_memory_store_destroy(inner);
    otsardb_unlink(WAL_PATH);
    puts("passed");
}

/* H3-DEGRADED: a checksum-bearing chain whose new head genuinely carries no
 * checksum (the 0063/legacy checksum-less path) is a documented degradation,
 * not a drop: warning logged, sync continues checksum-less (SQLITE_OK). */
static void test_checksumless_head_is_not_failed(void) {
    printf("[H3] DEGRADED: ");
    otsardb_unlink(WAL_PATH);
    otsardb_object_store *inner = otsardb_test_memory_store_create();
    assert(inner != NULL);
    otsardb_object_store *fault = otsardb_test_fault_store_create(inner);
    assert(fault != NULL);

    otsardb_wal_publisher publisher;
    assert(otsardb_wal_publisher_init(&publisher, fault, "152/h3-b", DB_ID,
                                    OTSARDB_WRITE_STRATEGY_DEFAULT));
    size_t sync1_size = 0;
    size_t full_size = 0;
    build_synthetic_wal(0x33333333u, 0x44444444u, &sync1_size, &full_size);
    assert(drive_sync(sync1_size, &publisher) == 0);
    assert(publisher.checksum_known == 1);

    otsardb_head h1;
    char *etag = NULL;
    assert(otsardb_journal_read_head(inner, "152/h3-b", &h1, &etag) == OTSARDB_JOURNAL_OK);
    free(etag);
    /* Injected gen2 WITHOUT a post_apply_checksum. */
    inject_gen2(inner, "152/h3-b", h1.format_version,
                publisher.current_commit_id, publisher.current_manifest_sha256, 0);

    int rc = otsardb_wal_publisher_refresh_head(&publisher);
    fprintf(stderr, "  rc=%d checksum_known=%d (SQLITE_OK=%d)\n",
            rc, publisher.checksum_known, SQLITE_OK);
    assert(rc == SQLITE_OK);
    assert(publisher.checksum_known == 0);

    otsardb_test_fault_store_destroy(fault);
    otsardb_test_memory_store_destroy(inner);
    otsardb_unlink(WAL_PATH);
    puts("passed");
}

/* H3-UNKNOWN: a fresh publisher (never synced, checksum status unknown) whose
 * first refresh cannot fetch the head manifest keeps the best-effort
 * behaviour — warning only, SQLITE_OK, checksum_known stays 0. */
static void test_unknown_chain_best_effort(void) {
    printf("[H3] UNKNOWN: ");
    otsardb_unlink(WAL_PATH);
    otsardb_object_store *inner = otsardb_test_memory_store_create();
    assert(inner != NULL);
    otsardb_object_store *fault = otsardb_test_fault_store_create(inner);
    assert(fault != NULL);

    /* Seed the chain (gen1) with one publisher. */
    otsardb_wal_publisher seeder;
    assert(otsardb_wal_publisher_init(&seeder, fault, "152/h3-c", DB_ID,
                                    OTSARDB_WRITE_STRATEGY_DEFAULT));
    size_t sync1_size = 0;
    size_t full_size = 0;
    build_synthetic_wal(0x55555555u, 0x66666666u, &sync1_size, &full_size);
    assert(drive_sync(sync1_size, &seeder) == 0);
    assert(head_generation(inner, "152/h3-c") == 1);

    /* A FRESH publisher over the same chain: first refresh, checksum status
     * unknown. Failing the manifest GET must stay best-effort. */
    otsardb_wal_publisher fresh;
    assert(otsardb_wal_publisher_init(&fresh, fault, "152/h3-c", DB_ID,
                                    OTSARDB_WRITE_STRATEGY_DEFAULT));
    otsardb_test_fault_store_fail_get(fault, 2, OTSARDB_STORE_ERROR);
    int rc = otsardb_wal_publisher_refresh_head(&fresh);
    fprintf(stderr, "  rc=%d checksum_known=%d (SQLITE_OK=%d)\n",
            rc, fresh.checksum_known, SQLITE_OK);
    assert(rc == SQLITE_OK);
    assert(fresh.checksum_known == 0);
    /* The head was still adopted (the failure only dropped the re-base). */
    assert(fresh.current_generation == 1);
    assert(head_generation(inner, "152/h3-c") == 1);

    otsardb_test_fault_store_destroy(fault);
    otsardb_test_memory_store_destroy(inner);
    otsardb_unlink(WAL_PATH);
    puts("passed");
}

/* H3-UNCHANGED: a re-refresh of the SAME checksum-bearing head must not
 * re-fetch the manifest at all (the redundant GET cannot degrade the base). */
static void test_unchanged_head_preserves_base(void) {
    printf("[H3] UNCHANGED: ");
    otsardb_unlink(WAL_PATH);
    otsardb_object_store *inner = otsardb_test_memory_store_create();
    assert(inner != NULL);
    otsardb_object_store *fault = otsardb_test_fault_store_create(inner);
    assert(fault != NULL);

    otsardb_wal_publisher publisher;
    assert(otsardb_wal_publisher_init(&publisher, fault, "152/h3-d", DB_ID,
                                    OTSARDB_WRITE_STRATEGY_DEFAULT));
    size_t sync1_size = 0;
    size_t full_size = 0;
    build_synthetic_wal(0x77777777u, 0x88888888u, &sync1_size, &full_size);
    assert(drive_sync(sync1_size, &publisher) == 0);
    assert(publisher.checksum_known == 1);
    uint64_t base = publisher.state_checksum;

    /* Second refresh on the UNCHANGED head: with 0152 the base is preserved
     * without re-fetching, so even a failing manifest GET cannot degrade it.
     * GET #2 (which would be the manifest) is armed to fail — the refresh
     * must still succeed and keep the base. */
    otsardb_test_fault_store_fail_get(fault, 2, OTSARDB_STORE_ERROR);
    int rc = otsardb_wal_publisher_refresh_head(&publisher);
    fprintf(stderr, "  rc=%d checksum_known=%d (SQLITE_OK=%d)\n",
            rc, publisher.checksum_known, SQLITE_OK);
    assert(rc == SQLITE_OK);
    assert(publisher.checksum_known == 1);
    assert(publisher.state_checksum == base);

    otsardb_test_fault_store_destroy(fault);
    otsardb_test_memory_store_destroy(inner);
    otsardb_unlink(WAL_PATH);
    puts("passed");
}

int main(void) {
    test_rebase_fail_closed_and_control();
    test_checksumless_head_is_not_failed();
    test_unknown_chain_best_effort();
    test_unchanged_head_preserves_base();
    puts("rolling-checksum re-base fail-closed + diagnostics (H3) passed");
    return 0;
}
