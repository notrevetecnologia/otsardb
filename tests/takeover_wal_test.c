#include "journal.h"
#include "memory_store.h"
#include "otsardb.h"
#include "otsardb_internal.h"
#include "s3_database.h"
#include "sqlite3.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#define otsardb_unlink _unlink
#define otsardb_mkdir _mkdir
#define otsardb_setenv _putenv_s
#define otsardb_getpid _getpid
#define otsardb_sleep_ms(ms) Sleep((DWORD)(ms))
#else
#include <glob.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#define otsardb_unlink unlink
#define otsardb_mkdir(p) mkdir((p), 0755)
#define otsardb_setenv setenv
#define otsardb_getpid getpid
#define otsardb_sleep_ms(ms)                                                          \
    do {                                                                              \
        struct timespec ts;                                                           \
        ts.tv_sec = (ms) / 1000;                                                      \
        ts.tv_nsec = (long)((ms) % 1000) * 1000000L;                                  \
        nanosleep(&ts, NULL);                                                         \
    } while (0)
#endif

/* (S3-1): the lease-takeover path must WAL-neutralise the
 * connection (PRAGMA wal_checkpoint(TRUNCATE)) before rematerialising, so
 * a stale pre-takeover -wal can never replay old frames over the fresh
 * base. On Windows the -wal/-shm remove()s fail on the open handles, so
 * without the checkpoint the pre-takeover WAL survives the image swap and
 * the open SQLite connection can replay its frames over the new image (a
 * mixed image served silently by the session after the takeover). This is
 * the deterministic lease-steal-with-a-live-WAL regression:
 *
 * S1 seed the store with a committed schema + row (a bootstrap session
 * closes; the store then has a HEAD).
 * S2 session A opens as leader over the seeded store and commits another
 * row; its execution -wal holds the un-checkpointed frames
 * (wal_autocheckpoint is off). Sanity: the -wal exists, non-empty.
 * S3 A's lease is rewritten (etag bump -> A's next renew CAS-conflicts ->
 * A demotes to candidate), aged, and session B claims it at open and
 * commits another row (a foreign advance while A stays open).
 * S4 B closes (lease deleted). A's lease thread polls, claims, and runs
 * session_takeover_commit.
 * S5 CONTRACT: after the takeover the -wal is TRUNCATED to zero bytes
 * (or absent on POSIX sidecar unlink) — the swap was WAL-neutralised
 * exactly like the FRESH rematerialisation path. Without 
 * 0134 A's old frames survive (size stays > 0) and the assertion
 * fails deterministically.
 * S6 the taken-over session serves the remote head state (all three
 * rows), and its next write publishes the next generation (the fence
 * re-armed).
 *
 * Deterministic knobs: lease TTL = 3 s (min, renew every ~3 s, poll every
 * 2 s), bounded waits of up to 20 s for the lease-thread phases, one
 * scratch cache dir per scenario.
 */

static const char *DB_ID = "m0134-takeover-db";
static const char *ROOT = "m0134/takeover";
static char g_cache_dir[512];
static const char *SOURCE = "m0134-takeover-source.db";

static void set_env(const char *name, const char *value) {
#if defined(_WIN32)
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1);
    else unsetenv(name);
#endif
}

static void remove_files(const char *path) {
    char sidecar[512];
    otsardb_unlink(path);
    snprintf(sidecar, sizeof(sidecar), "%s-wal", path);
    otsardb_unlink(sidecar);
    snprintf(sidecar, sizeof(sidecar), "%s-shm", path);
    otsardb_unlink(sidecar);
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

/* Size of the FIRST *.db-wal file in the scenario cache dir (there is only
 * ever one at the points the test checks it: session B's files are removed
 * at its close). Returns 0 when absent, 1 when found. */
static int execution_wal_size(uint64_t *out_size) {
    *out_size = 0;
#if defined(_WIN32)
    char pattern[1024];
    snprintf(pattern, sizeof(pattern), "%s\\*", g_cache_dir);
    struct _finddata64i32_t fd;
    intptr_t handle = _findfirst(pattern, &fd);
    if (handle == -1) return 0;
    int found = 0;
    do {
        size_t len = strlen(fd.name);
        if (len > 4 && strcmp(fd.name + len - 4, "-wal") == 0) {
            char full[1024];
            snprintf(full, sizeof(full), "%s\\%s", g_cache_dir, fd.name);
            struct _stat64 st;
            if (_stat64(full, &st) == 0) {
                *out_size = (uint64_t)st.st_size;
                found = 1;
            }
            break;
        }
    } while (_findnext(handle, &fd) == 0);
    _findclose(handle);
    return found;
#else
    char pattern[1024];
    /* POSIX: glob the first entry */
    glob_t gl;
    snprintf(pattern, sizeof(pattern), "%s/*.db-wal", g_cache_dir);
    int rc = glob(pattern, 0, NULL, &gl);
    if (rc != 0) return 0;
    struct stat st;
    int found = stat(gl.gl_pathv[0], &st) == 0;
    if (found) *out_size = (uint64_t)st.st_size;
    globfree(&gl);
    return found;
#endif
}

typedef int (*wait_pred)(void *ctx);
static int wait_until(wait_pred pred, void *ctx, int timeout_ms) {
    int waited = 0;
    while (waited < timeout_ms) {
        if (pred(ctx)) return 1;
        otsardb_sleep_ms(100);
        waited += 100;
    }
    return pred(ctx);
}

static int wal_is_zero_or_absent(void *ctx) {
    (void)ctx;
    uint64_t size = 0;
    if (!execution_wal_size(&size)) return 1; /* absent */
    return size == 0;
}

static int wal_exists_nonempty(void *ctx) {
    (void)ctx;
    uint64_t size = 0;
    return execution_wal_size(&size) && size > 0;
}

/* Open a real session over a borrowed store with the full lease machinery
 * (CAS store: lease claim at open + background lease thread). */
static void open_session(otsardb_object_store *store, otsardb **out_db) {
    int rc = otsardb_s3_open_with_store(store, 1, ROOT, DB_ID, NULL, out_db);
    if (rc != OTSARDB_OK) {
        fprintf(stderr, "open_session failed: %s\n", otsardb_s3_last_error());
    }
    assert(rc == OTSARDB_OK);
}

static void steal_lease(otsardb_object_store *inner) {
    /* Identical rewrite -> new etag (A's renew If-Match fails -> A demotes),
     * then age the object so the next claim takes it over. */
    char key[OTSARDB_KEY_MAX + 1];
    int n = snprintf(key, sizeof(key), "%s/lease.json", ROOT);
    assert(n > 0 && (size_t)n < sizeof(key));
    otsardb_store_object object = {0};
    assert(otsardb_store_get(inner, key, &object) == OTSARDB_STORE_OK);
    assert(object.size > 0 && object.data != NULL);
    assert(otsardb_store_put(inner, key, object.data, object.size, NULL, NULL) ==
           OTSARDB_STORE_OK);
    otsardb_store_release_object(inner, &object);
    otsardb_test_memory_store_age(inner, key, 120);
}

static void expect_generation(otsardb_object_store *store, uint64_t generation) {
    otsardb_head head;
    char *etag = NULL;
    assert(otsardb_journal_read_head(store, ROOT, &head, &etag) == OTSARDB_JOURNAL_OK);
    free(etag);
    assert(head.generation == generation);
}

static void test_takeover_neutralises_wal(void) {
    printf("[S3-1] ");
    snprintf(g_cache_dir, sizeof(g_cache_dir), "%s/otsardb-m0134-takeover-%d",
             getenv("TEMP") ? getenv("TEMP") : ".", (int)otsardb_getpid());
    otsardb_mkdir(g_cache_dir);
    set_env("OTSARDB_CACHE_DIR", g_cache_dir);
    set_env("OTSARDB_LAZY_PREFETCH", "0");
    set_env("OTSARDB_S3_RAM_CACHE_MB", "0");
    set_env("OTSARDB_PUBLISH_RETRIES", "1");
    set_env("OTSARDB_PUBLISH_RETRY_BASE_MS", "0");
    set_env("OTSARDB_LEASE_TTL", "3");
    set_env("OTSARDB_AUTO_CHECKPOINT", "0");

    otsardb_object_store *inner = otsardb_test_memory_store_create();
    assert(inner != NULL);

    /* S1: seed the store (schema + one row) with a bootstrap session. */
    {
        otsardb *db_seed = NULL;
        open_session(inner, &db_seed);
        assert(exec_ok(db_seed, "CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT);") ==
               OTSARDB_OK);
        assert(exec_ok(db_seed, "INSERT INTO t(v) VALUES('one');") == OTSARDB_OK);
        expect_generation(inner, 2);
        otsardb_close(db_seed); /* releases the lease; execution files removed */
    }

    /* S2: session A opens as leader over the seeded store and commits a
     * row; A's execution -wal holds the un-checkpointed frames. */
    otsardb *db_a = NULL;
    open_session(inner, &db_a);
    assert(exec_ok(db_a, "INSERT INTO t(v) VALUES('a-row');") == OTSARDB_OK);
    expect_generation(inner, 3);
    assert(wait_until(wal_exists_nonempty, NULL, 5000));
    uint64_t wal_before = 0;
    assert(execution_wal_size(&wal_before) && wal_before > 0);
    fprintf(stderr, "S3-1 pre-takeover wal bytes=%llu\n",
            (unsigned long long)wal_before);

    /* S3: steal A's lease; session B claims it and commits a foreign row. */
    steal_lease(inner);
    otsardb *db_b = NULL;
    open_session(inner, &db_b);
    assert(exec_ok(db_b, "INSERT INTO t(v) VALUES('b-row');") == OTSARDB_OK);
    expect_generation(inner, 4);
    /* S4: B closes: its lease object is deleted, its execution files
     * removed (the only *.db-wal left is A's). */
    otsardb_close(db_b);

    /* S5: A's lease thread polls (<= 2 s), claims the absent lease and runs
     * session_takeover_commit. CONTRACT: the -wal is neutralised by the
     * takeover's wal_checkpoint(TRUNCATE) — zero bytes (Windows: the open
     * handle keeps the file) or absent (POSIX sidecar unlink). */
    assert(wait_until(wal_is_zero_or_absent, NULL, 20000));
    {
        uint64_t wsize = 0;
        fprintf(stderr, "S3-1 post-takeover wal bytes=%llu\n",
                execution_wal_size(&wsize) ? (unsigned long long)wsize : 0);
    }

    /* S6: the taken-over session serves the remote head state (3 rows),
     * and its next write publishes the next generation (fence re-armed). */
    char result[128];
    query_one_text(db_a, "SELECT count(*) FROM t;", result);
    fprintf(stderr, "S3-1 post-takeover count=%s\n", result);
    assert(strcmp(result, "3") == 0);
    query_one_text(db_a, "SELECT v FROM t WHERE id=3;", result);
    assert(strcmp(result, "b-row") == 0);
    assert(exec_ok(db_a, "INSERT INTO t(v) VALUES('three');") == OTSARDB_OK);
    expect_generation(inner, 5);

    /* Cross-check: a fresh session sees all four rows. */
    otsardb *db_c = NULL;
    open_session(inner, &db_c);
    query_one_text(db_c, "SELECT count(*) FROM t;", result);
    assert(strcmp(result, "4") == 0);
    otsardb_close(db_c);

    otsardb_close(db_a);
    otsardb_test_memory_store_destroy(inner);
    remove_files(SOURCE);
    puts("passed");
}

int main(void) {
    assert(otsardb_initialize() == OTSARDB_OK);
    test_takeover_neutralises_wal();
    puts("S3-1: takeover WAL neutralisation "
         "(checkpoint TRUNCATE symmetric with the FRESH path) passed");
    return 0;
}
