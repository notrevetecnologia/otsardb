#include "fault_store.h"
#include "journal.h"
#include "lease.h"
#include "memory_store.h"
#include "object_store.h"
#include "recovery.h"
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
#define otsardb_access _access
#else
#include <unistd.h>
#define otsardb_unlink unlink
#define otsardb_access access
#endif

/* deterministic regression test: multi-S3 READ-REPLICA
 * sessions (0069 "read-replica" decision point).
 *
 * A READER session opens the database reading objects from the REPLICA
 * destination: same database_root, same OtsarDB format, OTSARDB_S3_REPLICA_*
 * environment (here: the attached replica store), READ-ONLY — no lease, no
 * publish. The session's read path is recovery/restore over the replica
 * store; this suite exercises that path with the same memory_store +
 * fault_store pattern as the 0072/0077 suites (no S3, no environment).
 *
 * Scenarios:
 * T1 read-replica env gate - OTSARDB_S3_READ_FROM_REPLICA=1 is the strict
 * gate (absent/0/garbage -> 0, exactly "1"
 * -> 1), independent of the promotion gate.
 * T2 reader sees COMMITTED - after a dual genesis+child publish the
 * reader path (full recovery over the REPLICA
 * store) sees the same COMMITTED state as the
 * primary: head generation 2 / CID2, a real
 * database file materialised at the head
 * (6 pages x page size), the replica ledger
 * COMMITTED, and the reader performed ZERO
 * writes (identical key sets before/after; no
 * lease.json, no new replica/ objects).
 * T3 reader never publishes - a publisher over the replica store with the
 * reader's permanently-closed lease fence (the
 * read-replica session never claims a lease;
 * installs exactly this fence)
 * refuses every publish with SQLITE_BUSY and
 * performs ZERO store puts.
 * T4 reader works while the primary session holds the lease - the lease
 * lives on the PRIMARY store; the reader path
 * over the replica store never touches it:
 * recovery succeeds with a live lease claimed
 * on the primary, and no lease object exists
 * on the replica store.
 * T5 replica behind -> reader sees the older COMMITTED commit - a failed
 * dual publish (primary OK / replica FAIL,
 * primary ledger ABORTED) leaves the replica at
 * its last fully-COMMITTED state (CID1); the
 * reader sees exactly that state (head CID1,
 * file at 3 pages), never the in-flight CID2.
 * T6 reconciliation then reader sees the new commit - after the 0077
 * reconciliation replays the gap and completes
 * the next dual publish (both heads CID3, all
 * ledgers COMMITTED), a fresh reader sees CID3
 * (generation 3, file at 9 pages).
 *
 * Consistency model: the reader sees the
 * newest commit that is COMMITTED on the replica, which is always a valid
 * committed state, never partial — the replica HEAD advances only after
 * every object of a commit is durable there, so an in-flight dual publish
 * leaves the replica at its previous HEAD and in-flight objects are
 * unreachable from it.
 */

#define TEST_PAGE_SIZE 4096u

static const char *WAL_PATH = "otsardb-read-replica.wal";
#define RECOVERED_DB "otsardb-read-replica-recovered.db"
static const char *ROOT_T2 = "read-replica/committed";
static const char *ROOT_T3 = "read-replica/no-publish";
static const char *ROOT_T4 = "read-replica/lease-held";
static const char *ROOT_T5 = "read-replica/behind";
static const char *ROOT_T6 = "read-replica/after-reconcile";
static const char *DATABASE_ID = "read-replica-db";
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

static void clear_env(const char *name) {
#if defined(_WIN32)
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

/* ---- synthetic WAL construction (SQLite WAL format, as in 0072/0077) ---- */

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
 * n_truncate of the LAST frame of each commit carries the commit's database
 * size: 3/6/9 pages). sync1_size covers only the first commit. */
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

/* Capture the sorted key set of a store under `prefix`. */
typedef struct key_set {
    char **keys;
    size_t count;
    size_t capacity;
} key_set;

static int key_set_cb(const char *key, void *ctx) {
    key_set *set = (key_set *)ctx;
    if (set->count == set->capacity) {
        size_t next = set->capacity ? set->capacity * 2 : 16;
        char **more = (char **)realloc(set->keys, next * sizeof(*more));
        if (!more) return 1;
        set->keys = more;
        set->capacity = next;
    }
    set->keys[set->count] = (char *)malloc(strlen(key) + 1);
    assert(set->keys[set->count] != NULL);
    strcpy(set->keys[set->count], key);
    ++set->count;
    return 0;
}

static void capture_keys(otsardb_object_store *store, const char *prefix, key_set *set) {
    memset(set, 0, sizeof(*set));
    assert(otsardb_store_list(store, prefix, key_set_cb, set) == OTSARDB_STORE_OK);
    for (size_t i = 1; i < set->count; ++i) {
        for (size_t j = i; j > 0 && strcmp(set->keys[j - 1], set->keys[j]) > 0; --j) {
            char *tmp = set->keys[j];
            set->keys[j] = set->keys[j - 1];
            set->keys[j - 1] = tmp;
        }
    }
}

static void release_keys(key_set *set) {
    for (size_t i = 0; i < set->count; ++i) free(set->keys[i]);
    free(set->keys);
    memset(set, 0, sizeof(*set));
}

static void assert_key_sets_identical(const key_set *a, const key_set *b) {
    assert(a->count == b->count);
    for (size_t i = 0; i < a->count; ++i) {
        assert(strcmp(a->keys[i], b->keys[i]) == 0);
    }
}

static int ledger_state(otsardb_object_store *store,
                        const char *root,
                        const char *commit_id,
                        char *out_state,
                        size_t out_state_size,
                        uint64_t *out_generation) {
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
    const char *state_p = strstr(copy, "\"state\":\"");
    if (!state_p) {
        ok = 0;
    } else {
        state_p += strlen("\"state\":\"");
        const char *end = strchr(state_p, '"');
        if (!end || (size_t)(end - state_p) >= out_state_size) ok = 0;
        else {
            memcpy(out_state, state_p, (size_t)(end - state_p));
            out_state[end - state_p] = '\0';
        }
    }
    if (ok) {
        const char *p = strstr(copy, "\"generation\":");
        if (p) {
            p += strlen("\"generation\":");
            while (*p == ' ' || *p == '\t') ++p;
            char *ep = NULL;
            unsigned long long g = strtoull(p, &ep, 10);
            if (ep != p) *out_generation = (uint64_t)g;
            else ok = 0;
        } else ok = 0;
    }
    otsardb_store_release_object(store, &object);
    free(copy);
    return ok;
}

static int file_size_of(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    assert(fseek(f, 0, SEEK_END) == 0);
    long size = ftell(f);
    fclose(f);
    return size >= 0 ? (int)size : -1;
}

/* ---- scenario harness (0072/0077 pattern) ------------------------------- */

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
    otsardb_unlink(RECOVERED_DB);
    otsardb_unlink(RECOVERED_DB "-wal");
    otsardb_unlink(RECOVERED_DB "-shm");
}

/* The reader's read path: full recovery over the REPLICA
 * store into a disposable local image — exactly what the read-replica
 * session does with its store (lazy hydration uses the same store; this
 * suite pins the materialised state). */
static int reader_restore(otsardb_object_store *replica_store, const char *root) {
    otsardb_unlink(RECOVERED_DB);
    otsardb_unlink(RECOVERED_DB "-wal");
    otsardb_unlink(RECOVERED_DB "-shm");
    return otsardb_recovery_restore_store(replica_store, root, RECOVERED_DB) ? 1 : 0;
}

/* The reader's publish fence: the read-replica session
 * never claims a lease, so session_lease_fence sees a null lease and is
 * permanently closed — this is that predicate. */
static int reader_closed_fence(void *ctx) {
    (void)ctx;
    return 0;
}

/* ---- T1: read-replica env gate ------------------------------------------- */

static void test_read_gate(void) {
    clear_env("OTSARDB_S3_READ_FROM_REPLICA");
    clear_env("OTSARDB_S3_PROMOTE");
    assert(otsardb_replica_read_requested() == 0);
    assert(otsardb_replica_promote_requested() == 0);

    set_env("OTSARDB_S3_READ_FROM_REPLICA", "0");
    assert(otsardb_replica_read_requested() == 0);
    set_env("OTSARDB_S3_READ_FROM_REPLICA", "garbage");
    assert(otsardb_replica_read_requested() == 0);
    set_env("OTSARDB_S3_READ_FROM_REPLICA", "1");
    assert(otsardb_replica_read_requested() == 1);
    assert(otsardb_replica_promote_requested() == 0); /* independent gates */

    clear_env("OTSARDB_S3_READ_FROM_REPLICA");
    assert(otsardb_replica_read_requested() == 0);
}

/* ---- T2: reader sees the COMMITTED state -------------------------------- */

static void test_reader_sees_committed(void) {
    set_env("OTSARDB_PUBLISH_RETRIES", "3");
    set_env("OTSARDB_PUBLISH_RETRY_BASE_MS", "0");
    dual_scenario sc = scenario_setup(ROOT_T2, 2);
    scenario_attach_replica(&sc);

    assert(drive_sync(sc.sync1_size, &sc.publisher) == 0);
    assert(drive_sync(sc.full_size, &sc.publisher) == 0);
    assert(sc.publisher.current_generation == 2);

    expect_head(sc.primary_inner, ROOT_T2, 2, CID2);
    expect_head(sc.replica_inner, ROOT_T2, 2, CID2);

    /* The reader path: recovery over the replica store. The store key set
     * must be IDENTICAL before and after — the reader performs ZERO writes
     * (no epoch bootstrap, no ledgers, no HEAD, no lease). */
    key_set before_keys;
    key_set after_keys;
    capture_keys(sc.replica_inner, ROOT_T2, &before_keys);
    assert(reader_restore(sc.replica_inner, ROOT_T2));
    capture_keys(sc.replica_inner, ROOT_T2, &after_keys);
    assert_key_sets_identical(&before_keys, &after_keys);
    release_keys(&before_keys);
    release_keys(&after_keys);

    /* The materialised image is at the replica's COMMITTED state: 6 pages x
     * page size (commit 2's database size) — the same bytes the primary's
     * chain describes. */
    assert(file_size_of(RECOVERED_DB) == 6 * (int)TEST_PAGE_SIZE);

    /* The replica ledger for the head commit is COMMITTED with the real
     * generation (the reader's view is the fully-committed state). */
    {
        char state[32];
        uint64_t generation = 0;
        assert(ledger_state(sc.replica_inner, ROOT_T2, CID2, state, sizeof(state),
                            &generation));
        assert(strcmp(state, "COMMITTED") == 0);
        assert(generation == 2);
    }

    /* No lease object exists on the replica store (the reader never touches
     * the lease; the writer's lease lives on the PRIMARY store only). */
    {
        char key[OTSARDB_KEY_MAX + 1];
        int n = snprintf(key, sizeof(key), "%s/lease.json", ROOT_T2);
        assert(n > 0 && (size_t)n < sizeof(key));
        otsardb_store_object object = {0};
        assert(otsardb_store_get(sc.replica_inner, key, &object) == OTSARDB_STORE_NOT_FOUND);
    }

    scenario_teardown(&sc);
}

/* ---- T3: the reader never publishes -------------------------------------- */

static void test_reader_never_publishes(void) {
    set_env("OTSARDB_PUBLISH_RETRIES", "3");
    set_env("OTSARDB_PUBLISH_RETRY_BASE_MS", "0");
    dual_scenario sc = scenario_setup(ROOT_T3, 2);
    scenario_attach_replica(&sc);

    /* Genesis dual, then the child commit's replica phase fails on every
     * attempt: the replica is left BEHIND (CID1) while the primary holds
     * CID2 — exactly the state in which a publish attempt over the replica
     * would have to advance it. */
    assert(drive_sync(sc.sync1_size, &sc.publisher) == 0);
    {
        const size_t fail[] = {1, 2, 3};
        otsardb_test_fault_store_fail_puts(sc.replica, fail, 3, OTSARDB_STORE_ERROR, 0);
        assert(drive_sync(sc.full_size, &sc.publisher) == SQLITE_IOERR_WRITE);
    }
    expect_head(sc.primary_inner, ROOT_T3, 2, CID2);
    expect_head(sc.replica_inner, ROOT_T3, 1, CID1);

    /* The read-replica session's publisher: over the REPLICA store, with
     * the permanently-closed fence (no lease was ever claimed). A WAL sync
     * that WOULD publish the pending commit is REFUSED with SQLITE_BUSY.
     * (The publish path first attempts the db.json metadata create, which
     * 412s on the already-replicated object — a read, not a write.) The
     * replica store's object set is untouched and the HEAD never advances. */
    otsardb_wal_publisher reader_pub;
    assert(otsardb_wal_publisher_init(&reader_pub, sc.replica, ROOT_T3,
                                    DATABASE_ID, OTSARDB_WRITE_STRATEGY_CAS));
    otsardb_wal_publisher_set_lease_fence(&reader_pub, reader_closed_fence, NULL);

    key_set before_keys;
    key_set after_keys;
    capture_keys(sc.replica_inner, ROOT_T3, &before_keys);
    assert(drive_sync(sc.full_size, &reader_pub) == SQLITE_BUSY);
    capture_keys(sc.replica_inner, ROOT_T3, &after_keys);
    assert_key_sets_identical(&before_keys, &after_keys);
    release_keys(&before_keys);
    release_keys(&after_keys);
    expect_head(sc.replica_inner, ROOT_T3, 1, CID1); /* HEAD unmoved */

    otsardb_wal_publisher_destroy(&reader_pub);
    scenario_teardown(&sc);
}

/* ---- T4: reader works while the primary session holds the lease ---------- */

static void test_reader_while_primary_lease_held(void) {
    set_env("OTSARDB_PUBLISH_RETRIES", "3");
    set_env("OTSARDB_PUBLISH_RETRY_BASE_MS", "0");
    dual_scenario sc = scenario_setup(ROOT_T4, 2);
    scenario_attach_replica(&sc);

    assert(drive_sync(sc.sync1_size, &sc.publisher) == 0);
    assert(drive_sync(sc.full_size, &sc.publisher) == 0);

    /* The primary session claims the writer lease on the PRIMARY store (the
     * same lease module the session path uses). While it is live, the
     * reader path over the REPLICA store must work normally: the lease is
     * per-destination (primary), the reader never reads or writes it. */
    char owner[OTSARDB_LEASE_OWNER_MAX + 1] = {0};
    snprintf(owner, sizeof(owner), "primary-lease-holder");
    uint64_t epoch = 0;
    int claim_state = OTSARDB_LEASE_ERROR;
    char holder[OTSARDB_LEASE_OWNER_MAX + 1] = {0};
    char holder_address[OTSARDB_LEASE_ADDRESS_MAX + 1] = {0};
    otsardb_lease *lease = otsardb_lease_claim(sc.primary_inner, ROOT_T4, owner, NULL,
                                           60, &epoch, &claim_state,
                                           holder, sizeof(holder),
                                           holder_address, sizeof(holder_address));
    assert(lease != NULL);
    assert(claim_state == OTSARDB_LEASE_CLAIMED);

    key_set before_keys;
    key_set after_keys;
    capture_keys(sc.replica_inner, ROOT_T4, &before_keys);
    assert(reader_restore(sc.replica_inner, ROOT_T4));
    capture_keys(sc.replica_inner, ROOT_T4, &after_keys);
    assert_key_sets_identical(&before_keys, &after_keys);
    release_keys(&before_keys);
    release_keys(&after_keys);

    expect_head(sc.replica_inner, ROOT_T4, 2, CID2);
    assert(file_size_of(RECOVERED_DB) == 6 * (int)TEST_PAGE_SIZE);

    /* The replica store carries no lease object. */
    {
        char key[OTSARDB_KEY_MAX + 1];
        int n = snprintf(key, sizeof(key), "%s/lease.json", ROOT_T4);
        assert(n > 0 && (size_t)n < sizeof(key));
        otsardb_store_object object = {0};
        assert(otsardb_store_get(sc.replica_inner, key, &object) == OTSARDB_STORE_NOT_FOUND);
    }

    otsardb_lease_destroy(lease);
    scenario_teardown(&sc);
}

/* ---- T5: replica behind -> reader sees the older COMMITTED commit --------- */

static void test_reader_behind_replica(void) {
    set_env("OTSARDB_PUBLISH_RETRIES", "3");
    set_env("OTSARDB_PUBLISH_RETRY_BASE_MS", "0");
    dual_scenario sc = scenario_setup(ROOT_T5, 2);
    scenario_attach_replica(&sc);

    assert(drive_sync(sc.sync1_size, &sc.publisher) == 0);

    /* The child commit's replica segment PUT fails on every attempt: the
     * dual publish fails, the primary ledger marks ABORTED, the primary
     * HEAD advanced to CID2 but the replica stays at its last
     * fully-COMMITTED state (CID1). The commit was never acked. */
    const size_t fail[] = {1, 2, 3};
    otsardb_test_fault_store_fail_puts(sc.replica, fail, 3, OTSARDB_STORE_ERROR, 0);
    assert(drive_sync(sc.full_size, &sc.publisher) == SQLITE_IOERR_WRITE);
    expect_head(sc.primary_inner, ROOT_T5, 2, CID2);
    expect_head(sc.replica_inner, ROOT_T5, 1, CID1);
    {
        char state[32];
        uint64_t generation = 0;
        assert(ledger_state(sc.primary_inner, ROOT_T5, CID2, state, sizeof(state),
                            &generation));
        assert(strcmp(state, "ABORTED") == 0);
    }

    /* The reader sees the OLDER fully-COMMITTED commit (CID1), never the
     * in-flight CID2: recovery from the replica store succeeds at the
     * replica's HEAD (generation 1, 3 pages) — the in-flight commit's
     * objects are unreachable from that HEAD. */
    assert(reader_restore(sc.replica_inner, ROOT_T5));
    expect_head(sc.replica_inner, ROOT_T5, 1, CID1);
    assert(file_size_of(RECOVERED_DB) == 3 * (int)TEST_PAGE_SIZE);

    /* The in-flight commit left nothing reachable on the replica: no
     * manifest, no ledger for CID2 (the primary's ABORTED marker is a
     * PRIMARY-store object — the reader never sees it). */
    {
        char key[OTSARDB_KEY_MAX + 1];
        int n = snprintf(key, sizeof(key), "%s/commits/%s.json", ROOT_T5, CID2);
        assert(n > 0 && (size_t)n < sizeof(key));
        otsardb_store_object object = {0};
        assert(otsardb_store_get(sc.replica_inner, key, &object) == OTSARDB_STORE_NOT_FOUND);
        n = snprintf(key, sizeof(key), "%s/replica/%s.json", ROOT_T5, CID2);
        assert(n > 0 && (size_t)n < sizeof(key));
        assert(otsardb_store_get(sc.replica_inner, key, &object) == OTSARDB_STORE_NOT_FOUND);
    }

    scenario_teardown(&sc);
}

/* ---- T6: reconciliation completes, then a new reader sees the new commit -- */

static void test_reader_after_reconcile(void) {
    set_env("OTSARDB_PUBLISH_RETRIES", "3");
    set_env("OTSARDB_PUBLISH_RETRY_BASE_MS", "0");
    dual_scenario sc = scenario_setup(ROOT_T6, 3);
    scenario_attach_replica(&sc);

    /* Session A: genesis dual, then CID2 fails dual (replica behind: the
     * replica segment PUT fails the whole 0065 budget). The primary ledger
     * marks ABORTED; the replica stays at CID1. */
    assert(drive_sync(sc.sync1_size, &sc.publisher) == 0);
    {
        const size_t fail[] = {1, 2, 3};
        otsardb_test_fault_store_fail_puts(sc.replica, fail, 3, OTSARDB_STORE_ERROR, 0);
        assert(drive_sync(sc.full_size, &sc.publisher) == SQLITE_IOERR_WRITE);
    }
    expect_head(sc.primary_inner, ROOT_T6, 2, CID2);
    expect_head(sc.replica_inner, ROOT_T6, 1, CID1);
    otsardb_wal_publisher_destroy(&sc.publisher); /* session A destroyed */

    /* Session B (fresh publisher over the SAME stores, replica attached):
     * the next dual publish triggers the 0077 reconciliation — the replica
     * phase detects the lag, REPLAYS CID2 (re-publishes objects, flips the
     * primary ABORTED ledger -> COMMITTED, CAS-advances the replica HEAD),
     * then publishes CID3 normally. Both destinations end at CID3 with
     * every ledger COMMITTED. */
    assert(otsardb_wal_publisher_init(&sc.publisher, sc.primary, ROOT_T6,
                                    DATABASE_ID, OTSARDB_WRITE_STRATEGY_DEFAULT));
    scenario_attach_replica(&sc);
    assert(drive_sync(sc.full_size, &sc.publisher) == 0);
    assert(drive_sync(sc.full_size, &sc.publisher) == 0);
    expect_head(sc.primary_inner, ROOT_T6, 3, CID3);
    expect_head(sc.replica_inner, ROOT_T6, 3, CID3);
    for (int which = 0; which < 2; ++which) {
        otsardb_object_store *store = which == 0 ? sc.primary_inner : sc.replica_inner;
        for (int commit = 1; commit <= 3; ++commit) {
            const char *cid = commit == 1 ? CID1 : (commit == 2 ? CID2 : CID3);
            char state[32];
            uint64_t generation = 0;
            assert(ledger_state(store, ROOT_T6, cid, state, sizeof(state),
                                &generation));
            assert(strcmp(state, "COMMITTED") == 0);
            assert(generation == (uint64_t)commit);
        }
    }

    /* A NEW reader sees the new commit: recovery over the replica store at
     * generation 3 / CID3, 9 pages x page size, and the replayed CID2
     * ledger COMMITTED on the replica. */
    assert(reader_restore(sc.replica_inner, ROOT_T6));
    expect_head(sc.replica_inner, ROOT_T6, 3, CID3);
    assert(file_size_of(RECOVERED_DB) == 9 * (int)TEST_PAGE_SIZE);
    {
        char state[32];
        uint64_t generation = 0;
        assert(ledger_state(sc.replica_inner, ROOT_T6, CID2, state, sizeof(state),
                            &generation));
        assert(strcmp(state, "COMMITTED") == 0);
        assert(generation == 2);
        assert(ledger_state(sc.replica_inner, ROOT_T6, CID3, state, sizeof(state),
                            &generation));
        assert(strcmp(state, "COMMITTED") == 0);
        assert(generation == 3);
    }

    scenario_teardown(&sc);
}

int main(void) {
    /* R5: mask the replica credential env so a developer's shell cannot
     * change the deterministic paths; pin the retry knobs per scenario. */
    set_env("OTSARDB_S3_REPLICA_ENDPOINT", "");
    set_env("OTSARDB_S3_REPLICA_REGION", "");
    set_env("OTSARDB_S3_REPLICA_ACCESS_KEY", "");
    set_env("OTSARDB_S3_REPLICA_SECRET_KEY", "");
    set_env("OTSARDB_S3_REPLICA_BUCKET", "");

    test_read_gate();
    test_reader_sees_committed();
    test_reader_never_publishes();
    test_reader_while_primary_lease_held();
    test_reader_behind_replica();
    test_reader_after_reconcile();

    puts("Multi-S3 read-replica sessions (COMMITTED view, zero writes, "
         "lease independence, behind-replica staleness, post-reconcile "
         "freshness) passed");
    return 0;
}
