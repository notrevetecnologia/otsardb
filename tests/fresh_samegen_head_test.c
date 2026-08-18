/* fresh_samegen_head_test.c — regression tests for 
 * 0146, audit finding S3-2 (FRESH anchor generation-only comparison).
 *
 * The FRESH pre-exec hook compared only the remote HEAD GENERATION against
 * the anchor. A same-generation HEAD replacement — a different commit_id at
 * the SAME generation, produced by a restored/corrupted single-writer store
 * — did not trigger re-materialisation, so the session kept silently serving
 * the old branch's snapshot.
 *
 * Discrimination: this test swaps the open session's remote HEAD to a
 * DIFFERENT valid chain whose head is at the SAME generation, then runs a
 * statement. Before the statement returns the stale row
 * ('one'); after the fix it re-materialises and returns the new row ('two').
 *
 * Deterministic, no network, no sleeps: two real chains are published to an
 * in-memory store (memory_store) via the WAL publisher; the second chain's
 * objects are copied into the first root's namespace and HEAD is replaced —
 * exactly the restored-backup shape the audit describes.
 *
 * Wiring (coordinator): NOT applied here; CMakeLists.txt is
 * out of scope for this change set):
 * add_executable(otsardb_fresh_samegen_head_test
 * tests/fresh_samegen_head_test.c tests/memory_store.c)
 * target_include_directories(otsardb_fresh_samegen_head_test
 * PRIVATE include src/core src/crypto src/storage src/ha tests)
 * target_link_libraries(otsardb_fresh_samegen_head_test
 * PRIVATE otsardb_core otsardb_s3_database)
 * add_test(NAME otsardb_fresh_samegen_head
 * COMMAND otsardb_fresh_samegen_head_test)
 * set_tests_properties(otsardb_fresh_samegen_head
 * PROPERTIES TIMEOUT 600)
 */
#include "otsardb.h"
#include "otsardb_internal.h"
#include "commit_manifest.h"
#include "journal.h"
#include "local_vfs.h"
#include "memory_store.h"
#include "recovery.h"
#include "sha256.h"
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
#define otsardb_setenv _putenv_s
#define otsardb_getpid _getpid
#else
#include <sys/stat.h>
#include <unistd.h>
#define otsardb_unlink unlink
#define otsardb_mkdir(p) mkdir((p), 0755)
#define otsardb_setenv setenv
#define otsardb_getpid getpid
#endif

static const char *DB_ID = "0146-fresh-samegen-db";
static const char *ROOT_A = "146/freshA";
static const char *ROOT_B = "146/freshB";
static const char *SOURCE = "0146-fm-source.db";

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

static void query_one(otsardb *db, const char *sql, char *out) {
    char *error_message = NULL;
    out[0] = '\0';
    assert(otsardb_exec(db, sql, collect_text, out, &error_message) == OTSARDB_OK);
    otsardb_free(error_message);
}

static void exec_ok(otsardb *db, const char *sql) {
    char *error_message = NULL;
    assert(otsardb_exec(db, sql, NULL, NULL, &error_message) == OTSARDB_OK);
    otsardb_free(error_message);
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

/* Copy the commits/ and segments/ objects of the source root into the
 * destination root (same relative keys). The restored-backup model: chain B
 * becomes addressable under root A's namespace. */
typedef struct copy_ctx {
    otsardb_object_store *store;
    const char *src_root;
    const char *dst_root;
} copy_ctx;

static int copy_cb(const char *logical_key, void *opaque) {
    copy_ctx *ctx = (copy_ctx *)opaque;
    if (strncmp(logical_key, "commits/", 8) != 0 &&
        strncmp(logical_key, "segments/", 9) != 0) {
        return 0;
    }
    char src_key[OTSARDB_KEY_MAX + 1];
    char dst_key[OTSARDB_KEY_MAX + 1];
    int n = snprintf(src_key, sizeof(src_key), "%s/%s", ctx->src_root, logical_key);
    assert(n > 0 && (size_t)n < sizeof(src_key));
    n = snprintf(dst_key, sizeof(dst_key), "%s/%s", ctx->dst_root, logical_key);
    assert(n > 0 && (size_t)n < sizeof(dst_key));

    otsardb_store_object obj = {0};
    if (otsardb_store_get(ctx->store, src_key, &obj) != OTSARDB_STORE_OK) return 1;
    otsardb_store_result rc =
        otsardb_store_put(ctx->store, dst_key, obj.data, obj.size, NULL, NULL);
    otsardb_store_release_object(ctx->store, &obj);
    return rc == OTSARDB_STORE_OK ? 0 : 1;
}

/* Replace root A's HEAD with root B's head at the SAME generation (a
 * different commit_id). The copied chain B (commits + segments) lives under
 * root A, so the session re-materialises to chain B's state. */
static void swap_head_same_generation(otsardb_object_store *store) {
    char src_prefix[OTSARDB_KEY_MAX + 1];
    int n = snprintf(src_prefix, sizeof(src_prefix), "%s/", ROOT_B);
    assert(n > 0 && (size_t)n < sizeof(src_prefix));
    copy_ctx c;
    c.store = store;
    c.src_root = ROOT_B;
    c.dst_root = ROOT_A;
    assert(otsardb_store_list(store, src_prefix, copy_cb, &c) == OTSARDB_STORE_OK);

    otsardb_head head_b;
    char *etag = NULL;
    assert(otsardb_journal_read_head(store, ROOT_B, &head_b, &etag) == OTSARDB_JOURNAL_OK);
    free(etag);
    assert(head_b.generation >= 2);
    assert(strlen(head_b.commit_sha256) == OTSARDB_MANIFEST_SHA256_HEX_SIZE);

    otsardb_head new_head;
    memset(&new_head, 0, sizeof(new_head));
    new_head.format_version = head_b.format_version;
    snprintf(new_head.database_id, sizeof(new_head.database_id), "%s", DB_ID);
    new_head.generation = head_b.generation;
    snprintf(new_head.commit_id, sizeof(new_head.commit_id), "%s", head_b.commit_id);
    snprintf(new_head.commit_key, sizeof(new_head.commit_key), "%s/commits/%s.json",
             ROOT_A, head_b.commit_id);
    snprintf(new_head.commit_sha256, sizeof(new_head.commit_sha256), "%s",
             head_b.commit_sha256);

    char *json = NULL;
    size_t json_size = 0;
    assert(otsardb_head_encode_json(&new_head, &json, &json_size));
    char head_key[OTSARDB_KEY_MAX + 1];
    n = snprintf(head_key, sizeof(head_key), "%s/HEAD.json", ROOT_A);
    assert(n > 0 && (size_t)n < sizeof(head_key));
    assert(otsardb_store_put(store, head_key, json, json_size, NULL, NULL) ==
           OTSARDB_STORE_OK);
    free(json);
}

static void open_session(otsardb_object_store *store,
                         const char *root,
                         const char *consistency,
                         otsardb **out_db) {
    set_env("OTSARDB_CONSISTENCY", consistency);
    int rc = otsardb_s3_open_with_store(store, 0, root, DB_ID, NULL, out_db);
    if (rc != OTSARDB_OK) {
        fprintf(stderr, "open_session failed: %s\n", otsardb_s3_last_error());
    }
    assert(rc == OTSARDB_OK);
    set_env("OTSARDB_CONSISTENCY", NULL);
}

static void test_fresh_same_generation_head_replaced(void) {
    char cache_dir[512];
    snprintf(cache_dir, sizeof(cache_dir), "%s/otsardb-0146-samegen-%d",
             getenv("TEMP") ? getenv("TEMP") : ".",
             (int)otsardb_getpid());
    otsardb_mkdir(cache_dir);
    set_env("OTSARDB_CACHE_DIR", cache_dir);
    set_env("OTSARDB_LAZY_HYDRATION", NULL);

    otsardb_object_store *store = otsardb_test_memory_store_create();
    assert(store != NULL);

    /* Chain A: the session opens against this chain. */
    writer_publish(store, ROOT_A, DB_ID, "CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT);");
    writer_publish(store, ROOT_A, DB_ID, "INSERT INTO t(v) VALUES('one');");

    /* Chain B: a DIFFERENT chain at the same final generation. */
    writer_publish(store, ROOT_B, DB_ID, "CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT);");
    writer_publish(store, ROOT_B, DB_ID, "INSERT INTO t(v) VALUES('two');");

    otsardb *db = NULL;
    open_session(store, ROOT_A, "fresh", &db);
    char result[128];
    query_one(db, "SELECT v FROM t WHERE id=1;", result);
    assert(strcmp(result, "one") == 0);

    /* Same-generation HEAD replacement (restored-backup model). */
    swap_head_same_generation(store);

    /* The next statement MUST re-materialise to the replaced head: it must
     * NOT keep serving the old branch's 'one'. */
    query_one(db, "SELECT v FROM t WHERE id=1;", result);
    assert(strcmp(result, "two") == 0);

    /* And it keeps serving the adopted state. */
    query_one(db, "SELECT count(*) FROM t;", result);
    assert(strcmp(result, "1") == 0);

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

    test_fresh_same_generation_head_replaced();

    puts("fresh-mode same-generation HEAD replacement re-materialises (S3-2)");
    return 0;
}
