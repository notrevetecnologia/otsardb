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

/* deterministic regression test: bounded exponential-backoff
 * retry for transient publish failures (policy).
 *
 * The publish path (segment PUT, manifest PUT, HEAD CAS) re-attempts
 * TRANSPORT-class failures (OTSARDB_JOURNAL_ERROR, OTSARDB_JOURNAL_AMBIGUOUS)
 * with the SAME commit request; CONFLICT (the HEAD CAS precondition failure
 * — the fencing result), NOT_FOUND, INVALID and UNSUPPORTED are NEVER
 * retried; on budget exhaustion the sync fails with the original error.
 *
 * The harness drives the publisher with a SYNTHETIC WAL file (no MinIO, no
 * real SQLite session): sync 1 publishes a genesis commit (generation 1),
 * sync 2 publishes a child commit (generation 2) through the fault store.
 * For a child commit one publish attempt is exactly three PUTs:
 * PUT 1 = segment, PUT 2 = manifest, PUT 3 = HEAD.
 * The fault store counts PUTs per sync, so every scenario asserts the exact
 * number of store calls: "retries happen" is a deterministic put-count, and
 * "commits exactly once, HEAD advances once" is the manifest count + the
 * head generation.
 *
 * Scenarios:
 * T1 segment PUT transient ERROR x2 -> retried, success, one commit
 * T2 manifest PUT transient ERROR x2 -> retried, success, one commit
 * T3 HEAD CAS transient ERROR x2 -> retried, success, one commit
 * T4 HEAD CAS PRECONDITION_FAILED -> CONFLICT, NOT retried, fails
 * T5 retry budget exhausted -> sync fails, HEAD unchanged
 * T6 HEAD timeout AFTER delegate -> AMBIGUOUS resolved COMMITTED
 * by identity, success, no retry
 * T7 HEAD timeout BEFORE delegate x2 -> AMBIGUOUS retried, success
 */

#define TEST_PAGE_SIZE 4096u

static const char *WAL_PATH = "otsardb-publisher-retry.wal";
static const char *ROOT_T1 = "retry-test/seg";
static const char *ROOT_T2 = "retry-test/manifest";
static const char *ROOT_T3 = "retry-test/head";
static const char *ROOT_T4 = "retry-test/fence";
static const char *ROOT_T5 = "retry-test/exhaust";
static const char *ROOT_T6 = "retry-test/ambig-committed";
static const char *ROOT_T7 = "retry-test/ambig-retry";

static void set_env(const char *name, const char *value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

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
    put_be32(hdr, 0x377f0682u); /* OTSARDB_SQLITE_WAL_MAGIC_LE (private) */
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

/* One WAL file, one WAL generation, two commits:
 * commit 1 (genesis): frames 1-3, pages 1..3, db size 3;
 * commit 2 (child): frames 4-6, pages 7..9, db size 9.
 * sync 1 reads only the first commit (header + 3 frames); sync 2 reads the
 * full file and continues from the cursor. */
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
        uint32_t page_no = i < 3u ? i + 1u : i + 4u; /* 1,2,3,7,8,9 */
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

static void expect_head_generation(otsardb_object_store *store,
                                   const char *root,
                                   uint64_t expected) {
    otsardb_head head;
    char *etag = NULL;
    otsardb_journal_result rc = otsardb_journal_read_head(store, root, &head, &etag);
    free(etag);
    assert(rc == OTSARDB_JOURNAL_OK);
    assert(head.generation == expected);
}

static int count_cb(const char *key, void *ctx) {
    (void)key;
    ++*(int *)ctx;
    return 0;
}

/* Number of objects under "<root>/commits/" — one per committed generation.
 * "commits exactly once" == this stays at 2 (genesis + the retried commit)
 * and the head's own manifest is readable. */
static int count_manifests(otsardb_object_store *store, const char *root) {
    char prefix[OTSARDB_KEY_MAX + 1];
    int n = snprintf(prefix, sizeof(prefix), "%s/commits/", root);
    assert(n > 0 && (size_t)n < sizeof(prefix));
    int count = 0;
    assert(otsardb_store_list(store, prefix, count_cb, &count) == OTSARDB_STORE_OK);
    return count;
}

static void assert_head_manifest_readable(otsardb_object_store *store,
                                          const char *root) {
    otsardb_head head;
    char *etag = NULL;
    assert(otsardb_journal_read_head(store, root, &head, &etag) == OTSARDB_JOURNAL_OK);
    free(etag);
    otsardb_store_object object = {0};
    assert(otsardb_store_get(store, head.commit_key, &object) == OTSARDB_STORE_OK);
    otsardb_store_release_object(store, &object);
}

/* ---- scenario harness ---------------------------------------------------- */

typedef struct retry_scenario {
    otsardb_object_store *inner;
    otsardb_object_store *fault;
    otsardb_wal_publisher publisher;
    const char *root;
    size_t sync1_size;
    size_t full_size;
} retry_scenario;

/* Fresh store + publisher, genesis commit published cleanly (generation 1).
 * Returns the scenario with the fault store armed for sync 2. */
static retry_scenario scenario_setup(const char *root, const char *database_id) {
    retry_scenario sc;
    memset(&sc, 0, sizeof(sc));
    sc.root = root;
    otsardb_unlink(WAL_PATH);
    sc.inner = otsardb_test_memory_store_create();
    assert(sc.inner != NULL);
    sc.fault = otsardb_test_fault_store_create(sc.inner);
    assert(sc.fault != NULL);
    assert(otsardb_wal_publisher_init(&sc.publisher,
                                    sc.fault,
                                    root,
                                    database_id,
                                    OTSARDB_WRITE_STRATEGY_DEFAULT));
    assert(sc.publisher.strategy == OTSARDB_WRITE_STRATEGY_CAS);
    build_synthetic_wal(0x11111111u, 0x22222222u, &sc.sync1_size, &sc.full_size);

    assert(drive_sync(sc.sync1_size, &sc.publisher) == 0);
    expect_head_generation(sc.inner, root, 1);
    assert(sc.publisher.current_generation == 1);
    return sc;
}

static void scenario_teardown(retry_scenario *sc) {
    otsardb_test_fault_store_destroy(sc->fault);
    otsardb_test_memory_store_destroy(sc->inner);
    otsardb_unlink(WAL_PATH);
}

/* ---- scenarios ----------------------------------------------------------- */

/* T1: transient ERROR on the segment PUT of two consecutive attempts;
 * the third attempt succeeds. The retried commit is exactly one generation,
 * HEAD advances exactly once. */
static void test_segment_transient_retry(void) {
    set_env("OTSARDB_PUBLISH_RETRIES", "3");
    set_env("OTSARDB_PUBLISH_RETRY_BASE_MS", "0");
    retry_scenario sc = scenario_setup(ROOT_T1, "retry-seg");

    const size_t fail[] = {1, 2}; /* attempt 1 + 2 segment PUTs */
    otsardb_test_fault_store_fail_puts(sc.fault, fail, 2, OTSARDB_STORE_ERROR, 0);

    assert(drive_sync(sc.full_size, &sc.publisher) == 0);
    expect_head_generation(sc.inner, ROOT_T1, 2);
    assert(sc.publisher.current_generation == 2);
    /* 3 attempts: seg,manifest,HEAD = 3 PUTs on attempt 3 + 2 failed PUTs. */
    assert(otsardb_test_fault_store_put_count(sc.fault) == 5);
    assert(count_manifests(sc.inner, ROOT_T1) == 2);
    assert_head_manifest_readable(sc.inner, ROOT_T1);
    scenario_teardown(&sc);
}

/* T2: transient ERROR on the manifest PUT of two attempts; the retry leaves
 * no orphan manifest behind (the successful attempt's manifest is the only
 * one). */
static void test_manifest_transient_retry(void) {
    set_env("OTSARDB_PUBLISH_RETRIES", "3");
    set_env("OTSARDB_PUBLISH_RETRY_BASE_MS", "0");
    retry_scenario sc = scenario_setup(ROOT_T2, "retry-manifest");

    const size_t fail[] = {2, 5}; /* each attempt's manifest PUT */
    otsardb_test_fault_store_fail_puts(sc.fault, fail, 2, OTSARDB_STORE_ERROR, 0);

    assert(drive_sync(sc.full_size, &sc.publisher) == 0);
    expect_head_generation(sc.inner, ROOT_T2, 2);
    /* attempt1: seg=1 OK, manifest=2 FAIL; attempt2: seg=3 OK, manifest=4 OK,
     * HEAD=5 FAIL; attempt3: seg=6 OK, manifest=7 OK, HEAD=8 OK. */
    assert(otsardb_test_fault_store_put_count(sc.fault) == 8);
    assert(count_manifests(sc.inner, ROOT_T2) == 2);
    assert_head_manifest_readable(sc.inner, ROOT_T2);
    scenario_teardown(&sc);
}

/* T3: transient ERROR on the HEAD CAS of two attempts. The orphan segments
 * and manifest from the failed attempts are content-addressed: the retry
 * accepts the existing identical objects (GET+compare) and HEAD advances
 * exactly once. */
static void test_head_transient_retry(void) {
    set_env("OTSARDB_PUBLISH_RETRIES", "3");
    set_env("OTSARDB_PUBLISH_RETRY_BASE_MS", "0");
    retry_scenario sc = scenario_setup(ROOT_T3, "retry-head");

    const size_t fail[] = {3, 6}; /* each attempt's HEAD CAS */
    otsardb_test_fault_store_fail_puts(sc.fault, fail, 2, OTSARDB_STORE_ERROR, 0);

    assert(drive_sync(sc.full_size, &sc.publisher) == 0);
    expect_head_generation(sc.inner, ROOT_T3, 2);
    assert(otsardb_test_fault_store_put_count(sc.fault) == 9);
    assert(count_manifests(sc.inner, ROOT_T3) == 2);
    assert_head_manifest_readable(sc.inner, ROOT_T3);
    scenario_teardown(&sc);
}

/* T4: a HEAD CAS PRECONDITION_FAILED is the fencing result (CONFLICT) and is
 * NEVER retried: exactly one attempt runs and the sync fails immediately
 * with SQLITE_BUSY. */
static void test_head_precondition_not_retried(void) {
    set_env("OTSARDB_PUBLISH_RETRIES", "3");
    set_env("OTSARDB_PUBLISH_RETRY_BASE_MS", "0");
    retry_scenario sc = scenario_setup(ROOT_T4, "retry-fence");

    const size_t fail[] = {3};
    otsardb_test_fault_store_fail_puts(sc.fault, fail, 1,
                                     OTSARDB_STORE_PRECONDITION_FAILED, 0);

    int t4_rc = drive_sync(sc.full_size, &sc.publisher);
    fprintf(stderr, "T4 drive_sync rc=%d (SQLITE_BUSY=%d OK=%d) puts=%zu\n",
            t4_rc, SQLITE_BUSY, SQLITE_OK,
            otsardb_test_fault_store_put_count(sc.fault));
    assert(t4_rc == SQLITE_BUSY);
    expect_head_generation(sc.inner, ROOT_T4, 1);
    /* One attempt only: seg,manifest,HEAD = 3 PUTs, no retry. */
    assert(otsardb_test_fault_store_put_count(sc.fault) == 3);
    /* The manifest PUT succeeded before the failed HEAD CAS, so an orphan
     * commit-2 manifest exists — but HEAD never advanced (the commit is not
     * acknowledged; orphan objects are the GC's job). */
    assert(count_manifests(sc.inner, ROOT_T4) == 2);
    scenario_teardown(&sc);
}

/* T5: the retry budget is exhausted -> the sync fails with the original
 * error, HEAD never advances and no manifest is written (the durability
 * contract: the commit is not acknowledged). */
static void test_budget_exhausted_fails(void) {
    set_env("OTSARDB_PUBLISH_RETRIES", "2");
    set_env("OTSARDB_PUBLISH_RETRY_BASE_MS", "0");
    retry_scenario sc = scenario_setup(ROOT_T5, "retry-exhaust");

    const size_t fail[] = {1, 2}; /* both attempts' segment PUT */
    otsardb_test_fault_store_fail_puts(sc.fault, fail, 2, OTSARDB_STORE_ERROR, 0);

    assert(drive_sync(sc.full_size, &sc.publisher) == SQLITE_IOERR_WRITE);
    expect_head_generation(sc.inner, ROOT_T5, 1);
    assert(otsardb_test_fault_store_put_count(sc.fault) == 2);
    assert(count_manifests(sc.inner, ROOT_T5) == 1);
    scenario_teardown(&sc);
}

/* T6: a HEAD timeout that arrived AFTER the store committed is AMBIGUOUS;
 * the retry cycle resolves it by commit identity (COMMITTED) and reports
 * success with NO further attempt — no duplicate HEAD, no double-commit. */
static void test_ambiguous_resolved_committed(void) {
    set_env("OTSARDB_PUBLISH_RETRIES", "3");
    set_env("OTSARDB_PUBLISH_RETRY_BASE_MS", "0");
    retry_scenario sc = scenario_setup(ROOT_T6, "retry-ambig-ok");

    const size_t fail[] = {3};
    otsardb_test_fault_store_fail_puts(sc.fault, fail, 1,
                                     OTSARDB_STORE_TIMEOUT, 1);

    assert(drive_sync(sc.full_size, &sc.publisher) == 0);
    expect_head_generation(sc.inner, ROOT_T6, 2);
    /* One attempt (3 PUTs): the identity resolution needs no retry. */
    assert(otsardb_test_fault_store_put_count(sc.fault) == 3);
    assert(count_manifests(sc.inner, ROOT_T6) == 2);
    scenario_teardown(&sc);
}

/* T7: a HEAD timeout BEFORE the store committed is genuinely ambiguous
 * (resolve: NOT_COMMITTED); the cycle retries and the third attempt wins. */
static void test_ambiguous_not_committed_retried(void) {
    set_env("OTSARDB_PUBLISH_RETRIES", "3");
    set_env("OTSARDB_PUBLISH_RETRY_BASE_MS", "0");
    retry_scenario sc = scenario_setup(ROOT_T7, "retry-ambig-retry");

    const size_t fail[] = {3, 6}; /* each attempt's HEAD CAS timeout */
    otsardb_test_fault_store_fail_puts(sc.fault, fail, 2,
                                     OTSARDB_STORE_TIMEOUT, 0);

    assert(drive_sync(sc.full_size, &sc.publisher) == 0);
    expect_head_generation(sc.inner, ROOT_T7, 2);
    assert(otsardb_test_fault_store_put_count(sc.fault) == 9);
    assert(count_manifests(sc.inner, ROOT_T7) == 2);
    scenario_teardown(&sc);
}

int main(void) {
    test_segment_transient_retry();
    test_manifest_transient_retry();
    test_head_transient_retry();
    test_head_precondition_not_retried();
    test_budget_exhausted_fails();
    test_ambiguous_resolved_committed();
    test_ambiguous_not_committed_retried();
    puts("bounded publish retry (transient failures, identity resolution, fencing) passed");
    return 0;
}
