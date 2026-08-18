#include "otsardb.h"
#include "otsardb_internal.h"
#include "journal.h"
#include "lazy_hydration.h"
#include "local_vfs.h"
#include "memory_store.h"
#include "recovery.h"
#include "sqlite3.h"
#include "wal_publisher.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#define otsardb_unlink _unlink
#define otsardb_setenv _putenv_s
#else
#include <unistd.h>
#define otsardb_unlink unlink
#define otsardb_setenv setenv
#endif

/*
 * — session-snapshot read-consistency contract (deterministic).
 *
 * The real S3 session (otsardb_s3_open_target_ex in s3_database.cpp) builds its
 * own store from the environment, so the read-consistency contract cannot be
 * unit-proven with a memory/fault store through that entry point. These tests
 * therefore drive the SAME lower layers the session composes — wal_publisher +
 * recovery/lazy-hydration materialisation + the local VFS read path — over a
 * counting/faulting memory store, which is exactly how
 * lazy_hydration_test.c / wal_publisher_local_image_test.c / multi_database_vfs_test.c
 * exercise the engine without S3.
 *
 * Each assertion is tagged with the layer it targets:
 *
 * S1 hot SELECT == 0 store requests [lazy-hydration read path]
 * Open a read-only session on a COUNTING memory store, fully materialise
 * the image, then run SELECTs: the store's get / get_range / HEAD-get
 * counters must NOT advance. A hot read is purely local.
 *
 * S2 session snapshot pinned at open [lazy-hydration session + wal_publisher]
 * Open a read-only session at generation G, then let an EXTERNAL writer
 * (a second publisher over the SAME memory store) advance the store to
 * G+1. The open session must still read G's data and must NOT see G+1.
 *
 * S3 read-your-writes [wal_publisher + local image]
 * One read-write session creates a table, inserts a row (which publishes
 * a commit through the WAL publisher) and reads it back in the SAME
 * session, observing its own row.
 *
 * S4 reopen picks latest [recovery/lazy-hydration fresh read path]
 * After an external write advances the store to G+1, a FRESH session
 * (fresh hydrator) materialises and reads the latest state, observing
 * the G+1 row (the full/lazy materialisation recovery path; the cached
 * delta variant is covered by delta_session_e2e at).
 *
 * S5 store down after open [lazy-hydration read path + fault store]
 * Materialise the image, then put the store into a "down" state that
 * fails every subsequent request. Hot SELECT still returns the correct
 * rows because the pages are already local.
 *
 * Deterministic knobs: prefetch (OTSARDB_LAZY_PREFETCH=0) and the RAM page
 * cache (OTSARDB_S3_RAM_CACHE_MB=0) are disabled so the read path performs no
 * background work that could race the counter assertions.
 */

static const char *SOURCE = "sc-source.db";
static const char *IMAGE = "sc-image.db";
static const char *DB_ID = "sc-consistency-db";
static const char *ROOT1 = "sc/hot-select";
static const char *ROOT2 = "sc/snapshot";
static const char *ROOT3 = "sc/read-your-writes";
static const char *ROOT4 = "sc/reopen";
static const char *ROOT5 = "sc/down";

static void set_env(const char *name, const char *value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
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

static otsardb_head read_head(otsardb_object_store *store, const char *root) {
    otsardb_head head;
    char *etag = NULL;
    assert(otsardb_journal_read_head(store, root, &head, &etag) == OTSARDB_JOURNAL_OK);
    free(etag);
    return head;
}

static int collect_text(void *ctx, int argc, char **argv, char **columns) {
    (void)columns;
    if (argc != 1 || !argv || !argv[0]) return 1;
    snprintf((char *)ctx, 128, "%s", argv[0]);
    return 0;
}

static void exec_ok(otsardb *db, const char *sql) {
    char *error_message = NULL;
    assert(otsardb_exec(db, sql, NULL, NULL, &error_message) == OTSARDB_OK);
    otsardb_free(error_message);
}

/* Publish one commit by driving a real SQLite connection through the WAL
 * publisher (the same path the S3 session uses for a write). Materialises the
 * current remote state first, exactly like a real session open. */
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

/* Dummy sync reader: the read-only session never publishes. */
static int dummy_sync_reader(const char *wal_name,
                             uint64_t wal_size,
                             otsardb_sqlite_wal_read_at read_at,
                             void *read_ctx,
                             otsardb_sqlite_wal_read_at db_read_at,
                             void *db_read_ctx,
                             void *ctx) {
    (void)wal_name; (void)wal_size; (void)read_at; (void)read_ctx;
    (void)db_read_at; (void)db_read_ctx; (void)ctx;
    return SQLITE_OK;
}

static int page_hydrate(void *ctx, long long offset, int amount) {
    return otsardb_lazy_hydrate_bytes((otsardb_lazy_hydrator *)ctx, offset, amount)
               ? SQLITE_OK
               : SQLITE_IOERR_READ;
}

static void page_mark(void *ctx, long long offset, int amount) {
    otsardb_lazy_mark_bytes((otsardb_lazy_hydrator *)ctx, offset, amount);
}

static void page_truncate(void *ctx, long long new_size) {
    otsardb_lazy_clear_beyond_bytes((otsardb_lazy_hydrator *)ctx, new_size);
}

/* Counting + fault ("down") store wrapper over a memory store. Counts whole
 * object GETs (and HEAD GETs specifically) and byte-range GETs, and can be
 * toggled into a "down" state that fails every request. */
typedef struct watch_ctx {
    otsardb_object_store *inner;
    size_t get_count;
    size_t get_range_count;
    size_t head_get_count;
    int down;
} watch_ctx;

static watch_ctx *watch_ctx_of(otsardb_object_store *store) {
    return (watch_ctx *)store->context;
}

static otsardb_store_result watch_get(otsardb_object_store *store,
                                     const char *key,
                                     otsardb_store_object *out) {
    watch_ctx *ctx = watch_ctx_of(store);
    ++ctx->get_count;
    if (key && strstr(key, "HEAD.json") != NULL) ++ctx->head_get_count;
    if (ctx->down) return OTSARDB_STORE_ERROR;
    return otsardb_store_get(ctx->inner, key, out);
}

static otsardb_store_result watch_get_range(otsardb_object_store *store,
                                           const char *key,
                                           size_t offset,
                                           size_t length,
                                           otsardb_store_object *out) {
    watch_ctx *ctx = watch_ctx_of(store);
    ++ctx->get_range_count;
    if (ctx->down) return OTSARDB_STORE_ERROR;
    return otsardb_store_get_range(ctx->inner, key, offset, length, out);
}

static otsardb_store_result watch_put(otsardb_object_store *store,
                                     const char *key,
                                     const void *data,
                                     size_t size,
                                     const otsardb_store_put_options *options,
                                     char **out_etag) {
    watch_ctx *ctx = watch_ctx_of(store);
    if (ctx->down) return OTSARDB_STORE_ERROR;
    return otsardb_store_put(ctx->inner, key, data, size, options, out_etag);
}

static void watch_release(otsardb_object_store *store, otsardb_store_object *object) {
    otsardb_store_release_object(watch_ctx_of(store)->inner, object);
}

static otsardb_store_result watch_delete(otsardb_object_store *store, const char *key) {
    watch_ctx *ctx = watch_ctx_of(store);
    if (ctx->down) return OTSARDB_STORE_ERROR;
    return otsardb_store_delete(ctx->inner, key);
}

static otsardb_store_result watch_list(otsardb_object_store *store,
                                      const char *prefix,
                                      otsardb_store_list_callback callback,
                                      void *ctx) {
    watch_ctx *w = watch_ctx_of(store);
    if (w->down) return OTSARDB_STORE_ERROR;
    return otsardb_store_list(w->inner, prefix, callback, ctx);
}

static const otsardb_object_store_ops WATCH_OPS = {
    .get = watch_get,
    .get_range = watch_get_range,
    .put = watch_put,
    .release_object = watch_release,
    .delete_object = watch_delete,
    .list = watch_list,
};

static otsardb_object_store *watch_store_create(otsardb_object_store *inner) {
    otsardb_object_store *store = (otsardb_object_store *)calloc(1, sizeof(*store));
    watch_ctx *ctx = (watch_ctx *)calloc(1, sizeof(*ctx));
    assert(store && ctx);
    ctx->inner = inner;
    store->ops = &WATCH_OPS;
    store->context = ctx;
    store->capabilities = inner->capabilities;
    return store;
}

static void watch_store_destroy(otsardb_object_store *store) {
    if (!store) return;
    free(store->context);
    free(store);
}

static void watch_set_down(otsardb_object_store *store, int down) {
    watch_ctx_of(store)->down = down;
}

static size_t watch_get_count(otsardb_object_store *store) {
    return watch_ctx_of(store)->get_count;
}

static size_t watch_get_range_count(otsardb_object_store *store) {
    return watch_ctx_of(store)->get_range_count;
}

static size_t watch_head_get_count(otsardb_object_store *store) {
    return watch_ctx_of(store)->head_get_count;
}

/* Open a read-only session over `store`: lazy-restore the image at the given
 * head and open a dedicated VFS instance with the page hooks installed. */
static void session_open_readonly(otsardb_object_store *store,
                                  const char *root,
                                  const otsardb_head *head,
                                  otsardb_lazy_hydrator **out_hydrator,
                                  otsardb_local_vfs_instance **out_vfs,
                                  otsardb **out_db) {
    assert(otsardb_lazy_restore_store(store, root, IMAGE, head, out_hydrator));
    assert(otsardb_local_vfs_instance_create(dummy_sync_reader, NULL, out_vfs) == SQLITE_OK);
    assert(otsardb_local_vfs_instance_set_page_hooks(*out_vfs,
                                                     page_hydrate,
                                                     page_mark,
                                                     page_truncate,
                                                     *out_hydrator) == SQLITE_OK);
    assert(otsardb_open_with_vfs_owned(IMAGE,
                                       otsardb_local_vfs_instance_name(*out_vfs),
                                       NULL,
                                       NULL,
                                       out_db) == OTSARDB_OK);
}

static void session_close(otsardb *db,
                          otsardb_local_vfs_instance *vfs,
                          otsardb_lazy_hydrator *hydrator) {
    otsardb_close(db);
    otsardb_local_vfs_instance_destroy(vfs);
    otsardb_lazy_destroy(hydrator);
}

static void query_one(otsardb *db, const char *sql, char *out) {
    char *error_message = NULL;
    out[0] = '\0';
    assert(otsardb_exec(db, sql, collect_text, out, &error_message) == OTSARDB_OK);
    otsardb_free(error_message);
}

/* ---- S1: hot SELECT does not hit the store ------------------------------- */

static void test_hot_select_zero_store_hits(void) {
    remove_files(SOURCE);
    remove_files(IMAGE);
    otsardb_object_store *inner = otsardb_test_memory_store_create();
    assert(inner != NULL);
    otsardb_object_store *watch = watch_store_create(inner);

    writer_publish(inner, ROOT1, DB_ID, "CREATE TABLE items(id INTEGER PRIMARY KEY, v TEXT);");
    writer_publish(inner, ROOT1, DB_ID, "INSERT INTO items(v) VALUES('a'),('b'),('c');");

    otsardb_head head = read_head(inner, ROOT1);
    otsardb_lazy_hydrator *hydrator = NULL;
    otsardb_local_vfs_instance *vfs = NULL;
    otsardb *db = NULL;
    session_open_readonly(watch, ROOT1, &head, &hydrator, &vfs, &db);

    /* Materialise the whole image while the store is up. */
    assert(otsardb_lazy_hydrate_range(hydrator, 1, 4096) == 1);
    assert(otsardb_lazy_is_complete(hydrator));

    size_t gets_before = watch_get_count(watch);
    size_t range_before = watch_get_range_count(watch);
    size_t head_before = watch_head_get_count(watch);

    char result[128];
    query_one(db, "SELECT count(*) FROM items;", result);
    assert(strcmp(result, "3") == 0);
    query_one(db, "SELECT v FROM items WHERE id=2;", result);
    assert(strcmp(result, "b") == 0);

    /* Hot reads are local: the store saw ZERO new requests. */
    assert(watch_get_count(watch) == gets_before);
    assert(watch_get_range_count(watch) == range_before);
    assert(watch_head_get_count(watch) == head_before);

    session_close(db, vfs, hydrator);
    watch_store_destroy(watch);
    otsardb_test_memory_store_destroy(inner);
    remove_files(IMAGE);
    remove_files(SOURCE);
}

/* ---- S2: session snapshot pinned at open --------------------------------- */

static void test_session_snapshot_pinned(void) {
    remove_files(SOURCE);
    remove_files(IMAGE);
    otsardb_object_store *inner = otsardb_test_memory_store_create();
    assert(inner != NULL);

    writer_publish(inner, ROOT2, DB_ID, "CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT);");
    writer_publish(inner, ROOT2, DB_ID, "INSERT INTO t(v) VALUES('one');");

    otsardb_head head_g = read_head(inner, ROOT2);
    assert(head_g.generation >= 1);

    otsardb_lazy_hydrator *hydrator = NULL;
    otsardb_local_vfs_instance *vfs = NULL;
    otsardb *db = NULL;
    session_open_readonly(inner, ROOT2, &head_g, &hydrator, &vfs, &db);

    /* External write: a second publisher over the SAME memory store advances
     * the store to G+1 while the session stays open. */
    writer_publish(inner, ROOT2, DB_ID, "INSERT INTO t(v) VALUES('two');");
    otsardb_head head_after = read_head(inner, ROOT2);
    assert(head_after.generation == head_g.generation + 1);

    /* The open session still reads G's data and does NOT see the G+1 row. */
    char result[128];
    query_one(db, "SELECT count(*) FROM t;", result);
    assert(strcmp(result, "1") == 0);
    query_one(db, "SELECT count(*) FROM t WHERE v='two';", result);
    assert(strcmp(result, "0") == 0);
    query_one(db, "SELECT v FROM t;", result);
    assert(strcmp(result, "one") == 0);

    session_close(db, vfs, hydrator);
    otsardb_test_memory_store_destroy(inner);
    remove_files(IMAGE);
    remove_files(SOURCE);
}

/* ---- S3: read-your-writes ------------------------------------------------ */

static void test_read_your_writes(void) {
    remove_files(SOURCE);
    otsardb_object_store *inner = otsardb_test_memory_store_create();
    assert(inner != NULL);

    otsardb_wal_publisher publisher;
    assert(otsardb_wal_publisher_init(&publisher,
                                    inner,
                                    ROOT3,
                                    DB_ID,
                                    OTSARDB_WRITE_STRATEGY_DEFAULT));

    otsardb_local_vfs_instance *vfs = NULL;
    assert(otsardb_local_vfs_instance_create(otsardb_wal_publisher_sync_reader,
                                             &publisher,
                                             &vfs) == SQLITE_OK);
    otsardb *db = NULL;
    assert(otsardb_open_with_vfs_owned(SOURCE,
                                       otsardb_local_vfs_instance_name(vfs),
                                       NULL,
                                       NULL,
                                       &db) == OTSARDB_OK);
    exec_ok(db, "PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0;");
    exec_ok(db, "CREATE TABLE rw(id INTEGER PRIMARY KEY, v TEXT);");
    exec_ok(db, "INSERT INTO rw(v) VALUES('mine');");

    char result[128];
    query_one(db, "SELECT v FROM rw WHERE id=1;", result);
    assert(strcmp(result, "mine") == 0);

    otsardb_close(db);
    otsardb_local_vfs_instance_destroy(vfs);

    otsardb_head head = read_head(inner, ROOT3);
    assert(head.generation >= 2);

    otsardb_test_memory_store_destroy(inner);
    remove_files(SOURCE);
}

/* ---- S4: reopen picks latest --------------------------------------------- */

static void test_reopen_picks_latest(void) {
    remove_files(SOURCE);
    remove_files(IMAGE);
    otsardb_object_store *inner = otsardb_test_memory_store_create();
    assert(inner != NULL);

    writer_publish(inner, ROOT4, DB_ID, "CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT);");
    writer_publish(inner, ROOT4, DB_ID, "INSERT INTO t(v) VALUES('one');");
    otsardb_head head_g = read_head(inner, ROOT4);

    writer_publish(inner, ROOT4, DB_ID, "INSERT INTO t(v) VALUES('two');");
    otsardb_head head_latest = read_head(inner, ROOT4);
    assert(head_latest.generation == head_g.generation + 1);

    /* A FRESH session (fresh hydrator) materialises the latest state. */
    otsardb_lazy_hydrator *hydrator = NULL;
    otsardb_local_vfs_instance *vfs = NULL;
    otsardb *db = NULL;
    session_open_readonly(inner, ROOT4, &head_latest, &hydrator, &vfs, &db);

    char result[128];
    query_one(db, "SELECT count(*) FROM t;", result);
    assert(strcmp(result, "2") == 0);
    query_one(db, "SELECT count(*) FROM t WHERE v='two';", result);
    assert(strcmp(result, "1") == 0);

    session_close(db, vfs, hydrator);
    otsardb_test_memory_store_destroy(inner);
    remove_files(IMAGE);
    remove_files(SOURCE);
}

/* ---- S5: store down after open ------------------------------------------- */

static void test_store_down_after_open(void) {
    remove_files(SOURCE);
    remove_files(IMAGE);
    otsardb_object_store *inner = otsardb_test_memory_store_create();
    assert(inner != NULL);
    otsardb_object_store *watch = watch_store_create(inner);

    writer_publish(inner, ROOT5, DB_ID, "CREATE TABLE items(id INTEGER PRIMARY KEY, v TEXT);");
    writer_publish(inner, ROOT5, DB_ID, "INSERT INTO items(v) VALUES('x'),('y'),('z');");

    otsardb_head head = read_head(inner, ROOT5);
    otsardb_lazy_hydrator *hydrator = NULL;
    otsardb_local_vfs_instance *vfs = NULL;
    otsardb *db = NULL;
    session_open_readonly(watch, ROOT5, &head, &hydrator, &vfs, &db);

    /* Materialise fully, then bring the store down before any SELECT. */
    assert(otsardb_lazy_hydrate_range(hydrator, 1, 4096) == 1);
    assert(otsardb_lazy_is_complete(hydrator));
    watch_set_down(watch, 1);

    char result[128];
    query_one(db, "SELECT count(*) FROM items;", result);
    assert(strcmp(result, "3") == 0);
    query_one(db, "SELECT v FROM items WHERE id=2;", result);
    assert(strcmp(result, "y") == 0);

    session_close(db, vfs, hydrator);
    watch_store_destroy(watch);
    otsardb_test_memory_store_destroy(inner);
    remove_files(IMAGE);
    remove_files(SOURCE);
}

int main(void) {
    /* Deterministic read path + no background work racing the counters. */
    set_env("OTSARDB_LAZY_PREFETCH", "0");
    set_env("OTSARDB_S3_RAM_CACHE_MB", "0");
    set_env("OTSARDB_S3_READ_STATS", "0");
    set_env("OTSARDB_PUBLISH_RETRIES", "1");
    set_env("OTSARDB_PUBLISH_RETRY_BASE_MS", "0");

    assert(otsardb_initialize() == OTSARDB_OK);

    test_hot_select_zero_store_hits();
    test_session_snapshot_pinned();
    test_read_your_writes();
    test_reopen_picks_latest();
    test_store_down_after_open();

    puts("session-snapshot read-consistency contract (hot reads local, pinned at "
         "open, read-your-writes, reopen-latest, store-down reads) passed");
    return 0;
}
