/* attach_denial_test.c — regression test for the failure-mode finding
 * security audit 0129 F08 (third recommendation): the durability authorizer
 * must deny ATTACH so Studio / loopback sessions cannot attach arbitrary
 * local files through SQL.
 *
 * Before 0152 the authorizer (s3_database.cpp durability_authorizer) only
 * denied the durability pragmas (journal_mode / synchronous /
 * wal_autocheckpoint). ATTACH was allowed, so a session could read or write
 * ANY local path the process user can access — e.g. `ATTACH
 * 'C:/Windows/...' AS x; SELECT * FROM x.foo;` — bypassing the s3:// default
 * of the Studio API (F08). After 0152 every ATTACH variant (file path,
 * ':memory:', encrypted, any aux name) is refused with SQLITE_AUTH.
 *
 * Discrimination: the ATTACH statements return OTSARDB_ERROR with a
 * "not authorized" sqlite message, while normal DDL/DML on the session
 * continue to succeed.
 *
 * Deterministic, no network: the session opens over the in-memory store via
 * otsardb_s3_open_with_store (the same open path that installs the
 * authorizer). No ATTACH file is ever touched: the authorizer refuses at
 * prepare time.
 *
 * Wiring (coordinator): NOT applied here; CMakeLists.txt is
 * out of scope for this change set):
 * add_executable(otsardb_attach_denial_test
 * tests/attach_denial_test.c tests/memory_store.c)
 * target_include_directories(otsardb_attach_denial_test
 * PRIVATE include src/core src/crypto src/storage tests)
 * target_link_libraries(otsardb_attach_denial_test
 * PRIVATE otsardb_core otsardb_s3_database)
 * add_test(NAME otsardb_attach_denial
 * COMMAND otsardb_attach_denial_test)
 * set_tests_properties(otsardb_attach_denial
 * PROPERTIES TIMEOUT 300)
 */
#include "otsardb.h"
#include "otsardb_internal.h"
#include "memory_store.h"
#include "s3_database.h"

#include <assert.h>
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

static const char *DB_ID = "0152-attach-denial-db";
static const char *ROOT = "152/attach-denial";

static void set_env(const char *name, const char *value) {
#if defined(_WIN32)
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1);
    else unsetenv(name);
#endif
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

static void exec_refused(otsardb *db, const char *sql) {
    char *error_message = NULL;
    int rc = otsardb_exec(db, sql, NULL, NULL, &error_message);
    if (rc == OTSARDB_OK) {
        fprintf(stderr, "exec_refused: statement unexpectedly succeeded: %s\n", sql);
    }
    assert(rc == OTSARDB_ERROR);
    assert(error_message != NULL);
    /* The refusal must be the durability authorizer's "not authorized". */
    assert(strstr(error_message, "not authorized") != NULL);
    fprintf(stderr, "[F08] refused as expected (%s): %s\n", error_message, sql);
    otsardb_free(error_message);
}

static int collect_text(void *ctx, int argc, char **argv, char **columns) {
    (void)columns;
    if (argc != 1 || !argv || !argv[0]) return 1;
    snprintf((char *)ctx, 128, "%s", argv[0]);
    return 0;
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

static void test_attach_denied_through_open_path(void) {
    char cache_dir[512];
    snprintf(cache_dir, sizeof(cache_dir), "%s/otsardb-0152-attach-%d",
             getenv("TEMP") ? getenv("TEMP") : ".",
             (int)otsardb_getpid());
    otsardb_mkdir(cache_dir);
    set_env("OTSARDB_CACHE_DIR", cache_dir);
    set_env("OTSARDB_AUTO_CHECKPOINT", "0");

    otsardb_object_store *store = otsardb_test_memory_store_create();
    assert(store != NULL);

    otsardb *db = NULL;
    int rc = otsardb_s3_open_with_store(store, 0, ROOT, DB_ID, NULL, &db);
    if (rc != OTSARDB_OK) {
        fprintf(stderr, "open failed: %s\n", otsardb_s3_last_error());
    }
    assert(rc == OTSARDB_OK);

    /* Normal DDL/DML on the session is unaffected. */
    exec_ok(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT);");
    exec_ok(db, "INSERT INTO t(v) VALUES('one');");
    exec_ok(db, "INSERT INTO t(v) VALUES('two');");
    exec_ok(db, "UPDATE t SET v='uno' WHERE id=1;");
    exec_ok(db, "DELETE FROM t WHERE id=2;");
    {
        char result[128];
        query_one(db, "SELECT count(*) FROM t;", result);
        assert(strcmp(result, "1") == 0);
        query_one(db, "SELECT v FROM t WHERE id=1;", result);
        assert(strcmp(result, "uno") == 0);
    }

    /* F08: every ATTACH variant is refused at prepare time. */
    exec_refused(db, "ATTACH ':memory:' AS aux;");
    exec_refused(db, "ATTACH 'C:/Windows/System32/drivers/etc/hosts' AS aux2;");
    exec_refused(db, "ATTACH '/tmp/otsardb-attach-attack.db' AS aux3;");
    exec_refused(db, "ATTACH 'attached-local.db' AS aux4;");
    exec_refused(db, "ATTACH 'encrypted.db' AS aux5 KEY 'secret';");
    exec_refused(db, "ATTACH DATABASE ':memory:' AS aux6;");

    /* The durability pragma denials still hold (regression). */
    exec_refused(db, "PRAGMA journal_mode=WAL;");
    exec_refused(db, "PRAGMA synchronous=NORMAL;");
    exec_refused(db, "PRAGMA wal_autocheckpoint=1000;");

    /* Normal DDL/DML still works after all refusals. */
    exec_ok(db, "INSERT INTO t(v) VALUES('three');");
    {
        char result[128];
        query_one(db, "SELECT count(*) FROM t;", result);
        assert(strcmp(result, "2") == 0);
    }

    otsardb_close(db);
    otsardb_test_memory_store_destroy(store);
    puts("passed");
}

int main(void) {
    assert(otsardb_initialize() == OTSARDB_OK);
    test_attach_denied_through_open_path();
    puts("durability authorizer denies ATTACH; normal DDL/DML unaffected (F08)");
    return 0;
}
