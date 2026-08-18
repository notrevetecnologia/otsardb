#include "journal.h"
#include "memory_store.h"
#include "otsardb.h"
#include "object_store.h"
#include "s3_database.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#define otsardb_unlink _unlink
#define otsardb_mkdir _mkdir
#define otsardb_setenv _putenv_s
#define otsardb_getpid _getpid
#else
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#define otsardb_unlink unlink
#define otsardb_mkdir(p) mkdir((p), 0755)
#define otsardb_setenv setenv
#define otsardb_getpid getpid
#endif

/* — F2 clock-skew takeover window vs the F3 generation fence.
 *
 * Failure-mode audit F2 (MEDIUM, PROJECTED, needs-test): a takeover candidate
 * whose clock is AHEAD of the holder can judge the holder's lease expired from
 * the store's Last-Modified (age = now - last_modified) while the holder still
 * believes it is live from its OWN clock (claimed_at). Until the holder's own
 * fence trips (local clock reaches TTL - skew, up to ~skew + renew interval
 * later), the holder can CAS-publish a commit computed from its stale snapshot
 * -> a silent lost update of the winner's acknowledged pages. The audit's
 * mitigation claim: the 0024 generation anchor (F3) closes the
 * stale-base window "regardless of clock skew", because every publish first
 * re-reads HEAD (wal_publisher.c refresh_head) and is refused with SQLITE_BUSY
 * when the remote generation is beyond the local image's generation
 * (wal_publisher.c:1530-1539) — BEFORE any object upload.
 *
 * Deterministic scenario (memory store, the skew injected by aging the lease
 * object's Last-Modified exactly as `otsardb_test_memory_store_age` does):
 *
 * F2-SKEW-MAIN (the dangerous schedule, with the fence present):
 * F1 holder A opens on the empty store (headless anchor 0), publishes
 * CREATE TABLE t + one row (HEAD gen 1 -> anchor 1, lease gen 1).
 * F2 inject clock skew: age <root>/lease.json's Last-Modified by
 * SKEW_AGE_SECS so a candidate's clock (now - last_modified) sees the
 * lease expired while A's own clock (claimed_at, renewed at most every
 * TTL/3 = 100 s) still reports the lease live.
 * F3 candidate B opens; its OPEN-TIME claim takes the lease over
 * (generation 2) synchronously, then B publishes a row (HEAD gen 2).
 * F4 A's stale write is REFUSED with BUSY ("database is locked"), BEFORE
 * any object upload (object census unchanged, HEAD still B's gen-2
 * commit). The winner's acknowledged row survives a fresh-session read.
 *
 * F2-SKEW-CONTROL (identical lease state, fence irrelevant):
 * C1 A opens + publishes the same genesis (lease gen 1, anchor 1).
 * C2 age the lease object identically.
 * C3 B opens and takes the lease over at generation 2 but DOES NOT publish;
 * HEAD is unchanged (still A's gen-1 commit, A's anchor still 1).
 * C4 A's stale write SUCCEEDS (rc == OTSARDB_OK): the lease-level window is
 * demonstrably open — A still believes it is live and the store has no
 * objection — so the lease alone cannot close F2.
 *
 * Discrimination: the holder's lease state is identical in MAIN (F4, refused)
 * and CONTROL (C4, published): same holder, same claimed_at (the renew thread
 * fires at most every TTL/3 = 100 s, far beyond the test), same lost flag. The
 * ONLY difference between the two schedules is that in MAIN the winner B has
 * advanced HEAD (remote generation > A's local-image anchor), while in CONTROL
 * HEAD is still at A's own generation. MAIN is therefore refused by the 0024
 * generation fence, and the same stale publish that CONTROL proves the lease
 * cannot stop is stopped in MAIN by the generation anchor. Had the generation
 * fence been absent, MAIN would behave exactly like CONTROL and A's stale
 * commit would win the CAS over B's acknowledged row (the F2 lost update).
 *
 * Skew envelope documented by this test: the lease-level takeover window is
 * bounded by OTSARDB_LEASE_SKEW_MARGIN_SECS + TTL/3 (the margin covers a
 * candidate clock up to 5 s ahead; the renew interval is the worst-case notice
 * latency). The DATA-loss window is 0 for anchored sessions: the generation
 * fence refuses the stale publish regardless of clock skew. This run uses
 * OTSARDB_LEASE_TTL=300 -> envelope = 5 + 100 = 105 s PROJECTED; the test
 * itself does not need the envelope to hold — it only needs SKEW_AGE_SECS to
 * exceed TTL - skew so B's takeover is deterministic.
 */

static const char *DB_ID = "m0148-fence-skew-db";
static const char *ROOT_MAIN = "m0148fence/main";
static const char *ROOT_CTRL = "m0148fence/control";
/* OTSARDB_LEASE_TTL for the run: 300 s -> renew cadence 100 s. The holder's
 * lease stays live for TTL - skew = 295 s on its own clock, so within the
 * (millisecond) test the renew thread never fires and the holder's lease is
 * provably still live — the same state in MAIN and CONTROL. */
#define TTL_SECS 300
/* Injected skew: age the lease's Last-Modified by 400 s so any candidate's
 * clock sees (now - last_modified) = elapsed + 400 >= TTL - skew = 295. */
#define SKEW_AGE_SECS 400

static void set_env(const char *name, const char *value) {
#if defined(_WIN32)
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1);
    else unsetenv(name);
#endif
}

static int exec_ok(otsardb *db, const char *sql) {
    char *error_message = NULL;
    int rc = otsardb_exec(db, sql, NULL, NULL, &error_message);
    if (error_message) {
        fprintf(stderr, "exec failed (%d): %s\n", rc, error_message);
    }
    otsardb_free(error_message);
    return rc;
}

static int count_cb(const char *key, void *ctx) {
    (void)key;
    ++*(int *)ctx;
    return 0;
}

static int count_prefix(otsardb_object_store *store, const char *prefix) {
    int count = 0;
    otsardb_store_list(store, prefix, count_cb, &count);
    return count;
}

static void open_session(otsardb_object_store *store,
                         const char *root,
                         otsardb **out_db) {
    int rc = otsardb_s3_open_with_store(store, 1, root, DB_ID, NULL, out_db);
    if (rc != OTSARDB_OK) {
        fprintf(stderr, "open_session failed: %s\n", otsardb_s3_last_error());
    }
    assert(rc == OTSARDB_OK);
}

static void read_head_generation(otsardb_object_store *store,
                                 const char *root,
                                 int expect_found,
                                 uint64_t *generation) {
    otsardb_head head;
    char *etag = NULL;
    int rc = otsardb_journal_read_head(store, root, &head, &etag);
    free(etag);
    if (expect_found) {
        assert(rc == OTSARDB_JOURNAL_OK);
        if (generation) *generation = head.generation;
    } else {
        assert(rc == OTSARDB_JOURNAL_NOT_FOUND);
    }
}

static int collect_text(void *ctx, int argc, char **argv, char **columns) {
    (void)columns;
    if (argc != 1 || !argv || !argv[0]) return 1;
    snprintf((char *)ctx, 128, "%s", argv[0]);
    return 0;
}

static void query_one_text(otsardb *db, const char *sql, char *out) {
    char *error_message = NULL;
    out[0] = '\0';
    int rc = otsardb_exec(db, sql, collect_text, out, &error_message);
    if (rc != OTSARDB_OK || error_message) {
        fprintf(stderr, "query failed (%d): %s\n", rc,
                error_message ? error_message : "");
    }
    otsardb_free(error_message);
    assert(rc == OTSARDB_OK);
}

static void setup_holder_genesis(otsardb *db_a) {
    /* Holder A publishes its own genesis: CREATE TABLE t (gen 1) and one row
     * (gen 2). The session opened on the HEAD-less store, so the 0134 headless
     * anchor pinned the generation fence at 0 and the publishes advance it to
     * the committed generation. */
    assert(exec_ok(db_a, "CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT);") == OTSARDB_OK);
    assert(exec_ok(db_a, "INSERT INTO t(v) VALUES('one');") == OTSARDB_OK);
}

/* MAIN: the dangerous schedule — the winner advances HEAD, then the holder's
 * stale publish must be refused by the generation fence before any upload. */
static void test_main_schedule(void) {
    printf("[F2-MAIN] ");
    char cache_dir[512];
    snprintf(cache_dir, sizeof(cache_dir), "%s/otsardb-m0148fence-main-%d",
             getenv("TEMP") ? getenv("TEMP") : ".", (int)otsardb_getpid());
    otsardb_mkdir(cache_dir);
    set_env("OTSARDB_CACHE_DIR", cache_dir);
    set_env("OTSARDB_LAZY_PREFETCH", "0");
    set_env("OTSARDB_S3_RAM_CACHE_MB", "0");
    set_env("OTSARDB_PUBLISH_RETRIES", "1");
    set_env("OTSARDB_PUBLISH_RETRY_BASE_MS", "0");
    set_env("OTSARDB_LEASE_TTL", "300");
    set_env("OTSARDB_AUTO_CHECKPOINT", "0");

    otsardb_object_store *store = otsardb_test_memory_store_create();
    assert(store != NULL);
    read_head_generation(store, ROOT_MAIN, 0, NULL);

    /* F1: holder A opens (headless) and publishes its genesis. */
    otsardb *db_a = NULL;
    open_session(store, ROOT_MAIN, &db_a);
    setup_holder_genesis(db_a);
    uint64_t gen_after_holder = 0;
    read_head_generation(store, ROOT_MAIN, 1, &gen_after_holder);

    /* F2: inject clock skew — age the lease's Last-Modified past TTL - skew so
     * a candidate with an ahead clock (age = now - last_modified) sees the
     * lease expired while the holder's own clock (claimed_at, renewed at most
     * every TTL/3 = 100 s) still reports it live. */
    {
        char key[OTSARDB_KEY_MAX + 1];
        int n = snprintf(key, sizeof(key), "%s/lease.json", ROOT_MAIN);
        assert(n > 0 && (size_t)n < sizeof(key));
        otsardb_store_object object = {0};
        assert(otsardb_store_get(store, key, &object) == OTSARDB_STORE_OK);
        assert(strstr((const char *)object.data, "\"owner\":\"") != NULL);
        otsardb_store_release_object(store, &object);
        otsardb_test_memory_store_age(store, key, SKEW_AGE_SECS);
    }

    /* F3: B opens; its open-time claim takes the expired lease over at
     * generation + 1 (synchronous, no polling), then B publishes one row. */
    otsardb *db_b = NULL;
    open_session(store, ROOT_MAIN, &db_b);
    assert(exec_ok(db_b, "INSERT INTO t(v) VALUES('two');") == OTSARDB_OK);
    uint64_t gen_after_winner = 0;
    read_head_generation(store, ROOT_MAIN, 1, &gen_after_winner);
    assert(gen_after_winner == gen_after_holder + 1);

    /* Object census before the holder's stale attempt. */
    int objects_before = count_prefix(store, ROOT_MAIN);

    /* F4: A's stale write is REFUSED by the 0024 generation fence: the remote
     * head (gen_after_winner) is beyond A's local-image anchor (gen_after_holder),
     * so the fence returns SQLITE_BUSY before any object upload. A still
     * believes its lease is live (the F2-SKEW-CONTROL below demonstrates that
     * identical lease state permits a publish when HEAD has not advanced). */
    char *error_message = NULL;
    int rc = otsardb_exec(db_a, "INSERT INTO t(v) VALUES('three');", NULL, NULL,
                          &error_message);
    fprintf(stderr, "F2-MAIN stale write rc=%d err=%s\n", rc,
            error_message ? error_message : "(null)");
    assert(rc != OTSARDB_OK);
    assert(error_message != NULL);
    assert(strstr(error_message, "locked") != NULL ||
           strstr(error_message, "busy") != NULL ||
           strstr(error_message, "BUSY") != NULL);
    otsardb_free(error_message);

    /* The fence refused BEFORE any object upload and the winner's commit is
     * untouched: census identical, head still B's gen_after_winner commit. */
    assert(count_prefix(store, ROOT_MAIN) == objects_before);
    {
        otsardb_head head;
        char *etag = NULL;
        assert(otsardb_journal_read_head(store, ROOT_MAIN, &head, &etag) ==
               OTSARDB_JOURNAL_OK);
        free(etag);
        assert(head.generation == gen_after_winner);
        assert(strcmp(head.commit_id, "") != 0);
    }

    /* The winner's acknowledged row is intact for a fresh reader. */
    otsardb *db_c = NULL;
    open_session(store, ROOT_MAIN, &db_c);
    char result[128];
    query_one_text(db_c, "SELECT count(*) FROM t;", result);
    assert(strcmp(result, "2") == 0);
    otsardb_close(db_c);

    otsardb_close(db_b);
    otsardb_close(db_a);
    otsardb_test_memory_store_destroy(store);
    puts("passed");
}
/* CONTROL: the identical holder-lease state with the winner NOT advancing
 * HEAD. A's stale write SUCCEEDS — the lease alone cannot close F2; the
 * generation fence is the clock-skew-independent barrier that MAIN exercises. */
static void test_control_lease_window_open(void) {
    printf("[F2-CTRL] ");
    char cache_dir[512];
    snprintf(cache_dir, sizeof(cache_dir), "%s/otsardb-m0148fence-ctrl-%d",
             getenv("TEMP") ? getenv("TEMP") : ".", (int)otsardb_getpid());
    otsardb_mkdir(cache_dir);
    set_env("OTSARDB_CACHE_DIR", cache_dir);
    set_env("OTSARDB_LAZY_PREFETCH", "0");
    set_env("OTSARDB_S3_RAM_CACHE_MB", "0");
    set_env("OTSARDB_PUBLISH_RETRIES", "1");
    set_env("OTSARDB_PUBLISH_RETRY_BASE_MS", "0");
    set_env("OTSARDB_LEASE_TTL", "300");
    set_env("OTSARDB_AUTO_CHECKPOINT", "0");

    otsardb_object_store *store = otsardb_test_memory_store_create();
    assert(store != NULL);
    read_head_generation(store, ROOT_CTRL, 0, NULL);

    /* C1: holder A publishes its genesis (lease gen 1, anchor gen 1). */
    otsardb *db_a = NULL;
    open_session(store, ROOT_CTRL, &db_a);
    setup_holder_genesis(db_a);
    uint64_t gen_after_holder = 0;
    read_head_generation(store, ROOT_CTRL, 1, &gen_after_holder);

    /* C2: identical skew injection. */
    {
        char key[OTSARDB_KEY_MAX + 1];
        int n = snprintf(key, sizeof(key), "%s/lease.json", ROOT_CTRL);
        assert(n > 0 && (size_t)n < sizeof(key));
        otsardb_store_object object = {0};
        assert(otsardb_store_get(store, key, &object) == OTSARDB_STORE_OK);
        otsardb_store_release_object(store, &object);
        otsardb_test_memory_store_age(store, key, SKEW_AGE_SECS);
    }

    /* C3: B opens and takes the lease over (generation + 1) but publishes
     * NOTHING; HEAD stays at the holder's generation. */
    otsardb *db_b = NULL;
    open_session(store, ROOT_CTRL, &db_b);

    /* C4: the holder's stale-base write SUCCEEDS (rc == OTSARDB_OK): with the
     * remote head still equal to A's anchor, the generation fence does not trip
     * and the lease fence is open (A's own clock, renew at most every TTL/3 =
     * 100 s, so A still believes the lease is live). This is the documented
     * F2 lease-level window: a takeover candidate with an ahead clock can hold
     * the store lease while the old holder still believes it is the writer. */
    assert(exec_ok(db_a, "INSERT INTO t(v) VALUES('three');") == OTSARDB_OK);
    uint64_t gen_after_stale = 0;
    read_head_generation(store, ROOT_CTRL, 1, &gen_after_stale);
    assert(gen_after_stale == gen_after_holder + 1);

    otsardb_close(db_b);
    otsardb_close(db_a);
    otsardb_test_memory_store_destroy(store);
    puts("passed");
}

int main(void) {
    assert(otsardb_initialize() == OTSARDB_OK);
    test_control_lease_window_open();
    test_main_schedule();
    puts("F2: the 0024 generation fence refuses a fenced-out "
         "holder's stale publish while its lease is still live, closing the "
         "clock-skew takeover window regardless of skew passed");
    return 0;
}
