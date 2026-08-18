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
#define otsardb_unlink _unlink
#define otsardb_mkdir _mkdir
#define otsardb_setenv _putenv_s
#define otsardb_getpid _getpid
#define otsardb_sleep_ms(ms) Sleep((DWORD)(ms))
#else
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

/* (F3): a session opened on a HEAD-less database (the
 * first-writer / genesis case) must still receive the 0024 generation
 * anchor at generation 0, so its generation fence is never off. Without
 * the anchor local_image_generation_known stays 0 and a stale-base commit
 * of that session can win the HEAD CAS after a foreign takeover — the
 * strongest theoretical lost-update path in the failure-mode audit.
 *
 * Deterministic scenario:
 * F1 session A opens as leader on the EMPTY store (headless). It never
 * publishes anything (its local image IS the empty database at
 * generation 0).
 * F2 A's lease is rewritten (etag bump -> A demotes on its next renew),
 * aged, and session B claims and publishes the genesis (gen 1:
 * CREATE TABLE + INSERT). B closes.
 * F3 A attempts a WRITE (CREATE TABLE on its empty image).
 * F4 CONTRACT: the write is refused (SQLITE_BUSY — "database is locked"):
 * the generation fence trips because the remote head (1) is ahead of
 * A's anchor (0). The refusal happens BEFORE any object upload: the
 * store gains no new object and the head stays at B's gen-1 commit.
 * A stale headless write is never silently committed.
 */

static const char *DB_ID = "m0134-headless-db";
static const char *ROOT = "m0134/headless";
static char g_cache_dir[512];
static const char *SOURCE = "m0134-headless-source.db";

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

static void open_session(otsardb_object_store *store, otsardb **out_db) {
    int rc = otsardb_s3_open_with_store(store, 1, ROOT, DB_ID, NULL, out_db);
    if (rc != OTSARDB_OK) {
        fprintf(stderr, "open_session failed: %s\n", otsardb_s3_last_error());
    }
    assert(rc == OTSARDB_OK);
}

static void test_headless_open_anchors_generation_zero(void) {
    printf("[F3] ");
    snprintf(g_cache_dir, sizeof(g_cache_dir), "%s/otsardb-m0134-headless-%d",
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

    /* The store is HEAD-less: no genesis commit exists yet. */
    {
        otsardb_head head;
        char *etag = NULL;
        assert(otsardb_journal_read_head(inner, ROOT, &head, &etag) ==
               OTSARDB_JOURNAL_NOT_FOUND);
        free(etag);
    }

    /* F1: session A opens as leader on the empty store (headless open). */
    otsardb *db_a = NULL;
    open_session(inner, &db_a);

    /* F2: steal A's lease deterministically (etag bump -> A's next renew
     * CAS-conflicts -> A demotes), age it, and let B claim + publish the
     * genesis while A stays open with its empty image. */
    {
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
    otsardb *db_b = NULL;
    open_session(inner, &db_b);
    assert(exec_ok(db_b, "CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT);") == OTSARDB_OK);
    assert(exec_ok(db_b, "INSERT INTO t(v) VALUES('one');") == OTSARDB_OK);
    {
        otsardb_head head;
        char *etag = NULL;
        assert(otsardb_journal_read_head(inner, ROOT, &head, &etag) == OTSARDB_JOURNAL_OK);
        free(etag);
        assert(head.generation == 2);
    }
    otsardb_close(db_b);

    /* Object census before A's stale attempt. */
    int objects_before = count_prefix(inner, ROOT);

    /* F3 + F4: A's stale-base write is REFUSED by the generation fence
     * (remote head 1 > anchor 0). */
    char *error_message = NULL;
    int rc = otsardb_exec(db_a, "CREATE TABLE t2(x INTEGER);", NULL, NULL,
                          &error_message);
    fprintf(stderr, "F3 stale write rc=%d err=%s\n", rc,
            error_message ? error_message : "(null)");
    assert(rc != OTSARDB_OK);
    assert(error_message != NULL);
    assert(strstr(error_message, "locked") != NULL ||
           strstr(error_message, "busy") != NULL ||
           strstr(error_message, "BUSY") != NULL);
    otsardb_free(error_message);

    /* The fence refused BEFORE any object upload: nothing new on the store
     * and the head still points at B's committed genesis. */
    assert(count_prefix(inner, ROOT) == objects_before);
    {
        otsardb_head head;
        char *etag = NULL;
        assert(otsardb_journal_read_head(inner, ROOT, &head, &etag) == OTSARDB_JOURNAL_OK);
        free(etag);
        assert(head.generation == 2);
        assert(strcmp(head.commit_id, "") != 0);
    }

    /* Control: a fresh session (opened after the genesis) sees the committed
     * state — the refused stale session caused no damage. */
    otsardb *db_c = NULL;
    open_session(inner, &db_c);
    char result[128];
    query_one_text(db_c, "SELECT count(*) FROM t;", result);
    assert(strcmp(result, "1") == 0);

    otsardb_close(db_c);
    otsardb_close(db_a);
    otsardb_test_memory_store_destroy(inner);
    remove_files(SOURCE);
    puts("passed");
}

int main(void) {
    assert(otsardb_initialize() == OTSARDB_OK);
    test_headless_open_anchors_generation_zero();
    puts("F3: headless open anchors generation 0 — a stale "
         "headless write is refused with BUSY before any upload passed");
    return 0;
}
