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

/* deterministic regression test: multi-S3 promotion,
 * reconciliation / return-to-primary, split-brain drills
 * (sections 4/6/9).
 *
 * Same harness pattern as the 0072 replica test: the publisher drives a
 * synthetic SQLite WAL over a PRIMARY store (memory_store + fault_store)
 * and a REPLICA store, attached via otsardb_replica_attach (no S3, no
 * environment).
 *
 * Scenarios:
 * T1 promotion happy path - OTSARDB_S3_PROMOTE env gate; the replica
 * destination epoch is RAISED by CAS
 * (1 -> 2), FULL recovery runs from the
 * promoted store (a real database file is
 * materialised), and the promoting handle
 * adopts the raised epoch.
 * T2 promote refused while the old primary is active - a live writer
 * lease on the old primary (<root>/lease.json,
 * fresh Last-Modified) REFUSES the promotion
 * (offline/disaster-recovery operator
 * contract); once the lease provably
 * expires the same promotion succeeds.
 * T3 reconcile replays the gap - a failed dual publish (primary OK /
 * replica FAIL / primary ledger ABORTED) is
 * followed by a SESSION RESTART; the next
 * dual publish compares the primary HEAD
 * with the replica ledger, REPLAYS the gap
 * (re-publishes objects + flips the
 * ABORTED ledger COMMITTED on both
 * destinations) and then proceeds with the
 * new commit.
 * T4 split-brain drill (412) - two promotions that read the SAME epoch
 * view cannot both win: the first CAS raise
 * succeeds, the second If-Match fails with
 * PRECONDITION_FAILED -> CONFLICT. Plus the
 * If-None-Match create-on-absent branch.
 * T5 dual publish after promote - after the promotion, the OLD primary
 * session's dual publish is refused by the
 * epoch fence (SQLITE_BUSY, replica HEAD
 * does not advance) while the PROMOTED
 * session completes the same commit dual-
 * publishing to the old primary store and
 * flips the ABORTED ledger COMMITTED.
 */

#define TEST_PAGE_SIZE 4096u

static const char *WAL_PATH = "otsardb-promotion.wal";
static const char *RECOVERED_DB = "otsardb-promotion-recovered.db";
static const char *ROOT_T1 = "promotion-test/ok";
static const char *ROOT_T2 = "promotion-test/refused";
static const char *ROOT_T3 = "promotion-test/reconcile";
static const char *ROOT_T4 = "promotion-test/split-brain";
static const char *ROOT_T5 = "promotion-test/dual-after-promote";
static const char *DATABASE_ID = "promotion-dual-db";
static const char *CID1 = "w1111111122222222-00000001";
static const char *CID2 = "w1111111122222222-00000002";
static const char *CID3 = "w1111111122222222-00000003";

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

/* One WAL file, one generation, `commits` commits of 3 frames each (the
 * n_truncate of the LAST frame of each commit carries the commit's
 * database size: 3/6/9 pages). sync1_size covers only the first commit. */
static void build_synthetic_wal(int commits, size_t *out_sync1_size, size_t *out_full_size) {
    size_t frame_size = (size_t)OTSARDB_SQLITE_WAL_FRAME_HEADER_SIZE + TEST_PAGE_SIZE;
    size_t frames = (size_t)commits * 3u;
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

/* ---- store helpers ------------------------------------------------------ */

/* Byte-for-byte compare of every object under `root` of two stores, skipping
 * keys containing `skip_needle` (the per-commit ledgers differ in
 * destination/timestamp fields by design). Defined at the bottom of this
 * file; declared here because T3 uses it. */
static void assert_objects_identical_promotion(otsardb_object_store *a,
                                               otsardb_object_store *b,
                                               const char *root,
                                               const char *skip_needle);

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

static int count_cb(const char *key, void *ctx) {
    (void)key;
    ++*(int *)ctx;
    return 0;
}

static int count_prefix(otsardb_object_store *store, const char *prefix) {
    int count = 0;
    assert(otsardb_store_list(store, prefix, count_cb, &count) == OTSARDB_STORE_OK);
    return count;
}

static int count_manifests(otsardb_object_store *store, const char *root) {
    char prefix[OTSARDB_KEY_MAX + 1];
    int n = snprintf(prefix, sizeof(prefix), "%s/commits/", root);
    assert(n > 0 && (size_t)n < sizeof(prefix));
    return count_prefix(store, prefix);
}

static int ledger_state(otsardb_object_store *store,
                        const char *root,
                        const char *commit_id,
                        char *out_state,
                        size_t out_state_size,
                        uint64_t *out_generation,
                        uint64_t *out_parent_generation,
                        char *out_manifest_sha256,
                        size_t out_manifest_sha_size) {
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
    if (ok) {
        p = strstr(copy, "\"generation\":");
        if (!p) ok = 0;
        if (ok) {
            p += strlen("\"generation\":");
            while (*p == ' ' || *p == '\t') ++p;
            char *ep = NULL;
            unsigned long long g = strtoull(p, &ep, 10);
            if (ep == p) ok = 0;
            else *out_generation = (uint64_t)g;
        }
    }
    if (ok) {
        p = strstr(copy, "\"parent_generation\":");
        if (!p) ok = 0;
        if (ok) {
            p += strlen("\"parent_generation\":");
            while (*p == ' ' || *p == '\t') ++p;
            char *ep = NULL;
            unsigned long long g = strtoull(p, &ep, 10);
            if (ep == p) ok = 0;
            else *out_parent_generation = (uint64_t)g;
        }
    }
    if (ok) {
        p = strstr(copy, "\"manifest_sha256\":\"");
        if (!p) ok = 0;
        if (ok) {
            p += strlen("\"manifest_sha256\":\"");
            const char *end = strchr(p, '"');
            if (!end || (size_t)(end - p) >= out_manifest_sha_size) ok = 0;
            else {
                memcpy(out_manifest_sha256, p, (size_t)(end - p));
                out_manifest_sha256[end - p] = '\0';
            }
        }
    }

    otsardb_store_release_object(store, &object);
    free(copy);
    return ok;
}

/* ---- scenario harness ---------------------------------------------------- */

typedef struct dual_scenario {
    otsardb_object_store *primary_inner;
    otsardb_object_store *primary;
    otsardb_object_store *replica_inner;
    otsardb_object_store *replica;
    otsardb_wal_publisher publisher;
    const char *root;
    size_t sync1_size;
    size_t full_size;
} dual_scenario;

static dual_scenario scenario_setup(const char *root, int commits) {
    dual_scenario sc;
    memset(&sc, 0, sizeof(sc));
    sc.root = root;
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
                                    root,
                                    DATABASE_ID,
                                    OTSARDB_WRITE_STRATEGY_DEFAULT));
    assert(sc.publisher.strategy == OTSARDB_WRITE_STRATEGY_CAS);
    build_synthetic_wal(commits, &sc.sync1_size, &sc.full_size);
    return sc;
}

static void scenario_attach_replica(dual_scenario *sc) {
    assert(otsardb_replica_attach(&sc->publisher, sc->replica,
                                OTSARDB_WRITE_STRATEGY_CAS, 1));
}

static void scenario_teardown(dual_scenario *sc) {
    otsardb_wal_publisher_destroy(&sc->publisher);
    otsardb_test_fault_store_destroy(sc->primary);
    otsardb_test_memory_store_destroy(sc->primary_inner);
    otsardb_test_fault_store_destroy(sc->replica);
    otsardb_test_memory_store_destroy(sc->replica_inner);
    otsardb_unlink(WAL_PATH);
}

/* A scratch publisher over the OLD primary store with the replica store
 * attached: this is how a promoter obtains a handle over the to-be-promoted
 * destination (the session path uses otsardb_replica_open instead). */
typedef struct promoter {
    otsardb_wal_publisher scratch;
    otsardb_replica *handle;
} promoter;

static promoter promoter_create(otsardb_object_store *old_primary,
                               otsardb_object_store *promoted,
                               const char *root) {
    promoter p;
    memset(&p, 0, sizeof(p));
    assert(otsardb_wal_publisher_init(&p.scratch, old_primary, root,
                                    DATABASE_ID, OTSARDB_WRITE_STRATEGY_DEFAULT));
    assert(otsardb_replica_attach(&p.scratch, promoted,
                                OTSARDB_WRITE_STRATEGY_CAS, 1));
    p.handle = p.scratch.replica;
    assert(p.handle != NULL);
    return p;
}

static void promoter_destroy(promoter *p) {
    otsardb_wal_publisher_destroy(&p->scratch);
}

/* ---- T1: promotion happy path ------------------------------------------- */

static void test_promotion_happy_path(void) {
    set_env("OTSARDB_PUBLISH_RETRIES", "3");
    set_env("OTSARDB_PUBLISH_RETRY_BASE_MS", "0");
    set_env("OTSARDB_LEASE_TTL", "30");

    /* The OTSARDB_S3_PROMOTE gate: only "1" asks for promotion. */
    set_env("OTSARDB_S3_PROMOTE", "1");
    assert(otsardb_replica_promote_requested() == 1);
    set_env("OTSARDB_S3_PROMOTE", "0");
    assert(otsardb_replica_promote_requested() == 0);
    set_env("OTSARDB_S3_PROMOTE", "garbage");
    assert(otsardb_replica_promote_requested() == 0);
    set_env("OTSARDB_S3_PROMOTE", "");
    assert(otsardb_replica_promote_requested() == 0);

    set_env("OTSARDB_S3_PROMOTE", "1");
    dual_scenario sc = scenario_setup(ROOT_T1, 2);
    scenario_attach_replica(&sc);
    assert(drive_sync(sc.sync1_size, &sc.publisher) == 0);
    expect_head(sc.primary_inner, ROOT_T1, 1, CID1);
    expect_head(sc.replica_inner, ROOT_T1, 1, CID1);

    /* Both destinations carry the epoch record at 1. */
    {
        otsardb_replica_epoch_view view;
        otsardb_journal_result rc = OTSARDB_JOURNAL_ERROR;
        assert(otsardb_replica_epoch_read(sc.replica_inner, ROOT_T1, &view, &rc));
        assert(rc == OTSARDB_JOURNAL_OK && view.exists && view.epoch == 1);
        assert(view.etag[0] != '\0');
    }

    /* Promote the replica store: lease check (no lease on the old primary
     * — clean close) -> epoch CAS raise 1 -> 2 -> full recovery. */
    otsardb_unlink(RECOVERED_DB);
    promoter p = promoter_create(sc.primary, sc.replica, ROOT_T1);
    char err[512];
    err[0] = '\0';
    otsardb_journal_result rc = OTSARDB_JOURNAL_ERROR;
    assert(otsardb_replica_promote(p.handle, sc.primary, RECOVERED_DB,
                                 30, err, sizeof(err), &rc));
    assert(rc == OTSARDB_JOURNAL_OK);
    assert(otsardb_replica_epoch(p.handle) == 2);

    /* The destination record was raised (1 -> 2). */
    {
        otsardb_replica_epoch_view view;
        otsardb_journal_result vrc = OTSARDB_JOURNAL_ERROR;
        assert(otsardb_replica_epoch_read(sc.replica_inner, ROOT_T1, &view, &vrc));
        assert(vrc == OTSARDB_JOURNAL_OK && view.exists && view.epoch == 2);
    }

    /* FULL recovery from the promoted store materialised the local image:
     * a real SQLite-sized database file at the promoted HEAD (genesis =
     * 3 pages x page size). */
    {
        FILE *f = fopen(RECOVERED_DB, "rb");
        assert(f != NULL);
        assert(fseek(f, 0, SEEK_END) == 0);
        long size = ftell(f);
        fclose(f);
        assert(size == (long)(3u * TEST_PAGE_SIZE));
    }

    /* The old primary's epoch record is untouched (1) — its own session
     * fence lives on the promoted destination, which now refuses it. */
    {
        otsardb_replica_epoch_view view;
        otsardb_journal_result vrc = OTSARDB_JOURNAL_ERROR;
        assert(otsardb_replica_epoch_read(sc.primary_inner, ROOT_T1, &view, &vrc));
        assert(vrc == OTSARDB_JOURNAL_OK && view.exists && view.epoch == 1);
    }

    otsardb_unlink(RECOVERED_DB);
    promoter_destroy(&p);
    scenario_teardown(&sc);
}

/* ---- T2: promotion refused while the old primary is active -------------- */

static void test_promote_refused_while_primary_active(void) {
    set_env("OTSARDB_PUBLISH_RETRIES", "3");
    set_env("OTSARDB_PUBLISH_RETRY_BASE_MS", "0");
    set_env("OTSARDB_LEASE_TTL", "30");
    set_env("OTSARDB_S3_PROMOTE", "1");

    dual_scenario sc = scenario_setup(ROOT_T2, 2);
    scenario_attach_replica(&sc);
    assert(drive_sync(sc.sync1_size, &sc.publisher) == 0);

    /* The old primary holds a LIVE writer lease (fresh Last-Modified). */
    {
        const char *lease_json = "{\"owner\":\"old-primary-instance\",\"generation\":1}";
        char key[OTSARDB_KEY_MAX + 1];
        int n = snprintf(key, sizeof(key), "%s/lease.json", ROOT_T2);
        assert(n > 0 && (size_t)n < sizeof(key));
        assert(otsardb_store_put(sc.primary, key, lease_json, strlen(lease_json),
                               NULL, NULL) == OTSARDB_STORE_OK);
    }

    promoter p = promoter_create(sc.primary, sc.replica, ROOT_T2);
    char err[512];
    err[0] = '\0';
    otsardb_journal_result rc = OTSARDB_JOURNAL_OK;
    assert(!otsardb_replica_promote(p.handle, sc.primary, RECOVERED_DB,
                                  30, err, sizeof(err), &rc));
    assert(rc == OTSARDB_JOURNAL_CONFLICT);
    assert(strstr(err, "lease") != NULL);
    fprintf(stderr, "T2 refused as expected: %s\n", err);

    /* Nothing moved: the destination epoch is still 1 and no recovery ran. */
    {
        otsardb_replica_epoch_view view;
        otsardb_journal_result vrc = OTSARDB_JOURNAL_ERROR;
        assert(otsardb_replica_epoch_read(sc.replica_inner, ROOT_T2, &view, &vrc));
        assert(vrc == OTSARDB_JOURNAL_OK && view.exists && view.epoch == 1);
    }
    assert(otsardb_replica_epoch(p.handle) == 1);
    assert(!file_exists(RECOVERED_DB));

    /* The lease provably expires (age past TTL - skew): the same promotion
     * now succeeds (offline/disaster-recovery contract). */
    {
        char key[OTSARDB_KEY_MAX + 1];
        int n = snprintf(key, sizeof(key), "%s/lease.json", ROOT_T2);
        assert(n > 0 && (size_t)n < sizeof(key));
        otsardb_test_memory_store_age(sc.primary_inner, key, 60);
    }
    rc = OTSARDB_JOURNAL_ERROR;
    err[0] = '\0';
    assert(otsardb_replica_promote(p.handle, sc.primary, RECOVERED_DB,
                                 30, err, sizeof(err), &rc));
    assert(rc == OTSARDB_JOURNAL_OK);
    assert(otsardb_replica_epoch(p.handle) == 2);
    {
        otsardb_replica_epoch_view view;
        otsardb_journal_result vrc = OTSARDB_JOURNAL_ERROR;
        assert(otsardb_replica_epoch_read(sc.replica_inner, ROOT_T2, &view, &vrc));
        assert(vrc == OTSARDB_JOURNAL_OK && view.exists && view.epoch == 2);
    }

    otsardb_unlink(RECOVERED_DB);
    promoter_destroy(&p);
    scenario_teardown(&sc);
}

/* ---- T3: reconcile replays the gap + flips ABORTED -> COMMITTED --------- */

static void test_reconcile_replays_gap(void) {
    set_env("OTSARDB_PUBLISH_RETRIES", "3");
    set_env("OTSARDB_PUBLISH_RETRY_BASE_MS", "0");
    set_env("OTSARDB_LEASE_TTL", "30");
    set_env("OTSARDB_S3_PROMOTE", "");

    /* Session A: genesis dual-commits, then the CID2 dual publish FAILS
     * (replica unrecoverable) — primary OK / replica FAIL / primary ledger
     * ABORTED. */
    dual_scenario sc = scenario_setup(ROOT_T3, 3);
    scenario_attach_replica(&sc);
    assert(drive_sync(sc.sync1_size, &sc.publisher) == 0);
    expect_head(sc.primary_inner, ROOT_T3, 1, CID1);
    expect_head(sc.replica_inner, ROOT_T3, 1, CID1);

    otsardb_test_fault_store_clear(sc.primary);
    const size_t fail[] = {1, 2, 3};
    otsardb_test_fault_store_fail_puts(sc.replica, fail, 3, OTSARDB_STORE_ERROR, 0);
    assert(drive_sync(sc.full_size, &sc.publisher) == SQLITE_IOERR_WRITE);
    expect_head(sc.primary_inner, ROOT_T3, 2, CID2);
    expect_head(sc.replica_inner, ROOT_T3, 1, CID1);
    {
        char state[16];
        uint64_t gen = 0;
        uint64_t pgen = 0;
        char sha[65];
        assert(ledger_state(sc.primary_inner, ROOT_T3, CID2, state, sizeof(state),
                            &gen, &pgen, sha, sizeof(sha)));
        assert(strcmp(state, "ABORTED") == 0);
        assert(gen == 2 && pgen == 1);
    }
    assert(otsardb_test_fault_store_put_count(sc.primary) == 4);
    assert(otsardb_test_fault_store_put_count(sc.replica) == 3);

    /* Session A is DESTROYED (the pending retry never re-runs) and a NEW
     * session publishes a fresh commit. The replica ledger shows the gap:
     * replica HEAD CID1 vs primary HEAD CID2 — the next dual publish must
     * RECONCILE: replay CID2 (re-publish objects + flip the ledgers
     * COMMITTED on both destinations) and THEN proceed with CID3. */
    otsardb_wal_publisher_destroy(&sc.publisher);

    otsardb_wal_publisher publisher_b;
    assert(otsardb_wal_publisher_init(&publisher_b, sc.primary, ROOT_T3,
                                    DATABASE_ID, OTSARDB_WRITE_STRATEGY_DEFAULT));
    assert(otsardb_replica_attach(&publisher_b, sc.replica,
                                OTSARDB_WRITE_STRATEGY_CAS, 1));

    otsardb_test_fault_store_clear(sc.primary);
    otsardb_test_fault_store_clear(sc.replica);
    int b_rc = drive_sync(sc.full_size, &publisher_b);
    fprintf(stderr, "T3 session-B sync rc=%d puts_p=%zu puts_r=%zu\n",
            b_rc,
            otsardb_test_fault_store_put_count(sc.primary),
            otsardb_test_fault_store_put_count(sc.replica));
    assert(b_rc == 0);
    /* primary: CID3 publish (3) + metadata 412 (1, ensure_metadata's
     * If-None-Match attempt on the existing db.json) + CID2 replay ledgers
     * (2) + CID3 phase ledgers (2) = 8; replica: CID2 replay (segment,
     * manifest, PREPARED, HEAD, COMMITTED = 5) + CID3 phase (5) = 10. */
    assert(otsardb_test_fault_store_put_count(sc.primary) == 8);
    assert(otsardb_test_fault_store_put_count(sc.replica) == 10);

    /* Both destinations sit at CID3; three manifests per destination. */
    expect_head(sc.primary_inner, ROOT_T3, 3, CID3);
    expect_head(sc.replica_inner, ROOT_T3, 3, CID3);
    assert(count_manifests(sc.primary_inner, ROOT_T3) == 3);
    assert(count_manifests(sc.replica_inner, ROOT_T3) == 3);

    /* The gap was replayed and the ledgers FLIPPED COMMITTED on both
     * destinations (the primary's ABORTED record advanced). */
    for (int which = 0; which < 2; ++which) {
        otsardb_object_store *store = which == 0 ? sc.primary_inner : sc.replica_inner;
        for (int commit = 0; commit < 3; ++commit) {
            const char *cid = commit == 0 ? CID1 : (commit == 1 ? CID2 : CID3);
            char state[16];
            uint64_t gen = 0;
            uint64_t pgen = 0;
            char sha[65];
            assert(ledger_state(store, ROOT_T3, cid, state, sizeof(state),
                                &gen, &pgen, sha, sizeof(sha)));
            assert(strcmp(state, "COMMITTED") == 0);
            assert(gen == (uint64_t)(commit + 1));
            assert(pgen == (uint64_t)commit);
        }
    }

    /* The replayed CID2 ledger records the real manifest identity. */
    {
        char state[16];
        uint64_t gen = 0;
        uint64_t pgen = 0;
        char sha[65];
        assert(ledger_state(sc.primary_inner, ROOT_T3, CID2, state, sizeof(state),
                            &gen, &pgen, sha, sizeof(sha)));
        assert(strcmp(state, "COMMITTED") == 0);
        assert(strlen(sha) == 64);
    }

    /* Byte-for-byte: everything except the per-commit ledgers is identical
     * between the destinations (the replayed objects are the primary's). */
    assert_objects_identical_promotion(sc.primary_inner, sc.replica_inner,
                                       ROOT_T3, "/replica/");
    assert(publisher_b.current_generation == 3);
    assert(publisher_b.dual_pending == 0);

    otsardb_wal_publisher_destroy(&publisher_b);
    scenario_teardown(&sc);
}

/* ---- T4: split-brain drill (CAS epoch 412) ------------------------------ */

static void test_split_brain_promote_race(void) {
    set_env("OTSARDB_PUBLISH_RETRIES", "3");
    set_env("OTSARDB_PUBLISH_RETRY_BASE_MS", "0");
    set_env("OTSARDB_LEASE_TTL", "30");
    set_env("OTSARDB_S3_PROMOTE", "1");

    /* Fresh destination: the epoch record is absent — the raise creates it
     * with If-None-Match at epoch 1. */
    {
        otsardb_object_store *fresh = otsardb_test_memory_store_create();
        assert(fresh != NULL);
        otsardb_replica_epoch_view absent;
        otsardb_journal_result rc = OTSARDB_JOURNAL_ERROR;
        assert(otsardb_replica_epoch_read(fresh, ROOT_T4, &absent, &rc));
        assert(rc == OTSARDB_JOURNAL_OK && !absent.exists);
        uint64_t new_epoch = 0;
        assert(otsardb_replica_epoch_raise(fresh, ROOT_T4, DATABASE_ID, &absent,
                                         &new_epoch, &rc));
        assert(rc == OTSARDB_JOURNAL_OK && new_epoch == 1);
        otsardb_replica_epoch_view created;
        rc = OTSARDB_JOURNAL_ERROR;
        assert(otsardb_replica_epoch_read(fresh, ROOT_T4, &created, &rc));
        assert(rc == OTSARDB_JOURNAL_OK && created.exists && created.epoch == 1);
        otsardb_test_memory_store_destroy(fresh);
    }

    dual_scenario sc = scenario_setup(ROOT_T4, 2);
    scenario_attach_replica(&sc);
    assert(drive_sync(sc.sync1_size, &sc.publisher) == 0);

    /* Two promoters READ the same epoch view before either writes — the
     * race window of two concurrent promotions. */
    otsardb_replica_epoch_view view1;
    otsardb_replica_epoch_view view2;
    otsardb_journal_result rc1 = OTSARDB_JOURNAL_ERROR;
    otsardb_journal_result rc2 = OTSARDB_JOURNAL_ERROR;
    assert(otsardb_replica_epoch_read(sc.replica_inner, ROOT_T4, &view1, &rc1));
    assert(otsardb_replica_epoch_read(sc.replica_inner, ROOT_T4, &view2, &rc2));
    assert(rc1 == OTSARDB_JOURNAL_OK && rc2 == OTSARDB_JOURNAL_OK);
    assert(view1.exists && view1.epoch == 1);
    assert(view2.exists && view2.epoch == 1);
    assert(strcmp(view1.etag, view2.etag) == 0);

    /* Exactly one CAS raise wins; the loser's If-Match fails with 412 and
     * the raise reports CONFLICT — two concurrent promotes cannot both
     * win. */
    uint64_t winner_epoch = 0;
    otsardb_journal_result winner_rc = OTSARDB_JOURNAL_ERROR;
    assert(otsardb_replica_epoch_raise(sc.replica_inner, ROOT_T4, DATABASE_ID,
                                     &view1, &winner_epoch, &winner_rc));
    assert(winner_rc == OTSARDB_JOURNAL_OK && winner_epoch == 2);

    uint64_t loser_epoch = 0;
    otsardb_journal_result loser_rc = OTSARDB_JOURNAL_OK;
    assert(!otsardb_replica_epoch_raise(sc.replica_inner, ROOT_T4, DATABASE_ID,
                                      &view2, &loser_epoch, &loser_rc));
    assert(loser_rc == OTSARDB_JOURNAL_CONFLICT);

    /* The destination record reflects exactly one winner (epoch 2). */
    {
        otsardb_replica_epoch_view view;
        otsardb_journal_result rc = OTSARDB_JOURNAL_ERROR;
        assert(otsardb_replica_epoch_read(sc.replica_inner, ROOT_T4, &view, &rc));
        assert(rc == OTSARDB_JOURNAL_OK && view.exists && view.epoch == 2);
    }
    fprintf(stderr, "T4 split-brain: winner epoch=%llu loser rc=%d\n",
            (unsigned long long)winner_epoch, (int)loser_rc);

    /* A session holding the OLD epoch on the promoted destination is
     * fenced: its dual publish refuses (CONFLICT class). */
    assert(drive_sync(sc.full_size, &sc.publisher) == SQLITE_BUSY);
    expect_head(sc.primary_inner, ROOT_T4, 2, CID2);
    expect_head(sc.replica_inner, ROOT_T4, 1, CID1);

    scenario_teardown(&sc);
}

/* ---- T5: dual publish after promote ------------------------------------- */

static void test_dual_publish_after_promote(void) {
    set_env("OTSARDB_PUBLISH_RETRIES", "3");
    set_env("OTSARDB_PUBLISH_RETRY_BASE_MS", "0");
    set_env("OTSARDB_LEASE_TTL", "30");
    set_env("OTSARDB_S3_PROMOTE", "1");

    /* Session A (old primary P, replica R): genesis dual-commits. */
    dual_scenario sc = scenario_setup(ROOT_T5, 2);
    scenario_attach_replica(&sc);
    assert(drive_sync(sc.sync1_size, &sc.publisher) == 0);
    expect_head(sc.primary_inner, ROOT_T5, 1, CID1);
    expect_head(sc.replica_inner, ROOT_T5, 1, CID1);

    /* Operator promotes R (no lease on P -> epoch CAS 1 -> 2 -> recovery). */
    promoter p = promoter_create(sc.primary, sc.replica, ROOT_T5);
    char err[512];
    err[0] = '\0';
    otsardb_journal_result rc = OTSARDB_JOURNAL_ERROR;
    assert(otsardb_replica_promote(p.handle, sc.primary, RECOVERED_DB,
                                 30, err, sizeof(err), &rc));
    assert(rc == OTSARDB_JOURNAL_OK);

    /* Session A's next dual publish: the primary publish succeeds on P but
     * the replica phase is REFUSED by the epoch fence on the promoted
     * destination (2 != session's 1) — SQLITE_BUSY, commit not acked,
     * replica HEAD does not advance. */
    otsardb_test_fault_store_clear(sc.primary);
    otsardb_test_fault_store_clear(sc.replica);
    int a_rc = drive_sync(sc.full_size, &sc.publisher);
    fprintf(stderr, "T5 old-primary-session sync rc=%d puts_p=%zu puts_r=%zu\n",
            a_rc,
            otsardb_test_fault_store_put_count(sc.primary),
            otsardb_test_fault_store_put_count(sc.replica));
    assert(a_rc == SQLITE_BUSY);
    assert(otsardb_test_fault_store_put_count(sc.primary) == 4);
    assert(otsardb_test_fault_store_put_count(sc.replica) == 0);
    expect_head(sc.primary_inner, ROOT_T5, 2, CID2);
    expect_head(sc.replica_inner, ROOT_T5, 1, CID1);
    {
        char state[16];
        uint64_t gen = 0;
        uint64_t pgen = 0;
        char sha[65];
        assert(ledger_state(sc.primary_inner, ROOT_T5, CID2, state, sizeof(state),
                            &gen, &pgen, sha, sizeof(sha)));
        assert(strcmp(state, "ABORTED") == 0);
    }
    otsardb_wal_publisher_destroy(&sc.publisher);

    /* Session B: the PROMOTED store R is the new primary; the old primary
     * store P is its replica (its own epoch record is adopted, 1). The same
     * commit id completes: R's HEAD advances, P's HEAD is re-applied
     * (already CID2), and the ABORTED ledger on P flips COMMITTED — the
     * return-to-primary of the in-flight commit. */
    otsardb_wal_publisher publisher_b;
    assert(otsardb_wal_publisher_init(&publisher_b, sc.replica, ROOT_T5,
                                    DATABASE_ID, OTSARDB_WRITE_STRATEGY_DEFAULT));
    assert(otsardb_replica_attach(&publisher_b, sc.primary,
                                OTSARDB_WRITE_STRATEGY_CAS, 1));

    otsardb_test_fault_store_clear(sc.primary);
    otsardb_test_fault_store_clear(sc.replica);
    int b_rc = drive_sync(sc.full_size, &publisher_b);
    fprintf(stderr, "T5 promoted-session sync rc=%d puts_p=%zu puts_r=%zu\n",
            b_rc,
            otsardb_test_fault_store_put_count(sc.primary),
            otsardb_test_fault_store_put_count(sc.replica));
    assert(b_rc == 0);
    /* promoted store R: primary publish (3) + metadata 412 (1, the fresh
     * publisher's ensure_metadata attempt on the existing db.json) +
     * PREPARED/COMMITTED ledgers (2) = 6; old primary store P (now the
     * replica): segment, manifest, PREPARED (ABORTED->PREPARED), HEAD
     * re-apply, COMMITTED = 5. */
    assert(otsardb_test_fault_store_put_count(sc.primary) == 5);
    assert(otsardb_test_fault_store_put_count(sc.replica) == 6);

    expect_head(sc.primary_inner, ROOT_T5, 2, CID2);
    expect_head(sc.replica_inner, ROOT_T5, 2, CID2);
    assert(count_manifests(sc.primary_inner, ROOT_T5) == 2);
    assert(count_manifests(sc.replica_inner, ROOT_T5) == 2);
    {
        char state[16];
        uint64_t gen = 0;
        uint64_t pgen = 0;
        char sha[65];
        assert(ledger_state(sc.primary_inner, ROOT_T5, CID2, state, sizeof(state),
                            &gen, &pgen, sha, sizeof(sha)));
        assert(strcmp(state, "COMMITTED") == 0); /* ABORTED flipped */
        assert(gen == 2 && pgen == 1);
    }
    {
        char state[16];
        uint64_t gen = 0;
        uint64_t pgen = 0;
        char sha[65];
        assert(ledger_state(sc.replica_inner, ROOT_T5, CID2, state, sizeof(state),
                            &gen, &pgen, sha, sizeof(sha)));
        assert(strcmp(state, "COMMITTED") == 0);
    }
    assert(publisher_b.current_generation == 2);
    assert(publisher_b.dual_pending == 0);

    otsardb_wal_publisher_destroy(&publisher_b);
    otsardb_unlink(RECOVERED_DB);
    promoter_destroy(&p);
    scenario_teardown(&sc);
}

/* ---- shared byte-for-byte comparison (as in the 0072 test) --------------- */

typedef struct captured_object {
    char key[OTSARDB_KEY_MAX + 1];
    unsigned char *data;
    size_t size;
} captured_object;

typedef struct object_capture {
    char root[OTSARDB_KEY_MAX + 1];
    captured_object *objects;
    size_t count;
    size_t capacity;
} object_capture;

static int capture_cb(const char *key, void *ctx) {
    object_capture *capture = (object_capture *)ctx;
    if (capture->count == capture->capacity) {
        size_t next = capture->capacity ? capture->capacity * 2 : 16;
        captured_object *more = (captured_object *)realloc(capture->objects,
                                                           next * sizeof(*more));
        if (!more) return 1;
        capture->objects = more;
        capture->capacity = next;
    }
    int n = snprintf(capture->objects[capture->count].key,
                     sizeof(capture->objects[capture->count].key),
                     "%s%s",
                     capture->root,
                     key);
    assert(n > 0 && (size_t)n < sizeof(capture->objects[capture->count].key));
    capture->objects[capture->count].data = NULL;
    capture->objects[capture->count].size = 0;
    ++capture->count;
    return 0;
}

static void capture_objects(otsardb_object_store *store,
                            const char *root,
                            object_capture *capture) {
    memset(capture, 0, sizeof(*capture));
    snprintf(capture->root, sizeof(capture->root), "%s", root);
    assert(otsardb_store_list(store, root, capture_cb, capture) == OTSARDB_STORE_OK);
    for (size_t i = 0; i < capture->count; ++i) {
        otsardb_store_object object = {0};
        assert(otsardb_store_get(store, capture->objects[i].key, &object) == OTSARDB_STORE_OK);
        capture->objects[i].data = (unsigned char *)malloc(object.size ? object.size : 1);
        assert(capture->objects[i].data != NULL);
        if (object.size) memcpy(capture->objects[i].data, object.data, object.size);
        capture->objects[i].size = object.size;
        otsardb_store_release_object(store, &object);
    }
}

static void release_capture(object_capture *capture) {
    for (size_t i = 0; i < capture->count; ++i) free(capture->objects[i].data);
    free(capture->objects);
    memset(capture, 0, sizeof(*capture));
}

static void assert_objects_identical_promotion(otsardb_object_store *a,
                                               otsardb_object_store *b,
                                               const char *root,
                                               const char *skip_needle) {
    object_capture cap_a;
    object_capture cap_b;
    capture_objects(a, root, &cap_a);
    capture_objects(b, root, &cap_b);

    assert(cap_a.count == cap_b.count);
    for (size_t i = 0; i < cap_a.count; ++i) {
        const char *key = cap_a.objects[i].key;
        if (skip_needle && strstr(key, skip_needle)) continue;
        int found = 0;
        for (size_t j = 0; j < cap_b.count; ++j) {
            if (strcmp(cap_b.objects[j].key, key) == 0) {
                assert(cap_b.objects[j].size == cap_a.objects[i].size);
                assert(cap_a.objects[i].size == 0 ||
                       memcmp(cap_b.objects[j].data, cap_a.objects[i].data,
                              cap_a.objects[i].size) == 0);
                found = 1;
                break;
            }
        }
        assert(found);
    }
    release_capture(&cap_a);
    release_capture(&cap_b);
}

int main(void) {
    /* R5: mask replica credentials + pin operator knobs so a developer's
     * shell cannot change the scenarios. */
    set_env("OTSARDB_S3_REPLICA_ENDPOINT", "");
    set_env("OTSARDB_S3_REPLICA_REGION", "");
    set_env("OTSARDB_S3_REPLICA_ACCESS_KEY", "");
    set_env("OTSARDB_S3_REPLICA_SECRET_KEY", "");
    set_env("OTSARDB_S3_REPLICA_BUCKET", "");
    set_env("OTSARDB_S3_PROMOTE", "");
    set_env("OTSARDB_LEASE_TTL", "30");
    otsardb_unlink(RECOVERED_DB);

    test_promotion_happy_path();
    test_promote_refused_while_primary_active();
    test_reconcile_replays_gap();
    test_split_brain_promote_race();
    test_dual_publish_after_promote();
    puts("multi-S3 promotion: epoch CAS raise, lease-gated refusal, "
         "reconciliation replay (ABORTED->COMMITTED), split-brain 412, "
         "dual-publish-after-promote passed");
    return 0;
}
