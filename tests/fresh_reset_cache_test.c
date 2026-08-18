/* fresh_reset_cache_test.c — regression test for 
 * 0152, failure-mode audit E3 (FRESH page-cache reset failure must fail the
 * statement, never run on a possibly-stale cache).
 *
 * The FRESH re-materialisation path (s3_database.cpp session_fresh_rematerialize)
 * swaps the execution image to the new generation and then calls
 * SQLITE_FCNTL_RESET_CACHE. Before 0152 a reset failure was warning-only, so
 * the next statement could run on stale cached pages. After 0152 the swap
 * fails CLOSED: the statement is aborted with the FRESH re-materialisation
 * error and the session keeps its previous anchor.
 *
 * The bundled SQLite amalgamation always returns SQLITE_OK for
 * SQLITE_FCNTL_RESET_CACHE (it silently no-ops inside a transaction), so the
 * failure cannot be forced through the SQLite API. therefore
 * adds a documented TEST-ONLY fault hook: OTSARDB_TEST_FAIL_RESET_CACHE=1
 * makes the reset report failure, exercising the fail-closed branch
 * deterministically. Production never sets it.
 *
 * Discrimination: with the hook ON the statement that would adopt a newer
 * generation FAILS (never a stale read); with the hook OFF the same statement
 * SUCCEEDS and returns the new row. Reverting the 0152 `return 0` makes the
 * MAIN assertion fail (the statement would succeed instead).
 *
 * Deterministic, no network, no sleeps: an in-memory store, two external
 * writers that advance the chain, and one FRESH session that must
 * re-materialise and, when the reset cannot be established, fail closed.
 *
 * Wiring (coordinator): NOT applied here; CMakeLists.txt is
 * out of scope for this change set):
 * add_executable(otsardb_fresh_reset_cache_test
 * tests/fresh_reset_cache_test.c tests/memory_store.c)
 * target_include_directories(otsardb_fresh_reset_cache_test
 * PRIVATE include src/core src/crypto src/storage tests)
 * target_link_libraries(otsardb_fresh_reset_cache_test
 * PRIVATE otsardb_core otsardb_s3_database)
 * add_test(NAME otsardb_fresh_reset_cache
 * COMMAND otsardb_fresh_reset_cache_test)
 * set_tests_properties(otsardb_fresh_reset_cache
 * PROPERTIES TIMEOUT 600)
 */
#include "otsardb.h"
#include "otsardb_internal.h"
#include "journal.h"
#include "local_vfs.h"
#include "memory_store.h"
#include "recovery.h"
#include "s3_database.h"
#include "wal_publisher.h"

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
#define otsardb_getpid _getpid
#define otsardb_setenv _putenv_s
#else
#include <sys/stat.h>
#include <unistd.h>
#define otsardb_unlink unlink
#define otsardb_mkdir(p) mkdir((p), 0755)
#define otsardb_getpid getpid
#define otsardb_setenv setenv
#endif

static const char *DB_ID = "0152-fresh-reset-db";
static const char *ROOT = "152/fresh-reset";
static const char *SOURCE = "0152-fm-source.db";

static void set_env(const char *name, const char *value) {
#if defined(_WIN32)
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1);
    else unsetenv(name);
#endif
}

static void remove_files(const char *path) {
    char sidecar[256];
    otsardb_unlink(path);
    snprintf(sidecar, sizeof(sidecar), "%s-wal", path);
    otsardb_unlink(sidecar);
    snprintf(sidecar, sizeof(sidecar), "%s-shm", path);
    otsardb_unlink(sidecar);
    snprintf(sidecar, sizeof(sidecar), "%s-journal", path);
    otsardb_unlink(sidecar);
}

static int collect_text(void *ctx, int argc, char **argv, char **columns) {
    (void)columns;
    if (argc != 1 || !argv || !argv[0]) return 1;
    snprintf((char *)ctx, 128, "%s", argv[0]);
    return 0;
}

/* Execute a statement that MUST FAIL. Returns the sqlite error message for
 * assertion, or NULL when there is none. */
static void exec_expect_fail(otsardb *db, const char *sql) {
    char *error_message = NULL;
    int rc = otsardb_exec(db, sql, NULL, NULL, &error_message);
    if (rc == OTSARDB_OK) {
        fprintf(stderr, "exec_expect_fail: statement unexpectedly succeeded: %s\n", sql);
    }
    assert(rc == OTSARDB_ERROR);
    assert(error_message != NULL);
    /* The FRESH fail-closed message must identify the re-materialisation. */
    assert(strstr(error_message, "re-materialisation") != NULL);
    fprintf(stderr, "[E3] refused as expected: %s\n", error_message);
    otsardb_free(error_message);
}

static void query_one(otsardb *db, const char *sql, char *out) {
    char *error_message = NULL;
    out[0] = '\0';
    int rc = otsardb_exec(db, sql, collect_text, out, &error_message);
    if (rc != OTSARDB_OK) {
        fprintf(stderr, "query_one failed (%d): %s\n", rc,
                error_message ? error_message : "");
    }
    otsardb_free(error_message);
    assert(rc == OTSARDB_OK);
}

static void exec_ok(otsardb *db, const char *sql) {
    char *error_message = NULL;
    int rc = otsardb_exec(db, sql, NULL, NULL, &error_message);
    if (rc != OTSARDB_OK) {
        fprintf(stderr, "exec_ok failed (%d): %s\n", rc,
                error_message ? error_message : "");
    }
    otsardb_free(error_message);
    assert(rc == OTSARDB_OK);
}

/* Publish one commit by driving a real SQLite connection through the WAL
 * publisher on the bare memory store (the "external writer"). */
static void writer_publish(otsardb_object_store *store,
                           const char *root,
                           const char *db_id,
                           const char *sql) {
    otsardb_wal_publisher publisher;
    assert(otsardb_wal_publisher_init(&publisher,
                                    store,
                                    root,
                                    db_id,
                                    OTSARDB_WRITE_STRATEGY_DEFAULT));
    otsardb_local_vfs_set_wal_reader_sync_hook(otsardb_wal_publisher_sync_reader, &publisher);

    otsardb_head head;
    char *etag = NULL;
    if (otsardb_journal_read_head(store, root, &head, &etag) == OTSARDB_JOURNAL_OK) {
        free(etag);
        assert(otsardb_recovery_restore_store(store, root, SOURCE));
    } else {
        free(etag);
    }

    otsardb *db = NULL;
    assert(otsardb_open(SOURCE, &db) == OTSARDB_OK);
    exec_ok(db, "PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0;");
    exec_ok(db, sql);
    otsardb_close(db);
    otsardb_local_vfs_set_wal_reader_sync_hook(NULL, NULL);
    remove_files(SOURCE);
}

static void open_session(otsardb_object_store *store, otsardb **out_db) {
    set_env("OTSARDB_CONSISTENCY", "fresh");
    int rc = otsardb_s3_open_with_store(store, 0, ROOT, DB_ID, NULL, out_db);
    if (rc != OTSARDB_OK) {
        fprintf(stderr, "open_session failed: %s\n", otsardb_s3_last_error());
    }
    assert(rc == OTSARDB_OK);
    set_env("OTSARDB_CONSISTENCY", NULL);
}

/* E3-MAIN: the FRESH session must FAIL the statement (never read stale
 * pages) when the page-cache reset cannot be established after the swap;
 * with the reset healthy it recovers and serves the new generation. */
static void test_fresh_reset_cache_fail_closed(void) {
    char cache_dir[512];
    snprintf(cache_dir, sizeof(cache_dir), "%s/otsardb-0152-reset-%d",
             getenv("TEMP") ? getenv("TEMP") : ".",
             (int)otsardb_getpid());
    otsardb_mkdir(cache_dir);
    set_env("OTSARDB_CACHE_DIR", cache_dir);
    /* Force the deterministic ladder (probe/delta skipped) so the swap always
     * reaches the page-cache reset. */
    set_env("OTSARDB_CACHE_REUSE", "0");
    set_env("OTSARDB_LAZY_HYDRATION", NULL);
    set_env("OTSARDB_AUTO_CHECKPOINT", "0");

    otsardb_object_store *store = otsardb_test_memory_store_create();
    assert(store != NULL);

    /* Chain: gen1 schema, gen2 first row. The FRESH session opens at gen2. */
    writer_publish(store, ROOT, DB_ID, "CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT);");
    writer_publish(store, ROOT, DB_ID, "INSERT INTO t(v) VALUES('one');");

    otsardb *db = NULL;
    open_session(store, &db);
    char result[128];
    query_one(db, "SELECT v FROM t WHERE id=1;", result);
    assert(strcmp(result, "one") == 0);

    /* External advance to gen3. */
    writer_publish(store, ROOT, DB_ID, "INSERT INTO t(v) VALUES('two');");

    /* E3-MAIN: force the reset failure. The statement that would adopt gen3
     * MUST fail closed — never run on stale pages. */
    set_env("OTSARDB_TEST_FAIL_RESET_CACHE", "1");
    exec_expect_fail(db, "SELECT v FROM t WHERE id=1;");
    /* Persistently fail-closed while the reset cannot be established. */
    exec_expect_fail(db, "SELECT count(*) FROM t;");

    /* E3-CONTROL: reset healthy again — the same statement succeeds and the
     * session self-heals to the new generation. */
    set_env("OTSARDB_TEST_FAIL_RESET_CACHE", "0");
    query_one(db, "SELECT v FROM t WHERE id=2;", result);
    assert(strcmp(result, "two") == 0);
    query_one(db, "SELECT count(*) FROM t;", result);
    assert(strcmp(result, "2") == 0);

    otsardb_close(db);
    otsardb_test_memory_store_destroy(store);
    remove_files(SOURCE);
    puts("passed");
}

int main(void) {
    set_env("OTSARDB_LAZY_PREFETCH", "0");
    set_env("OTSARDB_S3_RAM_CACHE_MB", "0");
    set_env("OTSARDB_S3_READ_STATS", "0");
    set_env("OTSARDB_PUBLISH_RETRIES", "1");
    set_env("OTSARDB_PUBLISH_RETRY_BASE_MS", "0");

    assert(otsardb_initialize() == OTSARDB_OK);

    test_fresh_reset_cache_fail_closed();

    puts("fresh-mode page-cache reset failure fails the statement closed (E3)");
    return 0;
}
