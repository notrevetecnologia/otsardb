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

/* deterministic regression test: multi-S3 Model C quorum
 * N-of-M (section 7).
 *
 * Same harness pattern as the 0072/0077 suites: the publisher drives a
 * synthetic SQLite WAL over a PRIMARY store (memory_store + fault_store)
 * and a LIST of replica destinations, attached via otsardb_replica_attach
 * (no S3, no environment; repeated attaches build M = 1 + R).
 *
 * Quorum semantics under test:
 * Q = OTSARDB_S3_QUORUM of M = 1 primary + R destinations must hold the
 * commit + COMMITTED ledger for the ack; the primary always counts as 1;
 * the default (unset/0/garbage) is Q = M — with R = 1 that is the
 * byte-identical 0072 Model B contract. Transport-class failures are
 * absorbed while the quorum stays reachable (lagging destinations are
 * caught up by the 0077 gap-replay on the next dual publish);
 * authoritative failures fail the sync regardless of Q.
 *
 * Scenarios:
 * T1 Q=2-of-3, one destination down - the child commit ACKS with the
 * primary + one destination COMMITTED (the down one stays lagging at
 * the parent); the NEXT publish reconciles the lagging destination
 * (0077 gap replay: objects re-published, ledgers flipped, HEAD
 * advanced) and completes normally. Exact put counts asserted at the
 * quorum boundary.
 * T2 Q=3-of-3 (all required), one down - the same failure FAILS the
 * sync (2 of 3 < 3): publisher rolled back (dual_pending), the
 * primary ledger stays COMMITTED (never regressed by the best-effort
 * ABORTED marker), and a retry without faults completes the commit on
 * all three destinations.
 * T3 backward compatibility - ONE destination, NO OTSARDB_S3_QUORUM env:
 * byte-identical 0072 behavior — exact 0072 put counts (6/6 genesis,
 * 5/5 child), the same unrecoverable-failure contract (SQLITE_IOERR_
 * WRITE, primary ABORTED, rollback, INCONCLUSIVE resolve, retry
 * completes with 5/5), and quorum gate values (requested 0, effective
 * M = 2).
 * T4 promotion picks the newest COMMITTED - two destinations at
 * different safe-last-commits (A ahead, B lagging): otsardb_replica_
 * promote picks A (promoted_index 0), raises ONLY A's epoch 1 -> 2,
 * recovers from A; B's epoch record stays 1. Plus the nothing-to-
 * promote refusal on a destination with no COMMITTED ledger.
 * T5 Q=1 (primary-only) - BOTH destinations down: the sync STILL ACKS
 * (quorum 1 = the primary) and the primary's OWN COMMITTED ledger is
 * written (the ack barrier); the next publish reconciles BOTH
 * lagging destinations and all ledgers end COMMITTED.
 * T6 env gates - OTSARDB_S3_QUORUM parsing (absent/0/garbage -> 0 =
 * "all"; positive verbatim), effective-quorum clamping to 1..M, and
 * otsardb_replica_configured over the REPLICA2_* and REPLICA3_* sets.
 */

#define TEST_PAGE_SIZE 4096u

static const char *WAL_PATH = "otsardb-quorum.wal";
static const char *RECOVERED_DB = "otsardb-quorum-recovered.db";
static const char *ROOT_T1 = "quorum-test/t1";
static const char *ROOT_T2 = "quorum-test/t2";
static const char *ROOT_T3 = "quorum-test/t3";
static const char *ROOT_T4 = "quorum-test/t4";
static const char *ROOT_T4B = "quorum-test/t4b";
static const char *ROOT_T5 = "quorum-test/t5";
static const char *DATABASE_ID = "quorum-dual-db";
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
 * database size: 3/6/9 pages). sync1_size covers only the first commit,
 * sync2_size the first two (for 3-commit WALs so each commit is driven by
 * its own sync), full_size the whole file. */
static void build_synthetic_wal(int commits, size_t *out_sync1_size,
                                size_t *out_sync2_size, size_t *out_full_size) {
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
    if (out_sync2_size) {
        *out_sync2_size = commits >= 2
            ? (size_t)OTSARDB_SQLITE_WAL_HEADER_SIZE + 6u * frame_size
            : *out_sync1_size;
    }
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

    int ok = 0;
    const char *p = strstr(copy, "\"state\":\"");
    if (p) {
        p += strlen("\"state\":\"");
        const char *end = strchr(p, '"');
        if (end && (size_t)(end - p) < out_state_size) {
            memcpy(out_state, p, (size_t)(end - p));
            out_state[end - p] = '\0';
            ok = 1;
        }
    }
    if (ok) {
        p = strstr(copy, "\"generation\":");
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

static void assert_epoch_val(otsardb_object_store *store,
                             const char *root,
                             uint64_t expected) {
    otsardb_replica_epoch_view view;
    otsardb_journal_result rc = OTSARDB_JOURNAL_ERROR;
    assert(otsardb_replica_epoch_read(store, root, &view, &rc));
    assert(rc == OTSARDB_JOURNAL_OK && view.exists && view.epoch == expected);
}

/* ---- object capture (byte-for-byte comparison) -------------------------- */

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

static int key_has(const char *key, const char *needle) {
    return strstr(key, needle) != NULL;
}

static void assert_objects_identical(otsardb_object_store *a,
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
        if (skip_needle && key_has(key, skip_needle)) continue;
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

/* ---- scenario harness ---------------------------------------------------- */

/* M = 1 primary + up to 3 attached destinations. */
typedef struct multi_scenario {
    otsardb_object_store *primary_inner;
    otsardb_object_store *primary;
    otsardb_object_store *a_inner;
    otsardb_object_store *a;
    otsardb_object_store *b_inner;
    otsardb_object_store *b;
    otsardb_wal_publisher publisher;
    const char *root;
    size_t sync1_size;
    size_t sync2_size;
    size_t full_size;
    int commits;
} multi_scenario;

static multi_scenario scenario_setup(const char *root, int commits) {
    multi_scenario sc;
    memset(&sc, 0, sizeof(sc));
    sc.root = root;
    sc.commits = commits;
    otsardb_unlink(WAL_PATH);
    sc.primary_inner = otsardb_test_memory_store_create();
    assert(sc.primary_inner != NULL);
    sc.primary = otsardb_test_fault_store_create(sc.primary_inner);
    assert(sc.primary != NULL);
    assert(otsardb_wal_publisher_init(&sc.publisher,
                                    sc.primary,
                                    root,
                                    DATABASE_ID,
                                    OTSARDB_WRITE_STRATEGY_DEFAULT));
    assert(sc.publisher.strategy == OTSARDB_WRITE_STRATEGY_CAS);
    build_synthetic_wal(commits, &sc.sync1_size, &sc.sync2_size, &sc.full_size);
    return sc;
}

static void scenario_attach(otsardb_wal_publisher *publisher,
                            otsardb_object_store *inner,
                            otsardb_object_store **out_fault) {
    otsardb_object_store *fault = otsardb_test_fault_store_create(inner);
    assert(fault != NULL);
    assert(otsardb_replica_attach(publisher, fault, OTSARDB_WRITE_STRATEGY_CAS, 1));
    if (out_fault) *out_fault = fault;
}

static void scenario_attach_a(multi_scenario *sc) {
    sc->a_inner = otsardb_test_memory_store_create();
    assert(sc->a_inner != NULL);
    scenario_attach(&sc->publisher, sc->a_inner, &sc->a);
}

static void scenario_attach_b(multi_scenario *sc) {
    sc->b_inner = otsardb_test_memory_store_create();
    assert(sc->b_inner != NULL);
    scenario_attach(&sc->publisher, sc->b_inner, &sc->b);
}

static void scenario_teardown(multi_scenario *sc) {
    otsardb_wal_publisher_destroy(&sc->publisher);
    otsardb_test_fault_store_destroy(sc->primary);
    otsardb_test_memory_store_destroy(sc->primary_inner);
    if (sc->a) {
        otsardb_test_fault_store_destroy(sc->a);
        otsardb_test_memory_store_destroy(sc->a_inner);
    }
    if (sc->b) {
        otsardb_test_fault_store_destroy(sc->b);
        otsardb_test_memory_store_destroy(sc->b_inner);
    }
    otsardb_unlink(WAL_PATH);
    otsardb_unlink(RECOVERED_DB);
}

/* ---- T1: Q=2-of-3 ack with one destination down; reconcile on next ------ */

static void test_quorum_2of3_ack_and_reconcile(void) {
    set_env("OTSARDB_PUBLISH_RETRIES", "3");
    set_env("OTSARDB_PUBLISH_RETRY_BASE_MS", "0");
    set_env("OTSARDB_S3_QUORUM", "2");

    multi_scenario sc = scenario_setup(ROOT_T1, 3);
    scenario_attach_a(&sc);
    scenario_attach_b(&sc);
    assert(otsardb_replica_destination_count(sc.publisher.replica) == 2);
    assert(otsardb_replica_quorum(sc.publisher.replica) == 2);

    /* Genesis: all three destinations confirm (Q=2 is met either way). */
    assert(drive_sync(sc.sync1_size, &sc.publisher) == 0);
    expect_head(sc.primary_inner, ROOT_T1, 1, CID1);
    expect_head(sc.a_inner, ROOT_T1, 1, CID1);
    expect_head(sc.b_inner, ROOT_T1, 1, CID1);

    /* CID2: destination B is DOWN (every segment PUT fails the whole 0065
     * budget). The quorum is still reachable: primary + A = 2 of 3 -> the
     * sync ACKS; B stays lagging at CID1. */
    otsardb_test_fault_store_clear(sc.primary);
    otsardb_test_fault_store_clear(sc.a);
    const size_t fail[] = {1, 2, 3};
    otsardb_test_fault_store_fail_puts(sc.b, fail, 3, OTSARDB_STORE_ERROR, 0);
    int cid2_rc = drive_sync(sc.sync2_size, &sc.publisher);
    fprintf(stderr, "T1 cid2 rc=%d puts_p=%zu puts_a=%zu puts_b=%zu\n",
            cid2_rc,
            otsardb_test_fault_store_put_count(sc.primary),
            otsardb_test_fault_store_put_count(sc.a),
            otsardb_test_fault_store_put_count(sc.b));
    assert(cid2_rc == 0);
    /* primary: publish (3) + A's phase PREPARED + COMMITTED (2) = 5;
     * A: segment, manifest, PREPARED, HEAD, COMMITTED = 5;
     * B: 3 failed segment PUT attempts, nothing advanced. */
    assert(otsardb_test_fault_store_put_count(sc.primary) == 5);
    assert(otsardb_test_fault_store_put_count(sc.a) == 5);
    assert(otsardb_test_fault_store_put_count(sc.b) == 3);
    expect_head(sc.primary_inner, ROOT_T1, 2, CID2);
    expect_head(sc.a_inner, ROOT_T1, 2, CID2);
    expect_head(sc.b_inner, ROOT_T1, 1, CID1);
    {
        char state[16];
        uint64_t gen = 0;
        assert(ledger_state(sc.primary_inner, ROOT_T1, CID2, state, sizeof(state), &gen));
        assert(strcmp(state, "COMMITTED") == 0 && gen == 2);
        assert(ledger_state(sc.a_inner, ROOT_T1, CID2, state, sizeof(state), &gen));
        assert(strcmp(state, "COMMITTED") == 0 && gen == 2);
        /* B has NO CID2 ledger: the commit was never selected there. */
        assert(!ledger_state(sc.b_inner, ROOT_T1, CID2, state, sizeof(state), &gen));
    }
    assert(sc.publisher.current_generation == 2);
    assert(sc.publisher.dual_pending == 0);

    /* CID3 (B recovered): the lagging destination is RECONCILED by the 0077
     * gap-replay (CID2 re-published, ledgers flipped, HEAD advanced) and
     * the phase proceeds with CID3. All three destinations end at CID3 with
     * every ledger COMMITTED. */
    otsardb_test_fault_store_clear(sc.primary);
    otsardb_test_fault_store_clear(sc.a);
    otsardb_test_fault_store_clear(sc.b);
    int cid3_rc = drive_sync(sc.full_size, &sc.publisher);
    fprintf(stderr, "T1 cid3 rc=%d puts_p=%zu puts_a=%zu puts_b=%zu\n",
            cid3_rc,
            otsardb_test_fault_store_put_count(sc.primary),
            otsardb_test_fault_store_put_count(sc.a),
            otsardb_test_fault_store_put_count(sc.b));
    assert(cid3_rc == 0);
    /* primary: publish (3) + A's PREPARED/COMMITTED (2) = 5 (B's replay +
     * phase ledger writes are idempotent no-ops on the primary);
     * A: CID3 phase = 5;
     * B: CID2 replay (segment, manifest, PREPARED, HEAD, COMMITTED = 5) +
     * CID3 phase (5) = 10. */
    assert(otsardb_test_fault_store_put_count(sc.primary) == 5);
    assert(otsardb_test_fault_store_put_count(sc.a) == 5);
    assert(otsardb_test_fault_store_put_count(sc.b) == 10);
    expect_head(sc.primary_inner, ROOT_T1, 3, CID3);
    expect_head(sc.a_inner, ROOT_T1, 3, CID3);
    expect_head(sc.b_inner, ROOT_T1, 3, CID3);
    for (int which = 0; which < 3; ++which) {
        otsardb_object_store *store = which == 0 ? sc.primary_inner
                                       : which == 1 ? sc.a_inner : sc.b_inner;
        for (int commit = 1; commit <= 3; ++commit) {
            const char *cid = commit == 1 ? CID1 : commit == 2 ? CID2 : CID3;
            char state[16];
            uint64_t gen = 0;
            assert(ledger_state(store, ROOT_T1, cid, state, sizeof(state), &gen));
            assert(strcmp(state, "COMMITTED") == 0);
            assert(gen == (uint64_t)commit);
        }
    }
    /* Object sets are byte-identical across the three destinations modulo
     * the per-commit ledgers (which differ in destination/timestamp). */
    assert_objects_identical(sc.primary_inner, sc.a_inner, ROOT_T1, "/replica/");
    assert_objects_identical(sc.primary_inner, sc.b_inner, ROOT_T1, "/replica/");

    scenario_teardown(&sc);
}

/* ---- T2: Q=3-of-3 (all required), one down -> sync FAILS, retry wins ---- */

static void test_quorum_3of3_below_quorum_fails(void) {
    set_env("OTSARDB_PUBLISH_RETRIES", "3");
    set_env("OTSARDB_PUBLISH_RETRY_BASE_MS", "0");
    set_env("OTSARDB_S3_QUORUM", "3");

    multi_scenario sc = scenario_setup(ROOT_T2, 3);
    scenario_attach_a(&sc);
    scenario_attach_b(&sc);
    assert(otsardb_replica_quorum(sc.publisher.replica) == 3);
    assert(drive_sync(sc.sync1_size, &sc.publisher) == 0);

    /* CID2: destination B down. A confirms (primary ledger COMMITTED via
     * A's phase), but 2 of 3 < 3 -> the sync FAILS: the commit is NOT
     * acked (INCONCLUSIVE per 0021). */
    otsardb_test_fault_store_clear(sc.primary);
    otsardb_test_fault_store_clear(sc.a);
    const size_t fail[] = {1, 2, 3};
    otsardb_test_fault_store_fail_puts(sc.b, fail, 3, OTSARDB_STORE_ERROR, 0);
    int cid2_rc = drive_sync(sc.sync2_size, &sc.publisher);
    fprintf(stderr, "T2 failed rc=%d puts_p=%zu puts_a=%zu puts_b=%zu\n",
            cid2_rc,
            otsardb_test_fault_store_put_count(sc.primary),
            otsardb_test_fault_store_put_count(sc.a),
            otsardb_test_fault_store_put_count(sc.b));
    assert(cid2_rc == SQLITE_IOERR_WRITE);
    expect_head(sc.primary_inner, ROOT_T2, 2, CID2);
    expect_head(sc.a_inner, ROOT_T2, 2, CID2);
    expect_head(sc.b_inner, ROOT_T2, 1, CID1);
    /* The primary ledger for CID2 was written COMMITTED by A's successful
     * phase; the best-effort ABORTED marker must NOT regress it. */
    {
        char state[16];
        uint64_t gen = 0;
        assert(ledger_state(sc.primary_inner, ROOT_T2, CID2, state, sizeof(state), &gen));
        assert(strcmp(state, "COMMITTED") == 0 && gen == 2);
    }
    /* The publisher rolled back to the parent and records the dual-pending
     * retry (the exact 0072 contract). */
    assert(sc.publisher.current_generation == 1);
    assert(sc.publisher.published_commit_count == 1);
    assert(sc.publisher.dual_pending == 1);
    assert(strcmp(sc.publisher.dual_pending_commit_id, CID2) == 0);
    /* INCONCLUSIVE: the commit exists on the primary (durable) but was not
     * acked. */
    {
        otsardb_commit_state state = OTSARDB_COMMIT_STATE_UNKNOWN;
        assert(otsardb_resolve_commit(sc.primary_inner, ROOT_T2, CID2, 1, &state) ==
               OTSARDB_JOURNAL_OK);
        assert(state == OTSARDB_COMMIT_STATE_COMMITTED);
    }

    /* Retry (no faults): the whole dual operation re-attempts the SAME
     * commit id; A re-applies (self) and B advances from the parent; all
     * three destinations end COMMITTED and dual_pending clears. */
    otsardb_test_fault_store_clear(sc.primary);
    otsardb_test_fault_store_clear(sc.a);
    otsardb_test_fault_store_clear(sc.b);
    int retry_rc = drive_sync(sc.sync2_size, &sc.publisher);
    fprintf(stderr, "T2 retry rc=%d gen=%llu pending=%d\n",
            retry_rc, (unsigned long long)sc.publisher.current_generation,
            sc.publisher.dual_pending);
    assert(retry_rc == 0);
    expect_head(sc.primary_inner, ROOT_T2, 2, CID2);
    expect_head(sc.a_inner, ROOT_T2, 2, CID2);
    expect_head(sc.b_inner, ROOT_T2, 2, CID2);
    for (int which = 0; which < 3; ++which) {
        otsardb_object_store *store = which == 0 ? sc.primary_inner
                                       : which == 1 ? sc.a_inner : sc.b_inner;
        char state[16];
        uint64_t gen = 0;
        assert(ledger_state(store, ROOT_T2, CID2, state, sizeof(state), &gen));
        assert(strcmp(state, "COMMITTED") == 0 && gen == 2);
    }
    assert(count_manifests(sc.b_inner, ROOT_T2) == 2);
    assert(sc.publisher.current_generation == 2);
    assert(sc.publisher.dual_pending == 0);

    scenario_teardown(&sc);
}

/* ---- T3: backward compatibility — 1 replica, no quorum env -------------- */

static void test_backward_compat_single_destination(void) {
    set_env("OTSARDB_PUBLISH_RETRIES", "3");
    set_env("OTSARDB_PUBLISH_RETRY_BASE_MS", "0");
    set_env("OTSARDB_S3_QUORUM", "");
    set_env("OTSARDB_S3_REPLICA_ENDPOINT", "");
    set_env("OTSARDB_S3_REPLICA_REGION", "");
    set_env("OTSARDB_S3_REPLICA_ACCESS_KEY", "");
    set_env("OTSARDB_S3_REPLICA_SECRET_KEY", "");
    set_env("OTSARDB_S3_REPLICA_BUCKET", "");
    set_env("OTSARDB_S3_REPLICA2_ENDPOINT", "");
    set_env("OTSARDB_S3_REPLICA2_BUCKET", "");
    set_env("OTSARDB_S3_REPLICA3_ENDPOINT", "");
    set_env("OTSARDB_S3_REPLICA3_BUCKET", "");
    assert(otsardb_replica_configured() == 0);
    assert(otsardb_replica_quorum_requested() == 0);

    multi_scenario sc = scenario_setup(ROOT_T3, 2);
    scenario_attach_a(&sc); /* exactly ONE destination (M = 2) */
    assert(otsardb_replica_destination_count(sc.publisher.replica) == 1);
    assert(otsardb_replica_quorum(sc.publisher.replica) == 2);

    /* The exact 0072 T1 put counts: genesis 6/6, child 5/5. */
    otsardb_test_fault_store_clear(sc.primary);
    otsardb_test_fault_store_clear(sc.a);
    assert(drive_sync(sc.sync1_size, &sc.publisher) == 0);
    assert(otsardb_test_fault_store_put_count(sc.primary) == 6);
    assert(otsardb_test_fault_store_put_count(sc.a) == 6);
    otsardb_test_fault_store_clear(sc.primary);
    otsardb_test_fault_store_clear(sc.a);
    assert(drive_sync(sc.full_size, &sc.publisher) == 0);
    assert(otsardb_test_fault_store_put_count(sc.primary) == 5);
    assert(otsardb_test_fault_store_put_count(sc.a) == 5);
    expect_head(sc.primary_inner, ROOT_T3, 2, CID2);
    expect_head(sc.a_inner, ROOT_T3, 2, CID2);
    {
        char state[16];
        uint64_t gen = 0;
        assert(ledger_state(sc.primary_inner, ROOT_T3, CID2, state, sizeof(state), &gen));
        assert(strcmp(state, "COMMITTED") == 0);
        assert(ledger_state(sc.a_inner, ROOT_T3, CID2, state, sizeof(state), &gen));
        assert(strcmp(state, "COMMITTED") == 0);
    }

    /* The exact 0072 T3 failure contract: an unrecoverable destination
     * fails the sync (SQLITE_IOERR_WRITE), the primary ledger marks ABORTED
     * and the publisher rolls back (dual_pending). */
    {
        multi_scenario sc2 = scenario_setup(ROOT_T3, 2);
        scenario_attach_a(&sc2);
        assert(drive_sync(sc2.sync1_size, &sc2.publisher) == 0);
        otsardb_test_fault_store_clear(sc2.primary);
        const size_t fail[] = {1, 2, 3};
        otsardb_test_fault_store_fail_puts(sc2.a, fail, 3, OTSARDB_STORE_ERROR, 0);
        assert(drive_sync(sc2.full_size, &sc2.publisher) == SQLITE_IOERR_WRITE);
        expect_head(sc2.primary_inner, ROOT_T3, 2, CID2);
        expect_head(sc2.a_inner, ROOT_T3, 1, CID1);
        {
            char state[16];
            uint64_t gen = 0;
            assert(ledger_state(sc2.primary_inner, ROOT_T3, CID2, state, sizeof(state), &gen));
            assert(strcmp(state, "ABORTED") == 0);
        }
        assert(sc2.publisher.current_generation == 1);
        assert(sc2.publisher.dual_pending == 1);
        scenario_teardown(&sc2);
    }

    scenario_teardown(&sc);
}

/* ---- T4: promotion picks the newest COMMITTED destination --------------- */

static void test_promotion_picks_newest_committed(void) {
    set_env("OTSARDB_PUBLISH_RETRIES", "3");
    set_env("OTSARDB_PUBLISH_RETRY_BASE_MS", "0");
    set_env("OTSARDB_S3_QUORUM", "2");
    set_env("OTSARDB_LEASE_TTL", "30");

    /* A ahead of B: CID2 acked with B down (Q=2-of-3) -> A safe-last-commit
     * 2, B safe-last-commit 1. */
    multi_scenario sc = scenario_setup(ROOT_T4, 2);
    scenario_attach_a(&sc);
    scenario_attach_b(&sc);
    assert(drive_sync(sc.sync1_size, &sc.publisher) == 0);
    {
        const size_t fail[] = {1, 2, 3};
        otsardb_test_fault_store_fail_puts(sc.b, fail, 3, OTSARDB_STORE_ERROR, 0);
        assert(drive_sync(sc.sync2_size, &sc.publisher) == 0);
    }
    expect_head(sc.a_inner, ROOT_T4, 2, CID2);
    expect_head(sc.b_inner, ROOT_T4, 1, CID1);

    /* Promoter handle over the PRIMARY store with BOTH destinations
     * attached (the multi-destination promotion pick surface). */
    otsardb_wal_publisher scratch;
    assert(otsardb_wal_publisher_init(&scratch, sc.primary, ROOT_T4,
                                    DATABASE_ID, OTSARDB_WRITE_STRATEGY_DEFAULT));
    assert(otsardb_replica_attach(&scratch, sc.a, OTSARDB_WRITE_STRATEGY_CAS, 1));
    assert(otsardb_replica_attach(&scratch, sc.b, OTSARDB_WRITE_STRATEGY_CAS, 1));
    assert(otsardb_replica_destination_count(scratch.replica) == 2);

    otsardb_unlink(RECOVERED_DB);
    char err[512];
    err[0] = '\0';
    otsardb_journal_result rc = OTSARDB_JOURNAL_ERROR;
    assert(otsardb_replica_promote(scratch.replica, sc.primary, RECOVERED_DB,
                                 30, err, sizeof(err), &rc));
    assert(rc == OTSARDB_JOURNAL_OK);
    /* The NEWEST COMMITTED destination (A, index 0) was picked. */
    assert(otsardb_replica_promoted_index(scratch.replica) == 0);
    assert(otsardb_replica_epoch(scratch.replica) == 2);
    assert_epoch_val(sc.a_inner, ROOT_T4, 2); /* raised 1 -> 2 */
    assert_epoch_val(sc.b_inner, ROOT_T4, 1); /* untouched */
    /* FULL recovery from A at its HEAD: 6 pages x page size (CID2). */
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

    /* Nothing-to-promote refusal: a destination with NO COMMITTED ledger
     * (a fresh, never-seeded store) refuses the promotion with a clear
     * message; the epoch record is untouched. */
    {
        otsardb_wal_publisher scratch2;
        assert(otsardb_wal_publisher_init(&scratch2, sc.primary, ROOT_T4B,
                                        DATABASE_ID, OTSARDB_WRITE_STRATEGY_DEFAULT));
        otsardb_object_store *fresh_inner = otsardb_test_memory_store_create();
        assert(fresh_inner != NULL);
        otsardb_object_store *fresh = otsardb_test_fault_store_create(fresh_inner);
        assert(fresh != NULL);
        assert(otsardb_replica_attach(&scratch2, fresh, OTSARDB_WRITE_STRATEGY_CAS, 1));
        err[0] = '\0';
        rc = OTSARDB_JOURNAL_OK;
        assert(!otsardb_replica_promote(scratch2.replica, sc.primary, RECOVERED_DB,
                                      30, err, sizeof(err), &rc));
        assert(rc == OTSARDB_JOURNAL_CONFLICT);
        assert(strstr(err, "COMMITTED") != NULL);
        fprintf(stderr, "T4 nothing-to-promote refused as expected: %s\n", err);
        assert(otsardb_replica_promoted_index(scratch2.replica) == -1);
        otsardb_wal_publisher_destroy(&scratch2);
        otsardb_test_fault_store_destroy(fresh);
        otsardb_test_memory_store_destroy(fresh_inner);
    }

    scenario_teardown(&sc);
}

/* ---- T5: Q=1 (primary-only) with both destinations down ------------------ */

static void test_quorum_primary_only(void) {
    set_env("OTSARDB_PUBLISH_RETRIES", "3");
    set_env("OTSARDB_PUBLISH_RETRY_BASE_MS", "0");
    set_env("OTSARDB_S3_QUORUM", "1");

    multi_scenario sc = scenario_setup(ROOT_T5, 3);
    scenario_attach_a(&sc);
    scenario_attach_b(&sc);
    assert(otsardb_replica_quorum(sc.publisher.replica) == 1);
    assert(drive_sync(sc.sync1_size, &sc.publisher) == 0);

    /* CID2: BOTH destinations down (transport, budget exhausted). Quorum 1
     * = the primary alone -> the sync ACKS; the primary's OWN COMMITTED
     * ledger is written (the ack barrier) although no destination phase
     * reached the COMMITTED step. */
    otsardb_test_fault_store_clear(sc.primary);
    const size_t fail[] = {1, 2, 3};
    otsardb_test_fault_store_fail_puts(sc.a, fail, 3, OTSARDB_STORE_ERROR, 0);
    otsardb_test_fault_store_fail_puts(sc.b, fail, 3, OTSARDB_STORE_ERROR, 0);
    int cid2_rc = drive_sync(sc.sync2_size, &sc.publisher);
    fprintf(stderr, "T5 cid2 rc=%d puts_p=%zu\n", cid2_rc,
            otsardb_test_fault_store_put_count(sc.primary));
    assert(cid2_rc == 0);
    assert(otsardb_test_fault_store_put_count(sc.primary) == 4); /* publish 3 + ensure-ledger 1 */
    expect_head(sc.primary_inner, ROOT_T5, 2, CID2);
    expect_head(sc.a_inner, ROOT_T5, 1, CID1);
    expect_head(sc.b_inner, ROOT_T5, 1, CID1);
    {
        char state[16];
        uint64_t gen = 0;
        assert(ledger_state(sc.primary_inner, ROOT_T5, CID2, state, sizeof(state), &gen));
        assert(strcmp(state, "COMMITTED") == 0 && gen == 2);
        assert(!ledger_state(sc.a_inner, ROOT_T5, CID2, state, sizeof(state), &gen));
        assert(!ledger_state(sc.b_inner, ROOT_T5, CID2, state, sizeof(state), &gen));
    }
    assert(sc.publisher.current_generation == 2);
    assert(sc.publisher.dual_pending == 0);

    /* CID3 (both recovered): BOTH lagging destinations are reconciled (CID2
     * gap replay) and the commit completes on all three. */
    otsardb_test_fault_store_clear(sc.primary);
    otsardb_test_fault_store_clear(sc.a);
    otsardb_test_fault_store_clear(sc.b);
    int cid3_rc = drive_sync(sc.full_size, &sc.publisher);
    fprintf(stderr, "T5 cid3 rc=%d puts_p=%zu puts_a=%zu puts_b=%zu\n",
            cid3_rc,
            otsardb_test_fault_store_put_count(sc.primary),
            otsardb_test_fault_store_put_count(sc.a),
            otsardb_test_fault_store_put_count(sc.b));
    assert(cid3_rc == 0);
    expect_head(sc.primary_inner, ROOT_T5, 3, CID3);
    expect_head(sc.a_inner, ROOT_T5, 3, CID3);
    expect_head(sc.b_inner, ROOT_T5, 3, CID3);
    for (int which = 0; which < 3; ++which) {
        otsardb_object_store *store = which == 0 ? sc.primary_inner
                                       : which == 1 ? sc.a_inner : sc.b_inner;
        for (int commit = 1; commit <= 3; ++commit) {
            const char *cid = commit == 1 ? CID1 : commit == 2 ? CID2 : CID3;
            char state[16];
            uint64_t gen = 0;
            assert(ledger_state(store, ROOT_T5, cid, state, sizeof(state), &gen));
            assert(strcmp(state, "COMMITTED") == 0);
            assert(gen == (uint64_t)commit);
        }
    }
    assert_objects_identical(sc.primary_inner, sc.a_inner, ROOT_T5, "/replica/");
    assert_objects_identical(sc.primary_inner, sc.b_inner, ROOT_T5, "/replica/");

    scenario_teardown(&sc);
}

/* ---- T6: env gates ------------------------------------------------------- */

static void test_env_gates(void) {
    set_env("OTSARDB_S3_QUORUM", "");
    assert(otsardb_replica_quorum_requested() == 0);
    set_env("OTSARDB_S3_QUORUM", "0");
    assert(otsardb_replica_quorum_requested() == 0);
    set_env("OTSARDB_S3_QUORUM", "garbage");
    assert(otsardb_replica_quorum_requested() == 0);
    set_env("OTSARDB_S3_QUORUM", "-1");
    assert(otsardb_replica_quorum_requested() == 0);
    set_env("OTSARDB_S3_QUORUM", "2");
    assert(otsardb_replica_quorum_requested() == 2);
    set_env("OTSARDB_S3_QUORUM", "5");
    assert(otsardb_replica_quorum_requested() == 5);
    set_env("OTSARDB_S3_QUORUM", "4294967299"); /* > INT_MAX -> 0 (fail closed) */
    assert(otsardb_replica_quorum_requested() == 0);

    /* Effective quorum clamps to 1..M (M = destination_count + 1). */
    set_env("OTSARDB_PUBLISH_RETRIES", "3");
    set_env("OTSARDB_PUBLISH_RETRY_BASE_MS", "0");
    multi_scenario sc = scenario_setup(ROOT_T4B, 2);
    scenario_attach_a(&sc);
    scenario_attach_b(&sc);
    set_env("OTSARDB_S3_QUORUM", "5");
    assert(otsardb_replica_quorum(sc.publisher.replica) == 3); /* clamp to M */
    set_env("OTSARDB_S3_QUORUM", "1");
    assert(otsardb_replica_quorum(sc.publisher.replica) == 1);
    set_env("OTSARDB_S3_QUORUM", "2");
    assert(otsardb_replica_quorum(sc.publisher.replica) == 2);
    set_env("OTSARDB_S3_QUORUM", "");
    assert(otsardb_replica_quorum(sc.publisher.replica) == 3); /* default = M */
    set_env("OTSARDB_S3_QUORUM", "0");
    assert(otsardb_replica_quorum(sc.publisher.replica) == 3);

    /* otsardb_replica_configured over the REPLICA2_* and REPLICA3_* sets. */
    set_env("OTSARDB_S3_REPLICA_ENDPOINT", "");
    set_env("OTSARDB_S3_REPLICA_REGION", "");
    set_env("OTSARDB_S3_REPLICA_ACCESS_KEY", "");
    set_env("OTSARDB_S3_REPLICA_SECRET_KEY", "");
    set_env("OTSARDB_S3_REPLICA_BUCKET", "");
    set_env("OTSARDB_S3_REPLICA2_ENDPOINT", "");
    set_env("OTSARDB_S3_REPLICA2_REGION", "");
    set_env("OTSARDB_S3_REPLICA2_ACCESS_KEY", "");
    set_env("OTSARDB_S3_REPLICA2_SECRET_KEY", "");
    set_env("OTSARDB_S3_REPLICA2_BUCKET", "");
    set_env("OTSARDB_S3_REPLICA3_ENDPOINT", "");
    set_env("OTSARDB_S3_REPLICA3_REGION", "");
    set_env("OTSARDB_S3_REPLICA3_ACCESS_KEY", "");
    set_env("OTSARDB_S3_REPLICA3_SECRET_KEY", "");
    set_env("OTSARDB_S3_REPLICA3_BUCKET", "");
    assert(otsardb_replica_configured() == 0);
    set_env("OTSARDB_S3_REPLICA2_ENDPOINT", "http://replica2.invalid:9000");
    assert(otsardb_replica_configured() == 1);
    set_env("OTSARDB_S3_REPLICA2_ENDPOINT", "");
    assert(otsardb_replica_configured() == 0);
    set_env("OTSARDB_S3_REPLICA3_BUCKET", "otsardb-ci-replica3");
    assert(otsardb_replica_configured() == 1);
    set_env("OTSARDB_S3_REPLICA3_BUCKET", "");

    scenario_teardown(&sc);
}

int main(void) {
    /* R5: mask every replica/quorum env variable so a developer's shell
     * cannot silently change the default-path scenarios. */
    set_env("OTSARDB_S3_QUORUM", "");
    set_env("OTSARDB_S3_REPLICA_ENDPOINT", "");
    set_env("OTSARDB_S3_REPLICA_REGION", "");
    set_env("OTSARDB_S3_REPLICA_ACCESS_KEY", "");
    set_env("OTSARDB_S3_REPLICA_SECRET_KEY", "");
    set_env("OTSARDB_S3_REPLICA_BUCKET", "");
    set_env("OTSARDB_S3_REPLICA2_ENDPOINT", "");
    set_env("OTSARDB_S3_REPLICA2_REGION", "");
    set_env("OTSARDB_S3_REPLICA2_ACCESS_KEY", "");
    set_env("OTSARDB_S3_REPLICA2_SECRET_KEY", "");
    set_env("OTSARDB_S3_REPLICA2_BUCKET", "");
    set_env("OTSARDB_S3_REPLICA3_ENDPOINT", "");
    set_env("OTSARDB_S3_REPLICA3_REGION", "");
    set_env("OTSARDB_S3_REPLICA3_ACCESS_KEY", "");
    set_env("OTSARDB_S3_REPLICA3_SECRET_KEY", "");
    set_env("OTSARDB_S3_REPLICA3_BUCKET", "");

    test_quorum_2of3_ack_and_reconcile();
    test_quorum_3of3_below_quorum_fails();
    test_backward_compat_single_destination();
    test_promotion_picks_newest_committed();
    test_quorum_primary_only();
    test_env_gates();
    puts("Model C quorum N-of-M (Q=2-of-3 ack + reconcile, Q=3-of-3 fail, "
         "backward-compat, newest-COMMITTED promotion, Q=1 primary-only, env "
         "gates) passed");
    return 0;
}
