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

/* deterministic regression test: Model B synchronous dual
 * publish (section 6).
 *
 * The publisher drives the SAME synthetic WAL as the 0065 retry test over a
 * PRIMARY store (memory_store + fault_store for put counting / fault
 * injection) and a REPLICA store (a second memory_store + fault_store,
 * attached via the otsardb_replica_attach test hook â€” no S3, no environment).
 *
 * Scenarios:
 * T1 success dual - both destinations hold every object, both
 * ledgers COMMITTED, replica HEAD == primary
 * HEAD (same commit id), ack == 0; segments,
 * manifests, HEAD and db.json are byte-identical
 * between the destinations; exact put counts.
 * T2 replica transient x2 - replica segment PUT fails ERROR twice, the
 * 0065 retry cycle re-attempts and the commit
 * succeeds (one HEAD advance, no duplicates).
 * T3 replica unrecoverable - budget exhausted -> sync FAILS, primary
 * ledger ABORTED, primary HEAD advanced but the
 * commit is NOT acked (identity resolve on the
 * primary reports COMMITTED â€” the INCONCLUSIVE
 * contract of); publisher state
 * rolled back; a retry then re-attempts the
 * replica phase (primary re-verified via
 * GET+compare + conflict interception) and
 * completes the dual commit exactly once.
 * T4 epoch once - the epoch record is created once; a second
 * session's If-None-Match create gets 412 and is
 * handled (one epoch object per destination).
 * T5 env absent - with NO replica env and NO attach the default
 * single-destination path is byte-identical to
 * a control run: identical key sets + object
 * bytes, no <root>/replica/ pollution, exact
 * baseline put counts.
 * T6 replica lag - an empty replica facing a CHAINED commit
 * fails closed: sync fails, primary ledger
 * ABORTED, replica HEAD never advances (v0 has
 * no roll-forward; the operator seeds the
 * replica â€”).
 */

#define TEST_PAGE_SIZE 4096u

static const char *WAL_PATH = "otsardb-replica-dual.wal";
static const char *ROOT_T1 = "replica-test/dual-ok";
static const char *ROOT_T2 = "replica-test/transient";
static const char *ROOT_T3 = "replica-test/unrecoverable";
static const char *ROOT_T4 = "replica-test/epoch-once";
static const char *ROOT_T5 = "replica-test/single-dest";
static const char *ROOT_T6 = "replica-test/lag";
static const char *DATABASE_ID = "replica-dual-db";
static const char *CID1 = "w1111111122222222-00000001";
static const char *CID2 = "w1111111122222222-00000002";

static void set_env(const char *name, const char *value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

/* ---- synthetic WAL construction (SQLite WAL format, as in 0065) --------- */

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

/* One WAL file, one WAL generation, two commits (genesis + child), exactly
 * like the 0065 retry test: sync 1 reads the genesis commit only, sync 2
 * reads the full file and continues from the cursor. */
static void build_synthetic_wal(size_t *out_sync1_size, size_t *out_full_size) {
    size_t frame_size = (size_t)OTSARDB_SQLITE_WAL_FRAME_HEADER_SIZE + TEST_PAGE_SIZE;
    size_t full_size = (size_t)OTSARDB_SQLITE_WAL_HEADER_SIZE + 6u * frame_size;
    unsigned char *wal = (unsigned char *)malloc(full_size);
    assert(wal != NULL);
    build_wal_header(wal, TEST_PAGE_SIZE, 0x11111111u, 0x22222222u);

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

static void expect_no_head(otsardb_object_store *store, const char *root) {
    otsardb_head head;
    char *etag = NULL;
    otsardb_journal_result rc = otsardb_journal_read_head(store, root, &head, &etag);
    free(etag);
    assert(rc == OTSARDB_JOURNAL_NOT_FOUND);
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
                        char *out_primary_etag,
                        size_t out_primary_etag_size,
                        char *out_replica_etag,
                        size_t out_replica_etag_size,
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

    const char *field_patterns[] = {"\"state\":\"", "\"primary_etag\":\"",
                                    "\"replica_etag\":\"", "\"manifest_sha256\":\""};
    char *out_fields[] = {out_state, out_primary_etag, out_replica_etag, out_manifest_sha256};
    size_t out_sizes[] = {out_state_size, out_primary_etag_size,
                          out_replica_etag_size, out_manifest_sha_size};
    int ok = 1;
    for (int i = 0; i < 4 && ok; ++i) {
        const char *p = strstr(copy, field_patterns[i]);
        if (!p) {
            ok = 0;
            break;
        }
        p += strlen(field_patterns[i]);
        const char *end = strchr(p, '"');
        if (!end || (size_t)(end - p) >= out_sizes[i]) {
            ok = 0;
            break;
        }
        memcpy(out_fields[i], p, (size_t)(end - p));
        out_fields[i][end - p] = '\0';
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
    if (ok) {
        const char *p = strstr(copy, "\"parent_generation\":");
        if (p) {
            p += strlen("\"parent_generation\":");
            while (*p == ' ' || *p == '\t') ++p;
            char *ep = NULL;
            unsigned long long g = strtoull(p, &ep, 10);
            if (ep != p) *out_parent_generation = (uint64_t)g;
            else ok = 0;
        } else ok = 0;
    }

    otsardb_store_release_object(store, &object);
    free(copy);
    return ok;
}

static void assert_epoch_exists(otsardb_object_store *store, const char *root) {
    char key[OTSARDB_KEY_MAX + 1];
    int n = snprintf(key, sizeof(key), "%s/replica/epoch.json", root);
    assert(n > 0 && (size_t)n < sizeof(key));
    otsardb_store_object object = {0};
    assert(otsardb_store_get(store, key, &object) == OTSARDB_STORE_OK);
    otsardb_store_release_object(store, &object);
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
    /* The store list API strips the prefix; rejoin it for the GET below. */
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

/* Capture every object of a store whose key starts with `root`. */
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

/* Byte-for-byte compare of the objects under `root` of `a` and `b`, skipping
 * keys containing `skip_needle` (the per-commit ledgers differ in
 * destination/etag/timestamp fields by design). */
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

static dual_scenario scenario_setup(const char *root) {
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
    build_synthetic_wal(&sc.sync1_size, &sc.full_size);
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

/* ---- T1: success dual ---------------------------------------------------- */

static void test_dual_success(void) {
    set_env("OTSARDB_PUBLISH_RETRIES", "3");
    set_env("OTSARDB_PUBLISH_RETRY_BASE_MS", "0");
    dual_scenario sc = scenario_setup(ROOT_T1);
    scenario_attach_replica(&sc);

    otsardb_test_fault_store_clear(sc.primary);
    otsardb_test_fault_store_clear(sc.replica);

    assert(drive_sync(sc.sync1_size, &sc.publisher) == 0);
    /* primary: db.json (metadata create) + seg, manifest, HEAD (publish) +
     * PREPARED + COMMITTED (ledger) */
    assert(otsardb_test_fault_store_put_count(sc.primary) == 6);
    /* replica: db.json (metadata copy), segment, manifest, PREPARED, HEAD,
     * COMMITTED */
    assert(otsardb_test_fault_store_put_count(sc.replica) == 6);

    otsardb_test_fault_store_clear(sc.primary);
    otsardb_test_fault_store_clear(sc.replica);
    int child_rc = drive_sync(sc.full_size, &sc.publisher);
    fprintf(stderr, "T1 child sync rc=%d\n", child_rc);
    assert(child_rc == 0);
    assert(otsardb_test_fault_store_put_count(sc.primary) == 5);
    /* db.json already replicated: segment, manifest, PREPARED, HEAD,
     * COMMITTED */
    assert(otsardb_test_fault_store_put_count(sc.replica) == 5);

    expect_head(sc.primary_inner, ROOT_T1, 2, CID2);
    expect_head(sc.replica_inner, ROOT_T1, 2, CID2);
    assert(count_manifests(sc.primary_inner, ROOT_T1) == 2);
    assert(count_manifests(sc.replica_inner, ROOT_T1) == 2);

    /* Both per-commit ledgers COMMITTED on BOTH destinations, with matching
     * generation/parent/manifest_sha256 and the real head etags. */
    for (int which = 0; which < 2; ++which) {
        otsardb_object_store *store = which == 0 ? sc.primary_inner : sc.replica_inner;
        for (int commit = 0; commit < 2; ++commit) {
            const char *cid = commit == 0 ? CID1 : CID2;
            char state[32];
            char primary_etag[128];
            char replica_etag[128];
            char manifest_sha[65];
            uint64_t generation = 0;
            uint64_t parent_generation = 0;
            int ledger_ok = ledger_state(store, ROOT_T1, cid,
                                         state, sizeof(state),
                                         primary_etag, sizeof(primary_etag),
                                         replica_etag, sizeof(replica_etag),
                                         &generation, &parent_generation,
                                         manifest_sha, sizeof(manifest_sha));
            if (!ledger_ok) {
                char key[OTSARDB_KEY_MAX + 1];
                snprintf(key, sizeof(key), "%s/replica/%s.json", ROOT_T1, cid);
                otsardb_store_object obj = {0};
                otsardb_store_result grc = otsardb_store_get(store, key, &obj);
                fprintf(stderr, "ledger decode failed: store=%d cid=%s get_rc=%d bytes=%zu\n",
                        which, cid, (int)grc, obj.size);
                if (grc == OTSARDB_STORE_OK) {
                    fwrite(obj.data, 1, obj.size > 512 ? 512 : obj.size, stderr);
                    fprintf(stderr, "\n");
                    otsardb_store_release_object(store, &obj);
                }
            }
            assert(ledger_ok);
            assert(strcmp(state, "COMMITTED") == 0);
            assert(generation == (uint64_t)(commit + 1));
            assert(parent_generation == (uint64_t)commit);
            /* manifest_sha256 must be a 64-hex-char SHA-256 string. */
            assert(strlen(manifest_sha) == 64);
            for (const char *p = manifest_sha; *p; ++p) {
                assert((*p >= '0' && *p <= '9') ||
                       (*p >= 'a' && *p <= 'f'));
            }
            assert(primary_etag[0] != '\0');
            assert(replica_etag[0] != '\0');
        }
    }

    /* The ledger etags must match the destinations' real HEAD etags. */
    {
        otsardb_head ph;
        char *p_etag = NULL;
        otsardb_head rh;
        char *r_etag = NULL;
        assert(otsardb_journal_read_head(sc.primary_inner, ROOT_T1, &ph, &p_etag) ==
               OTSARDB_JOURNAL_OK);
        assert(otsardb_journal_read_head(sc.replica_inner, ROOT_T1, &rh, &r_etag) ==
               OTSARDB_JOURNAL_OK);
        char state[32];
        char primary_etag[128];
        char replica_etag[128];
        char manifest_sha[65];
        uint64_t generation = 0;
        uint64_t parent_generation = 0;
        assert(ledger_state(sc.primary_inner, ROOT_T1, CID2,
                            state, sizeof(state),
                            primary_etag, sizeof(primary_etag),
                            replica_etag, sizeof(replica_etag),
                            &generation, &parent_generation,
                            manifest_sha, sizeof(manifest_sha)));
        assert(strcmp(primary_etag, p_etag) == 0);
        assert(strcmp(replica_etag, r_etag) == 0);
        free(p_etag);
        free(r_etag);
    }

    /* The epoch record exists on both destinations. */
    assert_epoch_exists(sc.primary_inner, ROOT_T1);
    assert_epoch_exists(sc.replica_inner, ROOT_T1);

    /* Byte-for-byte: everything except the per-commit ledgers is identical
     * between the destinations (same object keys, same bytes). */
    assert_objects_identical(sc.primary_inner, sc.replica_inner, ROOT_T1,
                             "/replica/");
    /* The ledgers on the two destinations carry the same commit identity:
     * the state/generation fields of the replica's COMMITTED ledger equal
     * the primary's. */
    {
        char st1[32], st2[32];
        char pe1[128], pe2[128];
        char re1[128], re2[128];
        char ms1[65], ms2[65];
        uint64_t g1 = 0, g2 = 0;
        uint64_t pg1 = 0, pg2 = 0;
        assert(ledger_state(sc.primary_inner, ROOT_T1, CID2, st1, sizeof(st1),
                            pe1, sizeof(pe1), re1, sizeof(re1), &g1, &pg1,
                            ms1, sizeof(ms1)));
        assert(ledger_state(sc.replica_inner, ROOT_T1, CID2, st2, sizeof(st2),
                            pe2, sizeof(pe2), re2, sizeof(re2), &g2, &pg2,
                            ms2, sizeof(ms2)));
        assert(strcmp(st1, "COMMITTED") == 0);
        assert(strcmp(st2, "COMMITTED") == 0);
        assert(g1 == g2 && pg1 == pg2);
        assert(strcmp(pe1, pe2) == 0 && strcmp(re1, re2) == 0);
    }

    assert(sc.publisher.current_generation == 2);
    assert(sc.publisher.dual_pending == 0);
    scenario_teardown(&sc);
}

/* ---- T2: replica transient failure x2 -> retried -> success -------------- */

static void test_replica_transient_retry(void) {
    set_env("OTSARDB_PUBLISH_RETRIES", "3");
    set_env("OTSARDB_PUBLISH_RETRY_BASE_MS", "0");
    dual_scenario sc = scenario_setup(ROOT_T2);
    scenario_attach_replica(&sc);

    assert(drive_sync(sc.sync1_size, &sc.publisher) == 0);

    /* The child commit's first replica store call is the segment PUT; fail
     * it ERROR on the first two attempts, the 0065 cycle retries. */
    const size_t fail[] = {1, 2};
    otsardb_test_fault_store_fail_puts(sc.replica, fail, 2, OTSARDB_STORE_ERROR, 0);

    assert(drive_sync(sc.full_size, &sc.publisher) == 0);
    /* 2 failed segment PUTs + segment, manifest, PREPARED, HEAD, COMMITTED */
    assert(otsardb_test_fault_store_put_count(sc.replica) == 7);

    expect_head(sc.primary_inner, ROOT_T2, 2, CID2);
    expect_head(sc.replica_inner, ROOT_T2, 2, CID2);
    assert(count_manifests(sc.primary_inner, ROOT_T2) == 2);
    assert(count_manifests(sc.replica_inner, ROOT_T2) == 2);

    /* Both ledgers COMMITTED after the retried commit. */
    for (int which = 0; which < 2; ++which) {
        otsardb_object_store *store = which == 0 ? sc.primary_inner : sc.replica_inner;
        char state[32];
        char primary_etag[128];
        char replica_etag[128];
        char manifest_sha[65];
        uint64_t generation = 0;
        uint64_t parent_generation = 0;
        assert(ledger_state(store, ROOT_T2, CID2,
                            state, sizeof(state),
                            primary_etag, sizeof(primary_etag),
                            replica_etag, sizeof(replica_etag),
                            &generation, &parent_generation,
                            manifest_sha, sizeof(manifest_sha)));
        assert(strcmp(state, "COMMITTED") == 0);
        assert(generation == 2 && parent_generation == 1);
    }
    assert_objects_identical(sc.primary_inner, sc.replica_inner, ROOT_T2,
                             "/replica/");
    scenario_teardown(&sc);
}

/* ---- T3: replica unrecoverable -> INCONCLUSIVE, then retry completes ----- */

static void test_replica_unrecoverable_then_retry(void) {
    set_env("OTSARDB_PUBLISH_RETRIES", "3");
    set_env("OTSARDB_PUBLISH_RETRY_BASE_MS", "0");
    dual_scenario sc = scenario_setup(ROOT_T3);
    scenario_attach_replica(&sc);

    assert(drive_sync(sc.sync1_size, &sc.publisher) == 0);

    /* The child commit's replica segment PUT fails on every attempt (3 =
     * budget): the replica phase fails, the primary ledger marks ABORTED
     * and the sync FAILS â€” the commit is NOT acked. Count only THIS sync's
     * puts: fail_puts resets the replica counter itself, and the primary
     * counter is cleared here so the epoch + genesis puts of the earlier
     * syncs do not leak into the assertion below. */
    otsardb_test_fault_store_clear(sc.primary);
    const size_t fail[] = {1, 2, 3};
    otsardb_test_fault_store_fail_puts(sc.replica, fail, 3, OTSARDB_STORE_ERROR, 0);
    assert(drive_sync(sc.full_size, &sc.publisher) == SQLITE_IOERR_WRITE);

    /* Primary HEAD advanced; replica HEAD did NOT advance; the primary
     * ledger carries the ABORTED marker (best-effort). */
    expect_head(sc.primary_inner, ROOT_T3, 2, CID2);
    expect_head(sc.replica_inner, ROOT_T3, 1, CID1);
    {
        char state[32];
        char primary_etag[128];
        char replica_etag[128];
        char manifest_sha[65];
        uint64_t generation = 0;
        uint64_t parent_generation = 0;
        assert(ledger_state(sc.primary_inner, ROOT_T3, CID2,
                            state, sizeof(state),
                            primary_etag, sizeof(primary_etag),
                            replica_etag, sizeof(replica_etag),
                            &generation, &parent_generation,
                            manifest_sha, sizeof(manifest_sha)));
        assert(strcmp(state, "ABORTED") == 0);
        assert(generation == 2 && parent_generation == 1);
    }
    /* The publisher state rolled back to the parent (the retry re-runs the
     * SAME commit id), and the pending dual identity is recorded. */
    assert(sc.publisher.current_generation == 1);
    assert(sc.publisher.published_commit_count == 1);
    assert(sc.publisher.dual_pending == 1);
    assert(strcmp(sc.publisher.dual_pending_commit_id, CID2) == 0);
    /* Primary puts in the failed sync: 3 (publish) + 1 (ABORTED ledger). */
    fprintf(stderr, "T3 failed-sync primary puts=%zu replica puts=%zu\n",
            otsardb_test_fault_store_put_count(sc.primary),
            otsardb_test_fault_store_put_count(sc.replica));
    assert(otsardb_test_fault_store_put_count(sc.primary) == 4);
    assert(otsardb_test_fault_store_put_count(sc.replica) == 3);

    /* INCONCLUSIVE contract: the commit EXISTS on the
     * primary (durable, HEAD advanced) but was NOT acked â€” a later
     * identity resolution reports COMMITTED. */
    {
        otsardb_commit_state state = OTSARDB_COMMIT_STATE_UNKNOWN;
        assert(otsardb_resolve_commit(sc.primary_inner, ROOT_T3, CID2, 1, &state) ==
               OTSARDB_JOURNAL_OK);
        assert(state == OTSARDB_COMMIT_STATE_COMMITTED);
    }

    /* Retry (no faults): the whole dual operation re-attempts. The failed
     * sync aborted the WAL scan BEFORE the cursor advanced past the child
     * commit (publish_one_commit returns failure and scan_reader leaves the
     * cursor at the parent), so this sync re-scans the child frames and the
     * SAME deterministic commit id (salt + frame index) is re-published:
     * the immutable objects re-verify via GET+compare, the HEAD CAS
     * preconditions-fails on our own commit, the conflict interception
     * re-enters the replica phase and the commit completes exactly once.
     * The preserved page map + rolling-checksum base make the recomputed
     * checksum byte-identical to the durable manifest. */
    otsardb_test_fault_store_clear(sc.primary);
    otsardb_test_fault_store_clear(sc.replica);
    int retry_rc = drive_sync(sc.full_size, &sc.publisher);
    fprintf(stderr, "T3 retry rc=%d gen=%llu pending=%d puts_p=%zu puts_r=%zu\n",
            retry_rc, (unsigned long long)sc.publisher.current_generation,
            sc.publisher.dual_pending,
            otsardb_test_fault_store_put_count(sc.primary),
            otsardb_test_fault_store_put_count(sc.replica));
    assert(retry_rc == 0);
    /* Primary puts: segment/manifest/HEAD (412 + GET+compare, counted) +
     * PREPARED + COMMITTED ledger. */
    assert(otsardb_test_fault_store_put_count(sc.primary) == 5);
    assert(otsardb_test_fault_store_put_count(sc.replica) == 5);

    expect_head(sc.primary_inner, ROOT_T3, 2, CID2);
    expect_head(sc.replica_inner, ROOT_T3, 2, CID2);
    /* Exactly two manifests per destination â€” the retry did NOT create a
     * second commit. */
    assert(count_manifests(sc.primary_inner, ROOT_T3) == 2);
    assert(count_manifests(sc.replica_inner, ROOT_T3) == 2);
    {
        char state[32];
        char primary_etag[128];
        char replica_etag[128];
        char manifest_sha[65];
        uint64_t generation = 0;
        uint64_t parent_generation = 0;
        assert(ledger_state(sc.primary_inner, ROOT_T3, CID2,
                            state, sizeof(state),
                            primary_etag, sizeof(primary_etag),
                            replica_etag, sizeof(replica_etag),
                            &generation, &parent_generation,
                            manifest_sha, sizeof(manifest_sha)));
        assert(strcmp(state, "COMMITTED") == 0);
        assert(generation == 2 && parent_generation == 1);
    }
    assert(sc.publisher.current_generation == 2);
    assert(sc.publisher.dual_pending == 0);
    assert_objects_identical(sc.primary_inner, sc.replica_inner, ROOT_T3,
                             "/replica/");
    scenario_teardown(&sc);
}

/* ---- T4: epoch created once; second session's 412 handled ---------------- */

static void test_epoch_created_once(void) {
    set_env("OTSARDB_PUBLISH_RETRIES", "3");
    set_env("OTSARDB_PUBLISH_RETRY_BASE_MS", "0");
    dual_scenario sc = scenario_setup(ROOT_T4);
    scenario_attach_replica(&sc);
    assert(drive_sync(sc.sync1_size, &sc.publisher) == 0);
    assert(drive_sync(sc.full_size, &sc.publisher) == 0);

    assert_epoch_exists(sc.primary_inner, ROOT_T4);
    assert_epoch_exists(sc.replica_inner, ROOT_T4);

    /* Second session over the SAME stores: the If-None-Match epoch create
     * gets 412 and is handled; the whole re-publish is idempotent (the
     * ledger records stay COMMITTED, exactly one epoch object per
     * destination, no new objects). */
    otsardb_wal_publisher publisher2;
    assert(otsardb_wal_publisher_init(&publisher2, sc.primary, ROOT_T4,
                                    DATABASE_ID, OTSARDB_WRITE_STRATEGY_DEFAULT));
    assert(otsardb_replica_attach(&publisher2, sc.replica,
                                OTSARDB_WRITE_STRATEGY_CAS, 1));

    otsardb_test_fault_store_clear(sc.primary);
    otsardb_test_fault_store_clear(sc.replica);
    /* A fresh publisher has no WAL cursor: the sync rebuilds it from the
     * remote HEAD (both commits span the FULL file), so the rebuild must
     * see full_size â€” sync1_size would cut the child commit's frames and
     * fail the rebuild with SQLITE_CORRUPT. Both syncs are then no-ops
     * (the cursor already covers the file); the stores must be unchanged:
     * the ledgers stay COMMITTED and exactly one epoch object exists per
     * destination. */
    assert(drive_sync(sc.full_size, &publisher2) == 0);
    assert(drive_sync(sc.full_size, &publisher2) == 0);

    assert_epoch_exists(sc.primary_inner, ROOT_T4);
    assert_epoch_exists(sc.replica_inner, ROOT_T4);
    {
        char prefix[OTSARDB_KEY_MAX + 1];
        int n = snprintf(prefix, sizeof(prefix), "%s/replica/", ROOT_T4);
        assert(n > 0 && (size_t)n < sizeof(prefix));
        assert(count_prefix(sc.primary_inner, prefix) == 3); /* epoch + 2 ledgers */
        assert(count_prefix(sc.replica_inner, prefix) == 3);
    }
    expect_head(sc.primary_inner, ROOT_T4, 2, CID2);
    expect_head(sc.replica_inner, ROOT_T4, 2, CID2);

    otsardb_wal_publisher_destroy(&publisher2);
    scenario_teardown(&sc);
}

/* ---- T5: no replica env -> single-destination path identical ------------- */

static void test_single_destination_unchanged(void) {
    set_env("OTSARDB_PUBLISH_RETRIES", "1");
    set_env("OTSARDB_PUBLISH_RETRY_BASE_MS", "0");

    /* The replica env must be ABSENT (R5: the harness also clears it so a
     * developer's shell cannot silently turn this into a dual run). */
    set_env("OTSARDB_S3_REPLICA_ENDPOINT", "");
    set_env("OTSARDB_S3_REPLICA_REGION", "");
    set_env("OTSARDB_S3_REPLICA_ACCESS_KEY", "");
    set_env("OTSARDB_S3_REPLICA_SECRET_KEY", "");
    set_env("OTSARDB_S3_REPLICA_BUCKET", "");
    assert(otsardb_replica_configured() == 0);

    object_capture run_a;
    object_capture run_b;

    for (int run = 0; run < 2; ++run) {
        otsardb_unlink(WAL_PATH);
        otsardb_object_store *inner = otsardb_test_memory_store_create();
        assert(inner != NULL);
        otsardb_object_store *fault = otsardb_test_fault_store_create(inner);
        assert(fault != NULL);
        otsardb_wal_publisher publisher;
        assert(otsardb_wal_publisher_init(&publisher, fault, ROOT_T5,
                                        DATABASE_ID, OTSARDB_WRITE_STRATEGY_DEFAULT));

        size_t sync1_size = 0;
        size_t full_size = 0;
        build_synthetic_wal(&sync1_size, &full_size);
        assert(drive_sync(sync1_size, &publisher) == 0);

        otsardb_test_fault_store_clear(fault);
        assert(drive_sync(full_size, &publisher) == 0);
        /* The pre-0072 baseline: exactly 3 puts per child commit. */
        assert(otsardb_test_fault_store_put_count(fault) == 3);

        expect_head(inner, ROOT_T5, 2, CID2);
        assert(count_manifests(inner, ROOT_T5) == 2);

        /* No <root>/replica/ objects may exist on the default path. */
        {
            char prefix[OTSARDB_KEY_MAX + 1];
            int n = snprintf(prefix, sizeof(prefix), "%s/replica/", ROOT_T5);
            assert(n > 0 && (size_t)n < sizeof(prefix));
            assert(count_prefix(inner, prefix) == 0);
        }

        if (run == 0) capture_objects(inner, ROOT_T5, &run_a);
        else capture_objects(inner, ROOT_T5, &run_b);

        otsardb_wal_publisher_destroy(&publisher);
        otsardb_test_fault_store_destroy(fault);
        otsardb_test_memory_store_destroy(inner);
        otsardb_unlink(WAL_PATH);
    }

    /* Byte-for-byte: the control run published the identical object set. */
    assert(run_a.count == run_b.count);
    for (size_t i = 0; i < run_a.count; ++i) {
        int found = 0;
        for (size_t j = 0; j < run_b.count; ++j) {
            if (strcmp(run_b.objects[j].key, run_a.objects[i].key) == 0) {
                assert(run_b.objects[j].size == run_a.objects[i].size);
                assert(run_a.objects[i].size == 0 ||
                       memcmp(run_b.objects[j].data, run_a.objects[i].data,
                              run_a.objects[i].size) == 0);
                found = 1;
                break;
            }
        }
        assert(found);
    }
    release_capture(&run_a);
    release_capture(&run_b);
}

/* ---- T6: replica lag fails closed ----------------------------------------- */

static void test_replica_lag_fails_closed(void) {
    set_env("OTSARDB_PUBLISH_RETRIES", "3");
    set_env("OTSARDB_PUBLISH_RETRY_BASE_MS", "0");
    dual_scenario sc = scenario_setup(ROOT_T6);

    /* Publish the genesis commit on the PRIMARY only (no replica yet). */
    assert(drive_sync(sc.sync1_size, &sc.publisher) == 0);

    /* Attach an EMPTY replica (no genesis, no objects): the child commit's
     * replica phase finds a replica HEAD where the parent should be and
     * fails closed â€” v0 has no roll-forward/reconciliation (0072). The
     * replica-head mismatch is an AUTHORITATIVE CONFLICT class (0065: never
     * retried), which maps to SQLITE_BUSY: the sync fails and the commit is
     * NOT acked. */
    scenario_attach_replica(&sc);
    assert(drive_sync(sc.full_size, &sc.publisher) == SQLITE_BUSY);

    expect_head(sc.primary_inner, ROOT_T6, 2, CID2);
    expect_no_head(sc.replica_inner, ROOT_T6);
    {
        char state[32];
        char primary_etag[128];
        char replica_etag[128];
        char manifest_sha[65];
        uint64_t generation = 0;
        uint64_t parent_generation = 0;
        assert(ledger_state(sc.primary_inner, ROOT_T6, CID2,
                            state, sizeof(state),
                            primary_etag, sizeof(primary_etag),
                            replica_etag, sizeof(replica_etag),
                            &generation, &parent_generation,
                            manifest_sha, sizeof(manifest_sha)));
        assert(strcmp(state, "ABORTED") == 0);
    }
    assert(sc.publisher.current_generation == 1);
    assert(sc.publisher.dual_pending == 1);
    scenario_teardown(&sc);
}

int main(void) {
    /* R5: mask the replica credentials env so a developer's shell cannot
     * change the default-path scenario; pin the retry knobs per scenario. */
    set_env("OTSARDB_S3_REPLICA_ENDPOINT", "");
    set_env("OTSARDB_S3_REPLICA_REGION", "");
    set_env("OTSARDB_S3_REPLICA_ACCESS_KEY", "");
    set_env("OTSARDB_S3_REPLICA_SECRET_KEY", "");
    set_env("OTSARDB_S3_REPLICA_BUCKET", "");

    test_dual_success();
    test_replica_transient_retry();
    test_replica_unrecoverable_then_retry();
    test_epoch_created_once();
    test_single_destination_unchanged();
    test_replica_lag_fails_closed();
    puts("Model B synchronous dual publish (dual durability, INCONCLUSIVE retry, "
         "epoch-once, single-destination identity) passed");
    return 0;
}
