#include "otsardb.h"
#include "checkpoint_build.h"
#include "checkpoint_manifest.h"
#include "commit_manifest.h"
#include "db_metadata.h"
#include "journal.h"
#include "lazy_hydration.h"
#include "local_vfs.h"
#include "memory_store.h"
#include "otsardb_internal.h"
#include "recovery.h"
#include "segment.h"
#include "sqlite3.h"
#include "wal_batch.h"
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

static const char *SOURCE = "lazy-source.db";
static const char *WAL_PATH = "lazy-source.db-wal";
static const char *SHM_PATH = "lazy-source.db-shm";
static const char *LAZY_IMAGE = "lazy-image.db";
static const char *FULL_IMAGE = "lazy-full.db";
static const char *ROOT = "lazy/orders";
static const char *ROOT2 = "lazy/deleted";
static const char *DB_ID = "lazy-orders-db";
static const char *DB_ID2 = "lazy-deleted-db";

static void cleanup_files(void) {
    otsardb_unlink(SOURCE);
    otsardb_unlink(WAL_PATH);
    otsardb_unlink(SHM_PATH);
    otsardb_unlink(LAZY_IMAGE);
    otsardb_unlink(FULL_IMAGE);
}

static otsardb_head read_head(otsardb_object_store *store, const char *root) {
    otsardb_head head;
    char *etag = NULL;
    assert(otsardb_journal_read_head(store, root, &head, &etag) == OTSARDB_JOURNAL_OK);
    free(etag);
    return head;
}

static int read_file_bytes(const char *path, unsigned char **out, size_t *out_size) {
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    assert(fseek(file, 0, SEEK_END) == 0);
    long size = ftell(file);
    assert(size >= 0);
    assert(fseek(file, 0, SEEK_SET) == 0);
    unsigned char *data = (unsigned char *)malloc((size_t)size ? (size_t)size : 1u);
    assert(data);
    assert(fread(data, 1, (size_t)size, file) == (size_t)size);
    fclose(file);
    *out = data;
    *out_size = (size_t)size;
    return 1;
}

/* Publish one commit by driving a real SQLite connection through the WAL
 * publisher (the same path the S3 session uses). Each call materialises the
 * current remote state into the local execution file first, exactly like a
 * real session open. */
static void publish_commit(otsardb_object_store *store,
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
    char *error_message = NULL;
    assert(otsardb_exec(db,
                      "PRAGMA journal_mode=WAL;"
                      "PRAGMA wal_autocheckpoint=0;",
                      NULL,
                      NULL,
                      &error_message) == OTSARDB_OK);
    otsardb_free(error_message);
    error_message = NULL;
    assert(otsardb_exec(db, sql, NULL, NULL, &error_message) == OTSARDB_OK);
    otsardb_free(error_message);
    otsardb_close(db);
    /* Clear the global VFS hook: the publisher is a stack local and must
     * never be called after this function returns. */
    otsardb_local_vfs_set_wal_reader_sync_hook(NULL, NULL);
    otsardb_unlink(SOURCE);
    otsardb_unlink(WAL_PATH);
    otsardb_unlink(SHM_PATH);
}

/* Dummy sync reader: the lazy session is read-only, nothing ever syncs. */
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

static int collect_text(void *ctx, int argc, char **argv, char **columns) {
    (void)columns;
    if (argc != 1 || !argv || !argv[0]) return 1;
    snprintf((char *)ctx, 128, "%s", argv[0]);
    return 0;
}

/* Open the lazy image through SQLite with the page hooks installed and run
 * a real SELECT; asserts the row count. */
static void query_through_sqlite(otsardb_object_store *store,
                                 const char *root,
                                 const char *image_path,
                                 const char *sql,
                                 const char *expected) {
    otsardb_head head = read_head(store, root);
    otsardb_lazy_hydrator *hydrator = NULL;
    assert(otsardb_lazy_restore_store(store, root, image_path, &head, &hydrator));

    otsardb_local_vfs_instance *vfs = NULL;
    assert(otsardb_local_vfs_instance_create(dummy_sync_reader, NULL, &vfs) == SQLITE_OK);
    assert(otsardb_local_vfs_instance_set_page_hooks(vfs,
                                                   page_hydrate,
                                                   page_mark,
                                                   page_truncate,
                                                   hydrator) == SQLITE_OK);

    otsardb *db = NULL;
    assert(otsardb_open_with_vfs_owned(image_path,
                                     otsardb_local_vfs_instance_name(vfs),
                                     NULL,
                                     NULL,
                                     &db) == OTSARDB_OK);
    char result[128] = {0};
    char *error_message = NULL;
    assert(otsardb_exec(db, sql, (otsardb_row_callback)collect_text, result, &error_message) == OTSARDB_OK);
    otsardb_free(error_message);
    assert(strcmp(result, expected) == 0);
    otsardb_close(db);
    otsardb_local_vfs_instance_destroy(vfs);
    otsardb_lazy_destroy(hydrator);
}

/* Corrupt the newest segment object's FIRST frame block (
 * the frame region is what a byte-range fetch reads, so corrupting it must
 * fail the sliced fetch closed). The corruption flips a byte of the block's
 * orig_len header field. */
static void corrupt_head_segment(otsardb_object_store *store, const char *root, const otsardb_head *head) {
    otsardb_store_object manifest_obj = {0};
    assert(otsardb_store_get(store, head->commit_key, &manifest_obj) == OTSARDB_STORE_OK);
    otsardb_commit_manifest manifest;
    assert(otsardb_commit_manifest_decode_json((const char *)manifest_obj.data,
                                             manifest_obj.size,
                                             &manifest));
    char segment_key[OTSARDB_KEY_MAX + 1];
    snprintf(segment_key, sizeof(segment_key), "%s/%s", root, manifest.segments[0].key);
    otsardb_store_object segment_obj = {0};
    assert(otsardb_store_get(store, segment_key, &segment_obj) == OTSARDB_STORE_OK);
    otsardb_segment_view view;
    assert(otsardb_segment_decode(segment_obj.data, segment_obj.size, &view));
    /* First frame block: object header + commit id + batch-header copy. */
    size_t first_block = (size_t)(view.payload - segment_obj.data) +
                         OTSARDB_SEGMENT_BATCH_HEADER_COPY_SIZE;
    assert(first_block + OTSARDB_SEGMENT_BLOCK_HEADER_SIZE <= segment_obj.size);
    segment_obj.data[first_block] ^= 0xff; /* corrupt orig_len byte 0 */
    otsardb_store_put_options plain = {0};
    assert(otsardb_store_put(store,
                           segment_key,
                           segment_obj.data,
                           segment_obj.size,
                           &plain,
                           NULL) == OTSARDB_STORE_OK);
    otsardb_store_release_object(store, &segment_obj);
    otsardb_commit_manifest_free(&manifest);
    otsardb_store_release_object(store, &manifest_obj);
}

/* Delete the newest segment object in the store (test helper). */
static void delete_head_segment(otsardb_object_store *store, const char *root, const otsardb_head *head) {
    otsardb_store_object manifest_obj = {0};
    assert(otsardb_store_get(store, head->commit_key, &manifest_obj) == OTSARDB_STORE_OK);
    otsardb_commit_manifest manifest;
    assert(otsardb_commit_manifest_decode_json((const char *)manifest_obj.data,
                                             manifest_obj.size,
                                             &manifest));
    char segment_key[OTSARDB_KEY_MAX + 1];
    snprintf(segment_key, sizeof(segment_key), "%s/%s", root, manifest.segments[0].key);
    otsardb_store_release_object(store, &manifest_obj);
    otsardb_commit_manifest_free(&manifest);
    assert(otsardb_store_delete(store, segment_key) == OTSARDB_STORE_OK);
}

/* Counting store wrapper: delegates every operation to a
 * memory store while counting get / get_range calls, so tests can prove that
 * frame-offset segments are fetched with byte-range GETs and never with a
 * whole-object GET. */
typedef struct counting_store_ctx {
    otsardb_object_store *inner;
    size_t get_count;
    size_t get_range_count;
    size_t segment_get_count;   /* whole-object GETs whose key is a segment */
} counting_store_ctx;

static counting_store_ctx *counting_ctx(otsardb_object_store *store) {
    return (counting_store_ctx *)store->context;
}

static otsardb_store_result counting_get(otsardb_object_store *store,
                                       const char *key,
                                       otsardb_store_object *out) {
    counting_store_ctx *ctx = counting_ctx(store);
    ++ctx->get_count;
    if (key && strstr(key, "segments/") != NULL) ++ctx->segment_get_count;
    return otsardb_store_get(ctx->inner, key, out);
}

static otsardb_store_result counting_get_range(otsardb_object_store *store,
                                             const char *key,
                                             size_t offset,
                                             size_t length,
                                             otsardb_store_object *out) {
    counting_store_ctx *ctx = counting_ctx(store);
    ++ctx->get_range_count;
    return otsardb_store_get_range(ctx->inner, key, offset, length, out);
}

static otsardb_store_result counting_put(otsardb_object_store *store,
                                       const char *key,
                                       const void *data,
                                       size_t size,
                                       const otsardb_store_put_options *options,
                                       char **out_etag) {
    return otsardb_store_put(counting_ctx(store)->inner, key, data, size, options, out_etag);
}

static void counting_release(otsardb_object_store *store, otsardb_store_object *object) {
    otsardb_store_release_object(counting_ctx(store)->inner, object);
}

static otsardb_store_result counting_delete(otsardb_object_store *store, const char *key) {
    return otsardb_store_delete(counting_ctx(store)->inner, key);
}

static otsardb_store_result counting_list(otsardb_object_store *store,
                                        const char *prefix,
                                        otsardb_store_list_callback callback,
                                        void *ctx) {
    return otsardb_store_list(counting_ctx(store)->inner, prefix, callback, ctx);
}

static const otsardb_object_store_ops COUNTING_OPS = {
    .get = counting_get,
    .get_range = counting_get_range,
    .put = counting_put,
    .release_object = counting_release,
    .delete_object = counting_delete,
    .list = counting_list,
};

static otsardb_object_store *counting_store_create(otsardb_object_store *inner) {
    otsardb_object_store *store = (otsardb_object_store *)calloc(1, sizeof(*store));
    counting_store_ctx *ctx = (counting_store_ctx *)calloc(1, sizeof(*ctx));
    assert(store && ctx);
    ctx->inner = inner;
    store->ops = &COUNTING_OPS;
    store->context = ctx;
    store->capabilities = inner->capabilities;
    return store;
}

static void counting_store_destroy(otsardb_object_store *store) {
    if (!store) return;
    free(store->context);
    free(store);
}

static size_t counting_segment_get_count(otsardb_object_store *store) {
    counting_store_ctx *ctx = counting_ctx(store);
    return ctx->segment_get_count;
}

static size_t counting_range_get_count(otsardb_object_store *store) {
    counting_store_ctx *ctx = counting_ctx(store);
    return ctx->get_range_count;
}

int main(void) {
    cleanup_files();
    assert(otsardb_initialize() == OTSARDB_OK);
    otsardb_object_store *store = otsardb_test_memory_store_create();
    assert(store != NULL);

    /* ------------------------------------------------------------------ *
     * 1. No checkpoint: lazy restore + hydrate everything must produce a
     * byte-identical image to full recovery. */
    publish_commit(store, ROOT, DB_ID, "CREATE TABLE items(id INTEGER PRIMARY KEY, value TEXT);");
    publish_commit(store, ROOT, DB_ID, "INSERT INTO items(value) VALUES('one'); INSERT INTO items(value) VALUES('two');");
    publish_commit(store, ROOT, DB_ID, "INSERT INTO items(value) VALUES('three'); INSERT INTO items(value) VALUES('four');");

    otsardb_head head = read_head(store, ROOT);
    assert(head.generation >= 3);

    assert(otsardb_recovery_restore_store(store, ROOT, FULL_IMAGE));

    otsardb_lazy_hydrator *hydrator = NULL;
    assert(otsardb_lazy_restore_store(store, ROOT, LAZY_IMAGE, &head, &hydrator));
    assert(!otsardb_lazy_is_complete(hydrator));
    assert(otsardb_lazy_hydrate_range(hydrator, 1, 4096) == 1);
    assert(otsardb_lazy_is_complete(hydrator));

    unsigned char *lazy_bytes = NULL;
    unsigned char *full_bytes = NULL;
    size_t lazy_size = 0;
    size_t full_size = 0;
    assert(read_file_bytes(LAZY_IMAGE, &lazy_bytes, &lazy_size));
    assert(read_file_bytes(FULL_IMAGE, &full_bytes, &full_size));
    assert(lazy_size == full_size);
    assert(memcmp(lazy_bytes, full_bytes, lazy_size) == 0);
    free(lazy_bytes);
    free(full_bytes);
    otsardb_lazy_destroy(hydrator);
    hydrator = NULL;
    otsardb_unlink(LAZY_IMAGE);
    otsardb_unlink(FULL_IMAGE);

    /* ------------------------------------------------------------------ *
     * 2. SQLite end-to-end through the VFS page hooks: a fresh lazy open
     * followed by a real SELECT must return the committed rows. */
    query_through_sqlite(store, ROOT, LAZY_IMAGE,
                         "SELECT count(*) FROM items;", "4");
    otsardb_unlink(LAZY_IMAGE);

    /* ------------------------------------------------------------------ *
     * 3. Fail closed: corrupt the newest segment object in the store; the
     * lazy fetch must fail and the failure must be sticky. */
    assert(otsardb_lazy_restore_store(store, ROOT, LAZY_IMAGE, &head, &hydrator));
    corrupt_head_segment(store, ROOT, &head);
    assert(otsardb_lazy_hydrate_range(hydrator, 1, 4096) == 0); /* fail closed */
    assert(otsardb_lazy_hydrate_range(hydrator, 1, 4096) == 0); /* sticky */
    otsardb_lazy_destroy(hydrator);
    hydrator = NULL;
    otsardb_unlink(LAZY_IMAGE);

    /* ------------------------------------------------------------------ *
     * 4. Fail closed: delete the newest segment object; the page it owned
     * must not be fetchable. (Fresh root: the corrupted ROOT chain of
     * test 3 can no longer be fully recovered.) */
    publish_commit(store, ROOT2, DB_ID2, "CREATE TABLE t2(id INTEGER PRIMARY KEY, v TEXT); INSERT INTO t2(v) VALUES('x');");
    publish_commit(store, ROOT2, DB_ID2, "INSERT INTO t2(v) VALUES('y');");
    head = read_head(store, ROOT2);
    assert(head.generation >= 2);
    assert(otsardb_lazy_restore_store(store, ROOT2, LAZY_IMAGE, &head, &hydrator));
    delete_head_segment(store, ROOT2, &head);
    assert(otsardb_lazy_hydrate_range(hydrator, 1, 4096) == 0);
    otsardb_lazy_destroy(hydrator);
    hydrator = NULL;
    otsardb_unlink(LAZY_IMAGE);

    /* ------------------------------------------------------------------ *
     * 5. Unknown-page segments (v1-v3 / v4 empty pages) fall back to a
     * whole-segment fetch: craft a v3 manifest + segment manually. */
    {
        /* Fresh root so the v3 chain is self-contained. */
        const char *root3 = "lazy/legacy";
        otsardb_store_put_options plain = {0};
        char *batch_data = NULL;
        size_t batch_size = 0;
        uint32_t page_nos[2] = {1, 2};
        unsigned char pages[2][512];
        memset(pages, 0x41, sizeof(pages));
        assert(otsardb_wal_batch_encode(512, 2, page_nos, (const unsigned char *)pages, 2,
                                      &batch_data, &batch_size));
        unsigned char *segment_data = NULL;
        size_t segment_size = 0;
        char segment_sha[OTSARDB_SEGMENT_SHA256_HEX_SIZE + 1];
        assert(otsardb_segment_encode(OTSARDB_SEGMENT_WAL_FRAMES,
                                    "legacy-v3-0001",
                                    batch_data,
                                    batch_size,
                                    &segment_data,
                                    &segment_size,
                                    segment_sha,
                                    1));
        free(batch_data);

        otsardb_db_metadata metadata;
        memset(&metadata, 0, sizeof(metadata));
        metadata.format_version = 1;
        metadata.page_size = 512;
        snprintf(metadata.database_id, sizeof(metadata.database_id), "%s", DB_ID);
        assert(otsardb_db_metadata_create(store, root3, &metadata) == OTSARDB_METADATA_OK);

        otsardb_commit_manifest manifest;
        memset(&manifest, 0, sizeof(manifest));
        manifest.format_version = 3;
        manifest.generation = 1;
        manifest.parent_generation = 0;
        manifest.page_size = 512;
        manifest.database_size_pages = 2;
        snprintf(manifest.database_id, sizeof(manifest.database_id), "%s", DB_ID);
        snprintf(manifest.commit_id, sizeof(manifest.commit_id), "legacy-v3-0001");
        manifest.segment_count = 1;
        snprintf(manifest.segments[0].key,
                 sizeof(manifest.segments[0].key),
                 "segments/legacy.otsseg");
        snprintf(manifest.segments[0].sha256,
                 sizeof(manifest.segments[0].sha256),
                 "%s", segment_sha);
        manifest.segments[0].size = segment_size;
        char *manifest_json = NULL;
        size_t manifest_size = 0;
        char manifest_sha[OTSARDB_MANIFEST_SHA256_HEX_SIZE + 1];
        assert(otsardb_commit_manifest_encode_json(&manifest,
                                                 &manifest_json,
                                                 &manifest_size,
                                                 manifest_sha));

        char seg_key[OTSARDB_KEY_MAX + 1];
        snprintf(seg_key, sizeof(seg_key), "%s/%s", root3, "segments/legacy.otsseg");
        assert(otsardb_store_put(store, seg_key, segment_data, segment_size, &plain, NULL) == OTSARDB_STORE_OK);
        char man_key[OTSARDB_KEY_MAX + 1];
        snprintf(man_key, sizeof(man_key), "%s/%s", root3, "commits/legacy-v3-0001.json");
        assert(otsardb_store_put(store, man_key, manifest_json, manifest_size, &plain, NULL) == OTSARDB_STORE_OK);
        otsardb_head legacy_head;
        memset(&legacy_head, 0, sizeof(legacy_head));
        legacy_head.format_version = 1;
        legacy_head.generation = 1;
        snprintf(legacy_head.database_id, sizeof(legacy_head.database_id), "%s", DB_ID);
        snprintf(legacy_head.commit_id, sizeof(legacy_head.commit_id), "legacy-v3-0001");
        snprintf(legacy_head.commit_key, sizeof(legacy_head.commit_key), "%s", man_key);
        snprintf(legacy_head.commit_sha256, sizeof(legacy_head.commit_sha256), "%s", manifest_sha);
        char *head_json = NULL;
        size_t head_size = 0;
        assert(otsardb_head_encode_json(&legacy_head, &head_json, &head_size));
        char head_key[OTSARDB_KEY_MAX + 1];
        snprintf(head_key, sizeof(head_key), "%s/%s", root3, "HEAD.json");
        assert(otsardb_store_put(store, head_key, head_json, head_size, &plain, NULL) == OTSARDB_STORE_OK);

        otsardb_lazy_hydrator *legacy_hydrator = NULL;
        assert(otsardb_lazy_restore_store(store, root3, LAZY_IMAGE, &legacy_head, &legacy_hydrator));
        /* The v3 segment has no page set: hydrating any page must fetch it. */
        assert(otsardb_lazy_hydrate_range(legacy_hydrator, 1, 2) == 1);
        unsigned char *page1 = (unsigned char *)malloc(512);
        assert(page1);
        FILE *file = fopen(LAZY_IMAGE, "rb");
        assert(file);
        assert(fread(page1, 1, 512, file) == 512);
        fclose(file);
        assert(memcmp(page1, pages[0], 512) == 0);
        free(page1);
        otsardb_lazy_destroy(legacy_hydrator);
        otsardb_unlink(LAZY_IMAGE);
        free(segment_data);
        free(manifest_json);
        free(head_json);
        otsardb_store_delete(store, seg_key);
        otsardb_store_delete(store, man_key);
        otsardb_store_delete(store, head_key);
        otsardb_store_delete(store, "lazy/legacy/db.json");
    }

    /* ------------------------------------------------------------------ *
     * 6. Checkpoint base image: after a checkpoint covers part of the
     * chain, lazy restore must serve covered pages from the base image
     * and newer pages from segments — byte-identical to full recovery.
     * (Fresh root: the previous roots are intentionally poisoned.) */
    {
        const char *root6 = "lazy/cp";
        const char *db6 = "lazy-cp-db";
        publish_commit(store, root6, db6, "CREATE TABLE cp_t(id INTEGER PRIMARY KEY, v TEXT);");
        publish_commit(store, root6, db6, "INSERT INTO cp_t(v) VALUES('a'); INSERT INTO cp_t(v) VALUES('b');");

        /* Build a checkpoint covering the current head, then grow the DB. */
        otsardb_head cp_head = read_head(store, root6);
        assert(otsardb_recovery_restore_store(store, root6, SOURCE));
        assert(otsardb_checkpoint_build(store, root6, SOURCE));
        otsardb_unlink(SOURCE);
        otsardb_unlink(WAL_PATH);
        otsardb_unlink(SHM_PATH);
        publish_commit(store, root6, db6, "INSERT INTO cp_t(v) VALUES('c'); INSERT INTO cp_t(v) VALUES('d');");
        publish_commit(store, root6, db6, "INSERT INTO cp_t(v) VALUES('e');");
        (void)cp_head;

        otsardb_head head6 = read_head(store, root6);
        assert(otsardb_recovery_restore_store(store, root6, FULL_IMAGE));
        assert(otsardb_lazy_restore_store(store, root6, LAZY_IMAGE, &head6, &hydrator));
        assert(otsardb_lazy_hydrate_range(hydrator, 1, 4096) == 1);
        assert(otsardb_lazy_is_complete(hydrator));
        assert(read_file_bytes(LAZY_IMAGE, &lazy_bytes, &lazy_size));
        assert(read_file_bytes(FULL_IMAGE, &full_bytes, &full_size));
        assert(lazy_size == full_size);
        assert(memcmp(lazy_bytes, full_bytes, lazy_size) == 0);
        free(lazy_bytes);
        free(full_bytes);
        otsardb_lazy_destroy(hydrator);
        hydrator = NULL;
        otsardb_unlink(LAZY_IMAGE);
        otsardb_unlink(FULL_IMAGE);
    }

    /* ------------------------------------------------------------------ *
     * 7. published v4 manifests carry frame_offsets and a
     * cold hydrate serves pages with byte-range GETs only — no whole
     * segment object is ever fetched. */
    {
        const char *root7 = "lazy/offsets";
        const char *db7 = "lazy-offsets-db";
        publish_commit(store, root7, db7, "CREATE TABLE of_t(id INTEGER PRIMARY KEY, v TEXT);");
        publish_commit(store, root7, db7, "INSERT INTO of_t(v) VALUES('a'); INSERT INTO of_t(v) VALUES('b');");

        otsardb_head head7 = read_head(store, root7);

        /* The head manifest must record per-page frame offsets (additive v4
         * field, ; populated since 0059). */
        otsardb_store_object manifest7 = {0};
        assert(otsardb_store_get(store, head7.commit_key, &manifest7) == OTSARDB_STORE_OK);
        otsardb_commit_manifest manifest7_dec;
        assert(otsardb_commit_manifest_decode_json((const char *)manifest7.data,
                                                 manifest7.size,
                                                 &manifest7_dec));
        int found_offset_segment = 0;
        for (uint32_t s = 0; s < manifest7_dec.segment_count; ++s) {
            if (manifest7_dec.segments[s].page_count > 0) {
                assert(manifest7_dec.segments[s].frame_offsets != NULL);
                uint64_t prev = 0;
                for (uint32_t p = 0; p < manifest7_dec.segments[s].page_count; ++p) {
                    assert(manifest7_dec.segments[s].frame_offsets[p] > prev);
                    prev = manifest7_dec.segments[s].frame_offsets[p];
                }
                assert(prev < manifest7_dec.segments[s].size);
                found_offset_segment = 1;
            }
        }
        assert(found_offset_segment);
        otsardb_commit_manifest_free(&manifest7_dec);
        otsardb_store_release_object(store, &manifest7);

        /* Full-image byte equality vs recovery (recovery decodes the same v3
         * objects through the whole-object path; both must agree). */
        assert(otsardb_recovery_restore_store(store, root7, FULL_IMAGE));
        otsardb_lazy_hydrator *offset_hydrator = NULL;
        assert(otsardb_lazy_restore_store(store, root7, LAZY_IMAGE, &head7, &offset_hydrator));
        assert(otsardb_lazy_hydrate_range(offset_hydrator, 1, 4096) == 1);
        assert(otsardb_lazy_is_complete(offset_hydrator));
        assert(read_file_bytes(LAZY_IMAGE, &lazy_bytes, &lazy_size));
        assert(read_file_bytes(FULL_IMAGE, &full_bytes, &full_size));
        assert(lazy_size == full_size);
        assert(memcmp(lazy_bytes, full_bytes, lazy_size) == 0);
        free(lazy_bytes);
        free(full_bytes);
        otsardb_lazy_destroy(offset_hydrator);
        otsardb_unlink(LAZY_IMAGE);
        otsardb_unlink(FULL_IMAGE);

        /* Fresh restore through the counting store: hydrating one page must
         * issue at least one range GET and ZERO whole-segment GETs. */
        otsardb_object_store *counting = counting_store_create(store);
        assert(counting != NULL);
        otsardb_lazy_hydrator *counted_hydrator = NULL;
        assert(otsardb_lazy_restore_store(counting, root7, LAZY_IMAGE, &head7, &counted_hydrator));
        assert(otsardb_lazy_hydrate_range(counted_hydrator, 1, 2) == 1);
        assert(counting_range_get_count(counting) > 0);
        assert(counting_segment_get_count(counting) == 0);
        otsardb_lazy_destroy(counted_hydrator);
        counting_store_destroy(counting);
        otsardb_unlink(LAZY_IMAGE);
    }

    /* ------------------------------------------------------------------ *
     * 8. a v4 manifest WITHOUT frame_offsets falls back to
     * the whole-segment fetch (0043 behavior) and stays correct. */
    {
        const char *root8 = "lazy/v4nooffsets";
        otsardb_store_put_options plain8 = {0};
        char *batch8 = NULL;
        size_t batch8_size = 0;
        uint32_t page_nos8[1] = {1};
        unsigned char pages8[512];
        memset(pages8, 0x5a, sizeof(pages8));
        assert(otsardb_wal_batch_encode(512, 2, page_nos8, pages8, 1,
                                      &batch8, &batch8_size));
        unsigned char *segment8 = NULL;
        size_t segment8_size = 0;
        char segment8_sha[OTSARDB_SEGMENT_SHA256_HEX_SIZE + 1];
        /* v2 segment via the legacy encoder: no frame table. */
        assert(otsardb_segment_encode(OTSARDB_SEGMENT_WAL_FRAMES,
                                    "v4-no-offsets-0001",
                                    batch8,
                                    batch8_size,
                                    &segment8,
                                    &segment8_size,
                                    segment8_sha,
                                    1));
        free(batch8);

        otsardb_db_metadata metadata8;
        memset(&metadata8, 0, sizeof(metadata8));
        metadata8.format_version = 1;
        metadata8.page_size = 512;
        snprintf(metadata8.database_id, sizeof(metadata8.database_id), "%s", DB_ID);
        assert(otsardb_db_metadata_create(store, root8, &metadata8) == OTSARDB_METADATA_OK);

        otsardb_commit_manifest manifest8;
        memset(&manifest8, 0, sizeof(manifest8));
        manifest8.format_version = 4;
        manifest8.generation = 1;
        manifest8.parent_generation = 0;
        manifest8.page_size = 512;
        manifest8.database_size_pages = 2;
        snprintf(manifest8.database_id, sizeof(manifest8.database_id), "%s", DB_ID);
        snprintf(manifest8.commit_id, sizeof(manifest8.commit_id), "v4-no-offsets-0001");
        manifest8.segment_count = 1;
        snprintf(manifest8.segments[0].key,
                 sizeof(manifest8.segments[0].key),
                 "segments/noff.otsseg");
        snprintf(manifest8.segments[0].sha256,
                 sizeof(manifest8.segments[0].sha256),
                 "%s", segment8_sha);
        manifest8.segments[0].size = segment8_size;
        uint32_t pageset8[1] = {1};
        manifest8.segments[0].pages = pageset8;
        manifest8.segments[0].page_count = 1;
        manifest8.segments[0].frame_offsets = NULL;
        char *manifest8_json = NULL;
        size_t manifest8_size = 0;
        char manifest8_sha[OTSARDB_MANIFEST_SHA256_HEX_SIZE + 1];
        assert(otsardb_commit_manifest_encode_json(&manifest8,
                                                 &manifest8_json,
                                                 &manifest8_size,
                                                 manifest8_sha));

        char seg8_key[OTSARDB_KEY_MAX + 1];
        snprintf(seg8_key, sizeof(seg8_key), "%s/%s", root8, "segments/noff.otsseg");
        assert(otsardb_store_put(store, seg8_key, segment8, segment8_size, &plain8, NULL) == OTSARDB_STORE_OK);
        char man8_key[OTSARDB_KEY_MAX + 1];
        snprintf(man8_key, sizeof(man8_key), "%s/%s", root8, "commits/v4-no-offsets-0001.json");
        assert(otsardb_store_put(store, man8_key, manifest8_json, manifest8_size, &plain8, NULL) == OTSARDB_STORE_OK);
        otsardb_head head8;
        memset(&head8, 0, sizeof(head8));
        head8.format_version = 1;
        head8.generation = 1;
        snprintf(head8.database_id, sizeof(head8.database_id), "%s", DB_ID);
        snprintf(head8.commit_id, sizeof(head8.commit_id), "v4-no-offsets-0001");
        snprintf(head8.commit_key, sizeof(head8.commit_key), "%s", man8_key);
        snprintf(head8.commit_sha256, sizeof(head8.commit_sha256), "%s", manifest8_sha);
        char *head8_json = NULL;
        size_t head8_size = 0;
        assert(otsardb_head_encode_json(&head8, &head8_json, &head8_size));
        char head8_key[OTSARDB_KEY_MAX + 1];
        snprintf(head8_key, sizeof(head8_key), "%s/%s", root8, "HEAD.json");
        assert(otsardb_store_put(store, head8_key, head8_json, head8_size, &plain8, NULL) == OTSARDB_STORE_OK);

        otsardb_object_store *counting8 = counting_store_create(store);
        assert(counting8 != NULL);
        otsardb_lazy_hydrator *hydrator8 = NULL;
        assert(otsardb_lazy_restore_store(counting8, root8, LAZY_IMAGE, &head8, &hydrator8));
        assert(otsardb_lazy_hydrate_range(hydrator8, 1, 1) == 1);
        /* No frame offsets: exactly one whole-segment GET, zero range GETs. */
        assert(counting_segment_get_count(counting8) == 1);
        assert(counting_range_get_count(counting8) == 0);
        unsigned char *page8 = (unsigned char *)malloc(512);
        assert(page8);
        FILE *file8 = fopen(LAZY_IMAGE, "rb");
        assert(file8);
        assert(fread(page8, 1, 512, file8) == 512);
        fclose(file8);
        assert(memcmp(page8, pages8, 512) == 0);
        free(page8);
        otsardb_lazy_destroy(hydrator8);
        counting_store_destroy(counting8);
        otsardb_unlink(LAZY_IMAGE);
        free(segment8);
        free(manifest8_json);
        free(head8_json);
        otsardb_store_delete(store, seg8_key);
        otsardb_store_delete(store, man8_key);
        otsardb_store_delete(store, head8_key);
        otsardb_store_delete(store, "lazy/v4nooffsets/db.json");
    }

    /* ------------------------------------------------------------------ *
     * 9. fail-closed: a corrupted byte inside the fetched
     * frame region (block header, then frame data) fails the sliced
     * hydrate, and the failure is sticky. */
    {
        const char *root9 = "lazy/corrupt-block";
        const char *db9 = "lazy-corrupt-block-db";
        publish_commit(store, root9, db9, "CREATE TABLE cb_t(id INTEGER PRIMARY KEY, v TEXT);");
        publish_commit(store, root9, db9, "INSERT INTO cb_t(v) VALUES('x');");

        otsardb_head head9 = read_head(store, root9);
        assert(otsardb_lazy_restore_store(store, root9, LAZY_IMAGE, &head9, &hydrator));
        corrupt_head_segment(store, root9, &head9); /* block header corruption */
        assert(otsardb_lazy_hydrate_range(hydrator, 1, 4096) == 0);
        assert(otsardb_lazy_hydrate_range(hydrator, 1, 4096) == 0); /* sticky */
        otsardb_lazy_destroy(hydrator);
        hydrator = NULL;
        otsardb_unlink(LAZY_IMAGE);

        /* Frame-DATA corruption (inside the page bytes): caught by the
         * per-block CRC-32. Fresh root (the previous chain is poisoned). */
        const char *root10 = "lazy/corrupt-data";
        const char *db10 = "lazy-corrupt-data-db";
        publish_commit(store, root10, db10, "CREATE TABLE cd_t(id INTEGER PRIMARY KEY, v TEXT);");
        publish_commit(store, root10, db10, "INSERT INTO cd_t(v) VALUES('y');");
        otsardb_head head10 = read_head(store, root10);
        assert(otsardb_lazy_restore_store(store, root10, LAZY_IMAGE, &head10, &hydrator));

        otsardb_store_object manifest10 = {0};
        assert(otsardb_store_get(store, head10.commit_key, &manifest10) == OTSARDB_STORE_OK);
        otsardb_commit_manifest manifest10_dec;
        assert(otsardb_commit_manifest_decode_json((const char *)manifest10.data,
                                                 manifest10.size,
                                                 &manifest10_dec));
        char segment10_key[OTSARDB_KEY_MAX + 1];
        snprintf(segment10_key,
                 sizeof(segment10_key),
                 "%s/%s",
                 root10,
                 manifest10_dec.segments[0].key);
        otsardb_store_object segment10_obj = {0};
        assert(otsardb_store_get(store, segment10_key, &segment10_obj) == OTSARDB_STORE_OK);
        otsardb_segment_view view10;
        assert(otsardb_segment_decode(segment10_obj.data, segment10_obj.size, &view10));
        size_t first_data = (size_t)(view10.payload - segment10_obj.data) +
                            OTSARDB_SEGMENT_BATCH_HEADER_COPY_SIZE +
                            OTSARDB_SEGMENT_BLOCK_HEADER_SIZE;
        assert(first_data < segment10_obj.size);
        segment10_obj.data[first_data] ^= 0xff; /* corrupt page byte */
        otsardb_store_put_options plain10 = {0};
        assert(otsardb_store_put(store,
                               segment10_key,
                               segment10_obj.data,
                               segment10_obj.size,
                               &plain10,
                               NULL) == OTSARDB_STORE_OK);
        otsardb_store_release_object(store, &segment10_obj);
        otsardb_commit_manifest_free(&manifest10_dec);
        otsardb_store_release_object(store, &manifest10);

        assert(otsardb_lazy_hydrate_range(hydrator, 1, 4096) == 0); /* CRC-32 mismatch */
        otsardb_lazy_destroy(hydrator);
        hydrator = NULL;
        otsardb_unlink(LAZY_IMAGE);
    }

    printf("lazy hydration test: all assertions passed\n");
    otsardb_test_memory_store_destroy(store);
    cleanup_files();
    return 0;
}
