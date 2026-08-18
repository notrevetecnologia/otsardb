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
 * — VACUUM behavior test (deterministic, memory store).
 *
 * VACUUM has no dedicated engine support: it is executed by SQLite through
 * the same local VFS + WAL-publisher path as any statement, so its behavior
 * through OtsarDB's session layers was UNTESTED until now. This test pins
 * what the engine ACTUALLY does (observed, then asserted — never the other
 * way around):
 *
 * VAC1 read-write session VACUUM
 * A read-write session (wal_publisher + local VFS) creates a table and
 * inserts rows, then runs VACUUM. Assertions encode the observed
 * contract: the exit code, data intactness (count preserved in-session
 * AND after a fresh recovery from the store chain), and the segment
 * rewrite size (put bytes of the VACUUM commit vs a control single-row
 * commit, counted by the in-file store wrapper). The vacuum metrics
 * are printed for the record.
 *
 * VAC2 non-publishing session VACUUM (documented behavior)
 * A fully materialised lazy-restored session with a dummy
 * (non-publishing) sync reader runs VACUUM: SQLite rewrites the local
 * image in place and the statement succeeds, but nothing reaches the
 * authoritative store chain. Pinned as documented behavior — the local
 * rewrite stays local and the data stays intact.
 *
 * If VACUUM ever fails or behaves surprisingly in the engine, this test
 * documents that behavior as asserted facts — the engine is NOT changed by
 * this test file.
 */

static const char *SOURCE = "vac-source.db";
static const char *IMAGE = "vac-image.db";
static const char *DB_ID = "vac-behavior-db";
static const char *ROOT1 = "vac/rw";
static const char *ROOT2 = "vac/ro";
/* 400 rows with a deterministic 160-hex-char padding column so the image is
 * a few dozen pages and the VACUUM rewrite is measurably "large" against a
 * single-row control commit. */
static const char *ROWS_SQL =
    "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c WHERE x<400) "
    "INSERT INTO t(id, v) SELECT x, 'row-' || x || '-' || hex(zeroblob(80)) FROM c;";
static const int EXPECTED_ROWS = 400;

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

static void query_one(otsardb *db, const char *sql, char *out) {
    char *error_message = NULL;
    out[0] = '\0';
    assert(otsardb_exec(db, sql, collect_text, out, &error_message) == OTSARDB_OK);
    otsardb_free(error_message);
}

static void assert_count(otsardb *db, const char *table, int expected) {
    char sql[256];
    char result[128];
    char want[32];
    snprintf(sql, sizeof(sql), "SELECT count(*) FROM %s;", table);
    snprintf(want, sizeof(want), "%d", expected);
    query_one(db, sql, result);
    assert(strcmp(result, want) == 0);
}

/* ---- counting store wrapper (put bytes/put count) ------------------------- */

typedef struct count_ctx {
    otsardb_object_store *inner;
    size_t put_count;
    size_t put_bytes;
} count_ctx;

static count_ctx *count_ctx_of(otsardb_object_store *store) {
    return (count_ctx *)store->context;
}

static otsardb_store_result count_put(otsardb_object_store *store,
                                      const char *key,
                                      const void *data,
                                      size_t size,
                                      const otsardb_store_put_options *options,
                                      char **out_etag) {
    count_ctx *ctx = count_ctx_of(store);
    ++ctx->put_count;
    ctx->put_bytes += size;
    return otsardb_store_put(ctx->inner, key, data, size, options, out_etag);
}

static otsardb_store_result count_put_stream(otsardb_object_store *store,
                                             const char *key,
                                             otsardb_store_source_read source_read,
                                             void *source_ctx,
                                             uint64_t expected_size,
                                             const otsardb_store_put_options *options,
                                             char **out_etag) {
    count_ctx *ctx = count_ctx_of(store);
    ++ctx->put_count;
    ctx->put_bytes += (size_t)expected_size;
    return otsardb_store_put_stream(ctx->inner, key, source_read, source_ctx,
                                    expected_size, options, out_etag);
}

static const otsardb_object_store_ops COUNT_OPS = {
    .get = NULL, /* inherited via forwarding below */
    .put = count_put,
    .put_stream = count_put_stream,
};

static otsardb_store_result count_get(otsardb_object_store *store,
                                      const char *key,
                                      otsardb_store_object *out) {
    return otsardb_store_get(count_ctx_of(store)->inner, key, out);
}

static otsardb_store_result count_get_range(otsardb_object_store *store,
                                            const char *key,
                                            size_t offset,
                                            size_t length,
                                            otsardb_store_object *out) {
    return otsardb_store_get_range(count_ctx_of(store)->inner, key, offset, length, out);
}

static void count_release(otsardb_object_store *store, otsardb_store_object *object) {
    otsardb_store_release_object(count_ctx_of(store)->inner, object);
}

static otsardb_store_result count_delete(otsardb_object_store *store, const char *key) {
    return otsardb_store_delete(count_ctx_of(store)->inner, key);
}

static otsardb_store_result count_list(otsardb_object_store *store,
                                       const char *prefix,
                                       otsardb_store_list_callback callback,
                                       void *ctx) {
    return otsardb_store_list(count_ctx_of(store)->inner, prefix, callback, ctx);
}

static otsardb_object_store *count_store_create(otsardb_object_store *inner) {
    otsardb_object_store *store = (otsardb_object_store *)calloc(1, sizeof(*store));
    count_ctx *ctx = (count_ctx *)calloc(1, sizeof(*ctx));
    static otsardb_object_store_ops ops;
    ops = COUNT_OPS;
    ops.get = count_get;
    ops.get_range = count_get_range;
    ops.release_object = count_release;
    ops.delete_object = count_delete;
    ops.list = count_list;
    assert(store && ctx);
    ctx->inner = inner;
    store->ops = &ops;
    store->context = ctx;
    store->capabilities = inner->capabilities;
    return store;
}

static void count_store_destroy(otsardb_object_store *store) {
    if (!store) return;
    free(store->context);
    free(store);
}

static void count_reset(otsardb_object_store *store) {
    count_ctx_of(store)->put_count = 0;
    count_ctx_of(store)->put_bytes = 0;
}

static size_t count_put_bytes(otsardb_object_store *store) {
    return count_ctx_of(store)->put_bytes;
}

/* Publish one commit by driving a real SQLite connection through the WAL
 * publisher (the same path the S3 session uses for a write). The hook is
 * installed on a DEDICATED VFS instance (not the process-global hook) so a
 * writer never disturbs another open session's hook. */
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
    otsardb_local_vfs_instance *vfs = NULL;
    assert(otsardb_local_vfs_instance_create(otsardb_wal_publisher_sync_reader,
                                             &publisher,
                                             &vfs) == SQLITE_OK);

    otsardb_head head;
    char *etag = NULL;
    if (otsardb_journal_read_head(store, root, &head, &etag) == OTSARDB_JOURNAL_OK) {
        free(etag);
        assert(otsardb_recovery_restore_store(store, root, SOURCE));
    } else {
        free(etag);
    }

    otsardb *db = NULL;
    assert(otsardb_open_with_vfs_owned(SOURCE,
                                       otsardb_local_vfs_instance_name(vfs),
                                       NULL,
                                       NULL,
                                       &db) == OTSARDB_OK);
    exec_ok(db, "PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0;");
    exec_ok(db, sql);
    otsardb_close(db);
    otsardb_local_vfs_instance_destroy(vfs);
    remove_files(SOURCE);
}

/* ---- VAC1: read-write session VACUUM -------------------------------------- */

static void test_readwrite_vacuum(void) {
    remove_files(SOURCE);
    remove_files(IMAGE);
    otsardb_object_store *inner = otsardb_test_memory_store_create();
    assert(inner != NULL);
    otsardb_object_store *counting = count_store_create(inner);

    otsardb_wal_publisher publisher;
    assert(otsardb_wal_publisher_init(&publisher,
                                    counting,
                                    ROOT1,
                                    DB_ID,
                                    OTSARDB_WRITE_STRATEGY_DEFAULT));
    otsardb_local_vfs_set_wal_reader_sync_hook(otsardb_wal_publisher_sync_reader, &publisher);

    otsardb *db = NULL;
    assert(otsardb_open(SOURCE, &db) == OTSARDB_OK);
    exec_ok(db, "PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0;");
    exec_ok(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT);");
    exec_ok(db, ROWS_SQL);

    char result[128];
    assert_count(db, "t", EXPECTED_ROWS);
    otsardb_head head_before = read_head(counting, ROOT1);
    assert(head_before.generation >= 2);

    /* VACUUM through the session layers. */
    count_reset(counting);
    char *vac_err = NULL;
    int vac_rc = otsardb_exec(db, "VACUUM;", NULL, NULL, &vac_err);
    size_t vacuum_bytes = count_put_bytes(counting);

    /* OBSERVED 2026-08-13: VACUUM succeeds through the read-write session
     * and its rewrite is published as a full-image commit (see the metrics
     * below and the printed values). This assertion pins that observation. */
    assert(vac_rc == OTSARDB_OK);
    otsardb_free(vac_err);

    /* Data intact in-session after the rewrite. */
    assert_count(db, "t", EXPECTED_ROWS);
    query_one(db, "SELECT substr(v,1,5) FROM t WHERE id=7;", result);
    assert(strcmp(result, "row-7") == 0);

    /* Control: a single-row commit for the growth comparison. */
    count_reset(counting);
    exec_ok(db, "INSERT INTO t(id, v) VALUES(1001, 'ctl');");
    size_t control_bytes = count_put_bytes(counting);
    assert(control_bytes > 0);

    otsardb_head head_after = read_head(counting, ROOT1);
    assert(head_after.generation >= head_before.generation + 1);

    /* Segment growth: the VACUUM commit rewrites the whole database image,
     * so it publishes far more bytes than a single-row commit. OBSERVED
     * 2026-08-13 (see the printed metrics; the floor below is a
     * conservative pin of "large rewrite"). */
    fprintf(stderr,
            "vacuum_behavior: vacuum_rc=%d vacuum_put_bytes=%zu "
            "control_put_bytes=%zu ratio=%.1f\n",
            vac_rc, vacuum_bytes, control_bytes,
            (double)vacuum_bytes / (double)control_bytes);
    assert(vacuum_bytes >= control_bytes * 4);

    otsardb_close(db);
    otsardb_local_vfs_set_wal_reader_sync_hook(NULL, NULL);

    /* Durability: a fresh recovery from the store chain still holds all
     * rows (the rewrite must not have lost or corrupted data). */
    assert(otsardb_recovery_restore_store(counting, ROOT1, IMAGE));
    otsardb *recovered = NULL;
    assert(otsardb_open(IMAGE, &recovered) == OTSARDB_OK);
    assert_count(recovered, "t", EXPECTED_ROWS + 1);
    query_one(recovered, "SELECT v FROM t WHERE id=1001;", result);
    assert(strcmp(result, "ctl") == 0);
    otsardb_close(recovered);

    count_store_destroy(counting);
    otsardb_test_memory_store_destroy(inner);
    remove_files(SOURCE);
    remove_files(IMAGE);
}

/* ---- VAC2: VACUUM on a non-publishing session stays local ------------------
 * A fully materialised lazy-restored session with a dummy (non-publishing)
 * sync reader runs VACUUM: OBSERVED 2026-08-13 — SQLite rewrites the local
 * image in place and the statement succeeds, but nothing is published (the
 * hook is a no-op), so the store chain is untouched. Pinned as documented
 * behavior: the local rewrite never reaches the authoritative store. */

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

static void test_nonpublishing_vacuum_stays_local(void) {
    remove_files(SOURCE);
    remove_files(IMAGE);
    otsardb_object_store *inner = otsardb_test_memory_store_create();
    assert(inner != NULL);

    writer_publish(inner, ROOT2, DB_ID, "CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT);");
    writer_publish(inner, ROOT2, DB_ID, ROWS_SQL);

    otsardb_head head = read_head(inner, ROOT2);
    assert(head.generation >= 2);

    /* Fully materialised lazy-restored session (the 0120 composition:
     * lazy restore + dedicated local VFS + page hooks + dummy sync reader). */
    otsardb_lazy_hydrator *hydrator = NULL;
    otsardb_local_vfs_instance *vfs = NULL;
    otsardb *db = NULL;
    assert(otsardb_lazy_restore_store(inner, ROOT2, IMAGE, &head, &hydrator));
    assert(otsardb_local_vfs_instance_create(dummy_sync_reader, NULL, &vfs) == SQLITE_OK);
    assert(otsardb_local_vfs_instance_set_page_hooks(vfs,
                                                     page_hydrate,
                                                     page_mark,
                                                     page_truncate,
                                                     hydrator) == SQLITE_OK);
    assert(otsardb_lazy_hydrate_range(hydrator, 1, 4096) == 1);
    assert(otsardb_lazy_is_complete(hydrator));
    assert(otsardb_open_with_vfs_owned(IMAGE,
                                       otsardb_local_vfs_instance_name(vfs),
                                       NULL,
                                       NULL,
                                       &db) == OTSARDB_OK);

    /* VACUUM succeeds: SQLite rewrites the local image in place. */
    char *vac_err = NULL;
    int vac_rc = otsardb_exec(db, "VACUUM;", NULL, NULL, &vac_err);
    assert(vac_rc == OTSARDB_OK);
    otsardb_free(vac_err);

    /* The session stays usable and the data is intact. */
    assert_count(db, "t", EXPECTED_ROWS);

    /* The store was not modified: the non-publishing session's rewrite
     * never reached the chain (dummy sync reader publishes nothing). */
    otsardb_head head_after = read_head(inner, ROOT2);
    assert(head_after.generation == head.generation);
    assert(strcmp(head_after.commit_id, head.commit_id) == 0);

    otsardb_close(db);
    otsardb_local_vfs_instance_destroy(vfs);
    otsardb_lazy_destroy(hydrator);

    /* The rewritten local image is still a valid database on its own. */
    otsardb *plain = NULL;
    assert(otsardb_open(IMAGE, &plain) == OTSARDB_OK);
    assert_count(plain, "t", EXPECTED_ROWS);
    otsardb_close(plain);

    otsardb_test_memory_store_destroy(inner);
    remove_files(SOURCE);
    remove_files(IMAGE);
}

int main(void) {
    set_env("OTSARDB_LAZY_PREFETCH", "0");
    set_env("OTSARDB_S3_RAM_CACHE_MB", "0");
    set_env("OTSARDB_S3_READ_STATS", "0");
    set_env("OTSARDB_PUBLISH_RETRIES", "1");
    set_env("OTSARDB_PUBLISH_RETRY_BASE_MS", "0");

    assert(otsardb_initialize() == OTSARDB_OK);

    test_readwrite_vacuum();
    test_nonpublishing_vacuum_stays_local();

    puts("vacuum behavior (read-write rewrite + non-publishing local rewrite, "
         "data intact) passed");
    return 0;
}
