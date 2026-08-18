#include "journal.h"
#include "memory_store.h"
#include "otsardb.h"
#include "otsardb_internal.h"
#include "object_store.h"
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
#include <process.h>
#include <windows.h>
#define otsardb_unlink _unlink
#define otsardb_mkdir _mkdir
#define otsardb_setenv _putenv_s
#define otsardb_getpid _getpid
#define otsardb_sleep_ms(ms) Sleep((DWORD)(ms))
#else
#include <pthread.h>
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

/* (E4): otsardb_exec must take the session sql_mutex so a
 * lease-thread takeover / rematerialisation (0123/0127) can never rewrite
 * the execution image under an in-flight statement. Deterministic race:
 *
 * E1 seed the store (schema), then session A opens as leader (CAS memory
 * store over a slow-GET wrapper, lazy hydration ON) and commits ~25k
 * rows across several generations.
 * E2 a second thread starts a long SELECT on A (every lazy page fetch is
 * a slow GET, so the statement runs for seconds).
 * E3 while the statement is in flight the main thread steals A's lease
 * (etag bump + age), opens session B, B commits one more row, B
 * closes — A's lease thread then takes over and (with 0134) blocks on
 * sql_mutex until the statement finishes.
 * E4 CONTRACT: the in-flight statement completes OTSARDB_OK on its pinned
 * pre-takeover snapshot (count == the exact pre-takeover row count) —
 * never IOERR and never a mixed old/new count. Then the takeover
 * completes and A's next statement adopts the new generation.
 *
 * Without 0134 the takeover rewrites the image + resets the page cache
 * mid-statement: the SELECT can fail (IOERR class) or read a mixed image.
 */

static const char *DB_ID = "m0134-exec-race-db";
static const char *ROOT = "m0134/exec-race";
/* 25 statements x 999 rows each (the recursive CTE is bounded by SQLite's
 * recursion limit of 1000). */
#define INSERT_STATEMENTS 25
#define ROWS_PER_STATEMENT 999
#define ROWS (INSERT_STATEMENTS * ROWS_PER_STATEMENT)

static char g_cache_dir[512];
static const char *SOURCE = "m0134-exec-race-source.db";

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

/* ---- slow-GET wrapper: every whole-object and range GET sleeps ---------- */

typedef struct slow_ctx {
    otsardb_object_store *inner;
    int delay_ms;
} slow_ctx;

static otsardb_store_result slow_get(otsardb_object_store *store,
                                     const char *key,
                                     otsardb_store_object *out) {
    slow_ctx *ctx = (slow_ctx *)store->context;
    if (ctx->delay_ms > 0) otsardb_sleep_ms(ctx->delay_ms);
    return otsardb_store_get(ctx->inner, key, out);
}

static otsardb_store_result slow_get_range(otsardb_object_store *store,
                                           const char *key,
                                           size_t offset,
                                           size_t length,
                                           otsardb_store_object *out) {
    slow_ctx *ctx = (slow_ctx *)store->context;
    if (ctx->delay_ms > 0) otsardb_sleep_ms(ctx->delay_ms);
    return otsardb_store_get_range(ctx->inner, key, offset, length, out);
}

static otsardb_store_result slow_put(otsardb_object_store *store,
                                     const char *key,
                                     const void *data,
                                     size_t size,
                                     const otsardb_store_put_options *options,
                                     char **out_etag) {
    slow_ctx *ctx = (slow_ctx *)store->context;
    return otsardb_store_put(ctx->inner, key, data, size, options, out_etag);
}

static void slow_release(otsardb_object_store *store, otsardb_store_object *object) {
    otsardb_store_release_object(((slow_ctx *)store->context)->inner, object);
}

static otsardb_store_result slow_delete(otsardb_object_store *store, const char *key) {
    return otsardb_store_delete(((slow_ctx *)store->context)->inner, key);
}

static otsardb_store_result slow_list(otsardb_object_store *store,
                                      const char *prefix,
                                      otsardb_store_list_callback callback,
                                      void *ctx) {
    return otsardb_store_list(((slow_ctx *)store->context)->inner, prefix, callback, ctx);
}

static const otsardb_object_store_ops SLOW_OPS = {
    .get = slow_get,
    .get_range = slow_get_range,
    .put = slow_put,
    .release_object = slow_release,
    .delete_object = slow_delete,
    .list = slow_list,
};

static otsardb_object_store *slow_store_create(otsardb_object_store *inner, int delay_ms) {
    otsardb_object_store *store = (otsardb_object_store *)calloc(1, sizeof(*store));
    slow_ctx *ctx = (slow_ctx *)calloc(1, sizeof(*ctx));
    assert(store && ctx);
    ctx->inner = inner;
    ctx->delay_ms = delay_ms;
    store->ops = &SLOW_OPS;
    store->context = ctx;
    store->capabilities = inner->capabilities;
    return store;
}

static void slow_store_destroy(otsardb_object_store *store) {
    if (!store) return;
    free(store->context);
    free(store);
}

/* ---- statement thread ---------------------------------------------------- */

typedef struct stmt_thread_ctx {
    otsardb *db;
    const char *sql;
    int rc;
    long count;
} stmt_thread_ctx;

static int count_cb(void *ctx, int argc, char **argv, char **columns) {
    (void)columns;
    if (argc != 1 || !argv || !argv[0]) return 1;
    *(long *)ctx = atol(argv[0]);
    return 0;
}

#if defined(_WIN32)
static unsigned __stdcall stmt_thread_main(void *arg) {
#else
static void *stmt_thread_main(void *arg) {
#endif
    stmt_thread_ctx *ctx = (stmt_thread_ctx *)arg;
    char *error_message = NULL;
    ctx->rc = otsardb_exec(ctx->db, ctx->sql, count_cb, &ctx->count, &error_message);
    if (error_message) {
        fprintf(stderr, "statement thread error: %s\n", error_message);
    }
    otsardb_free(error_message);
    return 0;
}

static void open_session(otsardb_object_store *store, otsardb **out_db) {
    int rc = otsardb_s3_open_with_store(store, 1, ROOT, DB_ID, NULL, out_db);
    if (rc != OTSARDB_OK) {
        fprintf(stderr, "open_session failed: %s\n", otsardb_s3_last_error());
    }
    assert(rc == OTSARDB_OK);
}

static void steal_lease(otsardb_object_store *inner) {
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

static void test_exec_serialised_against_takeover(void) {
    printf("[E4] ");
    snprintf(g_cache_dir, sizeof(g_cache_dir), "%s/otsardb-m0134-exec-race-%d",
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
    otsardb_object_store *slow = slow_store_create(inner, 20);
    assert(slow != NULL);

    /* E1a: seed the store (schema) with a bootstrap session. */
    {
        otsardb *db_seed = NULL;
        open_session(inner, &db_seed);
        assert(exec_ok(db_seed, "CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT);") ==
               OTSARDB_OK);
        otsardb_close(db_seed);
    }

    /* E1b: session A (leader) commits ~25k rows across 25 generations. The
     * segments make the lazy SELECT below need many slow GETs. */
    otsardb *db_a = NULL;
    open_session(slow, &db_a);
    for (int i = 0; i < INSERT_STATEMENTS; ++i) {
        char sql[256];
        snprintf(sql, sizeof(sql),
                 "WITH RECURSIVE seq(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM "
                 "seq WHERE x < %d) INSERT INTO t(v) SELECT 'row-'||x FROM seq;",
                 ROWS_PER_STATEMENT);
        assert(exec_ok(db_a, sql) == OTSARDB_OK);
    }
    {
        char result[128];
        query_one_text(db_a, "SELECT count(*) FROM t;", result);
        fprintf(stderr, "E4 rows after A inserts=%s\n", result);
    }

    /* E2: start the long SELECT on a second thread. */
    stmt_thread_ctx stmt = {db_a, "SELECT count(*) FROM t;", OTSARDB_ERROR, -1};
#if defined(_WIN32)
    uintptr_t thread = _beginthreadex(NULL, 0, stmt_thread_main, &stmt, 0, NULL);
    assert(thread != 0);
#else
    pthread_t thread;
    assert(pthread_create(&thread, NULL, stmt_thread_main, &stmt) == 0);
#endif
    /* Give the statement a head start: it is now deep in lazy hydration. */
    otsardb_sleep_ms(500);

    /* E3: steal the lease (etag bump -> A's renew CAS-conflicts -> A demotes
     * to candidate), age it, session B claims and commits one more row,
     * closes — A's lease thread then takes over. */
    steal_lease(inner);
    otsardb *db_b = NULL;
    open_session(slow, &db_b);
    assert(exec_ok(db_b, "INSERT INTO t(v) VALUES('two');") == OTSARDB_OK);
    otsardb_close(db_b);

    /* E4: join the statement. CONTRACT: it completed OK on its pinned
     * pre-takeover snapshot (count == ROWS). With 0134 the takeover was
     * blocked on sql_mutex; without it the image was rewritten mid-read. */
#if defined(_WIN32)
    WaitForSingleObject((HANDLE)thread, INFINITE);
    CloseHandle((HANDLE)thread);
#else
    pthread_join(thread, NULL);
#endif
    fprintf(stderr, "E4 statement rc=%d count=%ld (expected %d)\n",
            stmt.rc, stmt.count, ROWS);
    assert(stmt.rc == OTSARDB_OK);
    assert(stmt.count == ROWS);

    /* The takeover completed after the statement; A's next statement adopts
     * the new generation (bounded wait — the lease thread polls every 2 s). */
    {
        char result[128];
        char expected[64];
        snprintf(expected, sizeof(expected), "%d", ROWS + 1);
        int saw_new = 0;
        for (int i = 0; i < 200 && !saw_new; ++i) {
            query_one_text(db_a, "SELECT count(*) FROM t;", result);
            if (strcmp(result, expected) == 0) {
                saw_new = 1;
            } else {
                fprintf(stderr, "E4 transition count=%s\n", result);
                otsardb_sleep_ms(100);
            }
        }
        assert(saw_new);
    }

    otsardb_close(db_a);
    slow_store_destroy(slow);
    otsardb_test_memory_store_destroy(inner);
    remove_files(SOURCE);
    puts("passed");
}

int main(void) {
    assert(otsardb_initialize() == OTSARDB_OK);
    test_exec_serialised_against_takeover();
    puts("E4: otsardb_exec serialised against the lease-thread "
         "takeover (no mixed image, no IOERR) passed");
    return 0;
}
