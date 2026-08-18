#include "fault_store.h"
#include "journal.h"
#include "memory_store.h"
#include "object_store.h"
#include "replica.h"
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

/* (REP-1): promotion must NEVER serve a PREPARED-only head
 * commit. The destination selection picks the newest COMMITTED ledger, but
 * the pre-0134 code then recovered from that destination's CURRENT HEAD,
 * which can be one commit ahead of the safe ledger (a crash between the
 * replica HEAD CAS and the COMMITTED ledger write leaves a PREPARED-only
 * head). Recovery validates the chain, not the ledger, so the promoted
 * database silently included a commit that was never acked on any
 * destination.
 *
 * Deterministic scenario (0077 harness: synthetic WAL over memory+fault
 * stores, dual publish):
 * R1 genesis dual-publish (CID1 COMMITTED on both destinations).
 * R2 the second dual publish fails ONLY on the replica's COMMITTED-ledger
 * PUT (fault #5): the replica HEAD advances to CID2 with its ledger
 * stuck PREPARED — the exact kill-between-HEAD-CAS-and-ledger state.
 * R3 promotion of the replica store is REFUSED (CONFLICT) with the
 * PREPARED-head diagnostic, BEFORE the epoch raise: nothing moved
 * (epoch still 1, no recovery file). The pre-0134 code promoted and
 * materialised the un-acked CID2 image.
 * R4 after the operator completes the ledger (PREPARED -> COMMITTED),
 * the SAME promotion succeeds: epoch raised 1->2 and the recovery
 * file materialises exactly the 6-page (2-commit) image at CID2.
 */

#define TEST_PAGE_SIZE 4096u

static const char *WAL_PATH = "otsardb-m0134.wal";
static const char *RECOVERED_DB = "otsardb-m0134-recovered.db";
static const char *ROOT = "m0134/promotion";
static const char *DATABASE_ID = "m0134-promotion-db";
static const char *CID1 = "w1111111122222222-00000001";
static const char *CID2 = "w1111111122222222-00000002";

static void set_env(const char *name, const char *value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

static int file_exists(const char *path) {
#if defined(_WIN32)
    return _access(path, 0) == 0;
#else
    return access(path, F_OK) == 0;
#endif
}

/* ---- synthetic WAL construction (SQLite WAL format, as in 0065/0072) ---- */

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

/* One WAL file, one generation, 2 commits of 3 frames each. */
static void build_synthetic_wal(size_t *out_sync1_size, size_t *out_full_size) {
    size_t frame_size = (size_t)OTSARDB_SQLITE_WAL_FRAME_HEADER_SIZE + TEST_PAGE_SIZE;
    size_t frames = 6u;
    size_t full_size = (size_t)OTSARDB_SQLITE_WAL_HEADER_SIZE + frames * frame_size;
    unsigned char *wal = (unsigned char *)malloc(full_size);
    assert(wal != NULL);
    memset(wal, 0, OTSARDB_SQLITE_WAL_HEADER_SIZE);
    put_be32(wal, 0x377f0682u);
    put_be32(wal + 4, 3007000u);
    put_be32(wal + 8, TEST_PAGE_SIZE);
    put_be32(wal + 12, 1u);
    put_be32(wal + 16, 0x11111111u);
    put_be32(wal + 20, 0x22222222u);
    uint32_t s0 = 0;
    uint32_t s1 = 0;
    wal_checksum(wal, OTSARDB_SQLITE_WAL_HEADER_SIZE - 8u, &s0, &s1);
    put_be32(wal + 24, s0);
    put_be32(wal + 28, s1);
    s0 = 0;
    s1 = 0;
    wal_checksum(wal, OTSARDB_SQLITE_WAL_HEADER_SIZE - 8u, &s0, &s1);

    unsigned char *page = (unsigned char *)malloc(TEST_PAGE_SIZE);
    assert(page != NULL);
    for (size_t i = 0; i < frames; ++i) {
        uint32_t commit = (uint32_t)(i / 3u) + 1u;
        uint32_t page_no = (uint32_t)i + 1u;
        uint32_t n_truncate = 0;
        if (i % 3u == 2u) n_truncate = commit * 3u;
        memset(page, (int)(page_no * 7u), TEST_PAGE_SIZE);
        build_wal_frame(wal + OTSARDB_SQLITE_WAL_HEADER_SIZE + i * frame_size,
                        page_no, n_truncate, page, TEST_PAGE_SIZE,
                        0x11111111u, 0x22222222u, &s0, &s1);
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

static void expect_head(otsardb_object_store *store,
                        const char *root,
                        uint64_t generation,
                        const char *commit_id) {
    otsardb_head head;
    char *etag = NULL;
    otsardb_journal_result rc = otsardb_journal_read_head(store, root, &head, &etag);
    free(etag);
    assert(rc == OTSARDB_JOURNAL_OK);
    assert(head.generation == generation);
    assert(strcmp(head.commit_id, commit_id) == 0);
}

static int ledger_state(otsardb_object_store *store,
                        const char *root,
                        const char *commit_id,
                        char *out_state,
                        size_t out_state_size) {
    char key[OTSARDB_KEY_MAX + 1];
    int n = snprintf(key, sizeof(key), "%s/replica/%s.json", root, commit_id);
    assert(n > 0 && (size_t)n < sizeof(key));

    otsardb_store_object object = {0};
    otsardb_store_result rc = otsardb_store_get(store, key, &object);
    if (rc != OTSARDB_STORE_OK) return 0;

    char *copy = (char *)malloc(object.size + 1);
    assert(copy != NULL);
    memcpy(copy, object.data, object.size);
    copy[object.size] = '\0';

    int ok = 1;
    const char *p = strstr(copy, "\"state\":\"");
    if (!p) ok = 0;
    if (ok) {
        p += strlen("\"state\":\"");
        const char *end = strchr(p, '"');
        if (!end || (size_t)(end - p) >= out_state_size) ok = 0;
        else {
            memcpy(out_state, p, (size_t)(end - p));
            out_state[end - p] = '\0';
        }
    }
    otsardb_store_release_object(store, &object);
    free(copy);
    return ok;
}

/* Operator repair: flip the destination's PREPARED ledger to COMMITTED. */
static void complete_ledger(otsardb_object_store *store,
                            const char *root,
                            const char *commit_id) {
    char key[OTSARDB_KEY_MAX + 1];
    int n = snprintf(key, sizeof(key), "%s/replica/%s.json", root, commit_id);
    assert(n > 0 && (size_t)n < sizeof(key));

    otsardb_store_object object = {0};
    assert(otsardb_store_get(store, key, &object) == OTSARDB_STORE_OK);
    assert(object.size > 0 && object.data != NULL);
    char *copy = (char *)malloc(object.size + 1);
    assert(copy != NULL);
    memcpy(copy, object.data, object.size);
    copy[object.size] = '\0';
    otsardb_store_release_object(store, &object);

    char *marker = strstr(copy, "\"state\":\"PREPARED\"");
    assert(marker != NULL);
    memcpy(marker, "\"state\":\"COMMITTED\"", strlen("\"state\":\"COMMITTED\""));
    assert(otsardb_store_put(store, key, copy, strlen(copy), NULL, NULL) ==
           OTSARDB_STORE_OK);
    free(copy);
}

typedef struct dual_scenario {
    otsardb_object_store *primary_inner;
    otsardb_object_store *primary;
    otsardb_object_store *replica_inner;
    otsardb_object_store *replica;
    otsardb_wal_publisher publisher;
    size_t sync1_size;
    size_t full_size;
} dual_scenario;

static dual_scenario scenario_setup(void) {
    dual_scenario sc;
    memset(&sc, 0, sizeof(sc));
    otsardb_unlink(WAL_PATH);
    sc.primary_inner = otsardb_test_memory_store_create();
    assert(sc.primary_inner != NULL);
    sc.primary = otsardb_test_fault_store_create(sc.primary_inner);
    assert(sc.primary != NULL);
    sc.replica_inner = otsardb_test_memory_store_create();
    assert(sc.replica_inner != NULL);
    sc.replica = otsardb_test_fault_store_create(sc.replica_inner);
    assert(sc.replica != NULL);
    assert(otsardb_wal_publisher_init(&sc.publisher,
                                    sc.primary,
                                    ROOT,
                                    DATABASE_ID,
                                    OTSARDB_WRITE_STRATEGY_DEFAULT));
    assert(sc.publisher.strategy == OTSARDB_WRITE_STRATEGY_CAS);
    build_synthetic_wal(&sc.sync1_size, &sc.full_size);
    return sc;
}

static void scenario_teardown(dual_scenario *sc) {
    otsardb_wal_publisher_destroy(&sc->publisher);
    otsardb_test_fault_store_destroy(sc->primary);
    otsardb_test_memory_store_destroy(sc->primary_inner);
    otsardb_test_fault_store_destroy(sc->replica);
    otsardb_test_memory_store_destroy(sc->replica_inner);
    otsardb_unlink(WAL_PATH);
}

static void test_promotion_refuses_prepared_only_head(void) {
    printf("[REP-1] ");
    /* One attempt per store call: the faulted COMMITTED-ledger PUT must be
     * TERMINAL (a 0065 retry would re-attempt the write and complete the
     * commit, which is correct publisher behavior — the stuck state is the
     * crash-before-retry window this test models). */
    set_env("OTSARDB_PUBLISH_RETRIES", "1");
    set_env("OTSARDB_PUBLISH_RETRY_BASE_MS", "0");
    set_env("OTSARDB_LEASE_TTL", "30");
    set_env("OTSARDB_S3_PROMOTE", "1");

    dual_scenario sc = scenario_setup();

    /* R1: genesis dual-publish — CID1 COMMITTED on both destinations. */
    assert(otsardb_replica_attach(&sc.publisher, sc.replica,
                                OTSARDB_WRITE_STRATEGY_CAS, 1));
    assert(drive_sync(sc.sync1_size, &sc.publisher) == 0);
    expect_head(sc.primary_inner, ROOT, 1, CID1);
    expect_head(sc.replica_inner, ROOT, 1, CID1);

    /* R2: the second dual publish fails ONLY on the replica's COMMITTED-ledger
     * PUT (replica phase PUT #5). The replica HEAD advances to CID2 with its
     * ledger stuck PREPARED — the kill-between-HEAD-CAS-and-ledger state. */
    otsardb_test_fault_store_clear(sc.primary);
    otsardb_test_fault_store_clear(sc.replica);
    {
        const size_t fail[] = {5};
        otsardb_test_fault_store_fail_puts(sc.replica, fail, 1,
                                           OTSARDB_STORE_ERROR, 0);
    }
    int fail_rc = drive_sync(sc.full_size, &sc.publisher);
    fprintf(stderr, "REP-1 fail-sync rc=%d replica_puts=%zu primary_puts=%zu\n",
            fail_rc,
            otsardb_test_fault_store_put_count(sc.replica),
            otsardb_test_fault_store_put_count(sc.primary));
    assert(fail_rc == SQLITE_IOERR_WRITE);
    expect_head(sc.primary_inner, ROOT, 2, CID2);
    expect_head(sc.replica_inner, ROOT, 2, CID2);
    {
        char state[16];
        assert(ledger_state(sc.replica_inner, ROOT, CID2, state, sizeof(state)));
        assert(strcmp(state, "PREPARED") == 0);
    }
    otsardb_test_fault_store_clear(sc.replica);

    /* Promoter handle: the old primary store with the replica attached (the
     * session path opens the to-be-promoted destination as a destination of
     * a handle rooted at the old primary). */
    otsardb_wal_publisher scratch;
    assert(otsardb_wal_publisher_init(&scratch, sc.primary, ROOT, DATABASE_ID,
                                    OTSARDB_WRITE_STRATEGY_DEFAULT));
    assert(otsardb_replica_attach(&scratch, sc.replica,
                                OTSARDB_WRITE_STRATEGY_CAS, 1));
    otsardb_replica *handle = scratch.replica;
    assert(handle != NULL);

    /* R3: promotion is REFUSED — the destination's HEAD (gen 2) is ahead of
     * its newest COMMITTED ledger (gen 1). Nothing moved: epoch still 1, no
     * recovery file, the stuck ledger untouched. */
    otsardb_unlink(RECOVERED_DB);
    char err[512];
    err[0] = '\0';
    otsardb_journal_result rc = OTSARDB_JOURNAL_OK;
    assert(!otsardb_replica_promote(handle, sc.primary, RECOVERED_DB,
                                   30, err, sizeof(err), &rc));
    fprintf(stderr, "REP-1 refused rc=%d err=%s\n", (int)rc, err);
    assert(rc == OTSARDB_JOURNAL_CONFLICT);
    assert(strstr(err, "PREPARED") != NULL);
    assert(!file_exists(RECOVERED_DB));
    {
        otsardb_replica_epoch_view view;
        otsardb_journal_result vrc = OTSARDB_JOURNAL_ERROR;
        assert(otsardb_replica_epoch_read(sc.replica_inner, ROOT, &view, &vrc));
        assert(vrc == OTSARDB_JOURNAL_OK && view.exists && view.epoch == 1);
    }
    {
        char state[16];
        assert(ledger_state(sc.replica_inner, ROOT, CID2, state, sizeof(state)));
        assert(strcmp(state, "PREPARED") == 0);
    }

    /* R4: the operator completes the ledger; the SAME promotion now
     * succeeds — epoch raised 1->2, recovery materialises the 6-page image
     * of the fully-acked CID2 state. */
    complete_ledger(sc.replica_inner, ROOT, CID2);
    {
        char state[16];
        assert(ledger_state(sc.replica_inner, ROOT, CID2, state, sizeof(state)));
        assert(strcmp(state, "COMMITTED") == 0);
    }
    rc = OTSARDB_JOURNAL_ERROR;
    err[0] = '\0';
    assert(otsardb_replica_promote(handle, sc.primary, RECOVERED_DB,
                                   30, err, sizeof(err), &rc));
    assert(rc == OTSARDB_JOURNAL_OK);
    assert(otsardb_replica_epoch(handle) == 2);
    {
        otsardb_replica_epoch_view view;
        otsardb_journal_result vrc = OTSARDB_JOURNAL_ERROR;
        assert(otsardb_replica_epoch_read(sc.replica_inner, ROOT, &view, &vrc));
        assert(vrc == OTSARDB_JOURNAL_OK && view.exists && view.epoch == 2);
    }
    {
        FILE *f = fopen(RECOVERED_DB, "rb");
        assert(f != NULL);
        assert(fseek(f, 0, SEEK_END) == 0);
        long size = ftell(f);
        fclose(f);
        assert(size == (long)(6u * TEST_PAGE_SIZE));
    }

    otsardb_unlink(RECOVERED_DB);
    otsardb_wal_publisher_destroy(&scratch);
    scenario_teardown(&sc);
    puts("passed");
}

int main(void) {
    set_env("OTSARDB_S3_REPLICA_ENDPOINT", "");
    set_env("OTSARDB_S3_REPLICA_REGION", "");
    set_env("OTSARDB_S3_REPLICA_ACCESS_KEY", "");
    set_env("OTSARDB_S3_REPLICA_SECRET_KEY", "");
    set_env("OTSARDB_S3_REPLICA_BUCKET", "");
    set_env("OTSARDB_S3_PROMOTE", "");

    test_promotion_refuses_prepared_only_head();
    puts("REP-1: promotion refuses a PREPARED-only head "
         "(COMMITTED-ledger selection) and succeeds after the ledger "
         "completes passed");
    return 0;
}
