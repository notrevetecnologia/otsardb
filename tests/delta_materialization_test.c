/*
 * — delta materialization on a cached image
 * (/ NVMe tier).
 *
 * Deterministic, in-memory only (memory_store + counting store wrapper, no
 * S3): builds real chains via the WAL publisher, saves a real local-cache
 * image at generation G (otsardb_local_cache_save), publishes newer commits
 * (G+1..G+k) and proves otsardb_recovery_materialize_delta applies ONLY the
 * newer segments on top of the cached image:
 *
 * 1. GET accounting: base-generation segments are NEVER re-fetched; each
 * newer segment is fetched exactly once; the resulting image is
 * byte-identical to full recovery and readable through plain SQLite
 * with ZERO additional store traffic (no hydrator needed — the image
 * is complete, so a lazy session performs no double work).
 * 2. Fail-closed / full fallback: stale base sha, wrong base generation,
 * corrupt base image bytes, missing base image, base==dest and bad
 * sha length all return 0 without touching the destination, and full
 * recovery still produces the correct image.
 * 3. Delta + lazy hydration interplay: with a checkpoint at G, the delta
 * path (cached image + newer segments) and the lazy path (checkpoint
 * base + on-demand hydration) reach the SAME head image, and neither
 * path re-fetches the other's base source (no double work).
 * 4. Base missing -> full path (read_state 0 + delta 0 + restore OK).
 * 5. Determinism: two delta runs produce byte-identical images, and the
 * cached base image is never modified.
 *
 * Expected coordinator target (CMakeLists.txt wiring by the coordinator):
 * add_executable(otsardb_delta_materialization
 * tests/delta_materialization_test.c
 * tests/memory_store.c
 *)
 * target_include_directories(otsardb_delta_materialization PRIVATE
 * include src/core src/crypto src/storage tests)
 * target_link_libraries(otsardb_delta_materialization PRIVATE
 * otsardb_core otsardb_s3_database)
 * add_test(NAME otsardb_delta_materialization
 * COMMAND otsardb_delta_materialization)
 * set_tests_properties(otsardb_delta_materialization PROPERTIES TIMEOUT 120)
 */

#include "otsardb.h"
#include "checkpoint_build.h"
#include "commit_manifest.h"
#include "journal.h"
#include "lazy_hydration.h"
#include "local_cache.h"
#include "local_vfs.h"
#include "memory_store.h"
#include "object_store.h"
#include "recovery.h"
#include "wal_publisher.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#include <direct.h>
#define otsardb_unlink _unlink
#define otsardb_rmdir _rmdir
#else
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#define otsardb_unlink unlink
#define otsardb_rmdir rmdir
#endif

#define OTSARDB_TEST_KEY_MAX 511
#define OTSARDB_TEST_SET_MAX 128

static const char *SOURCE = "delta-source.db";
static const char *WAL_PATH = "delta-source.db-wal";
static const char *SHM_PATH = "delta-source.db-shm";

static void cleanup_files(void) {
    otsardb_unlink(SOURCE);
    otsardb_unlink(WAL_PATH);
    otsardb_unlink(SHM_PATH);
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

static void assert_files_identical(const char *a, const char *b) {
    unsigned char *ba = NULL, *bb = NULL;
    size_t sa = 0, sb = 0;
    assert(read_file_bytes(a, &ba, &sa));
    assert(read_file_bytes(b, &bb, &sb));
    assert(sa == sb);
    assert(memcmp(ba, bb, sa) == 0);
    free(ba);
    free(bb);
}

static void assert_no_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f) {
        fclose(f);
        assert(!"file exists");
    }
}

/* Publish one commit by driving a real SQLite connection through the WAL
 * publisher (the same path an S3 session uses). */
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
        /* anchor, exactly like the S3 session: the restored
         * image is the proven parent state, so old-page reads may feed the
         * rolling checksum and chained commits publish post_apply_checksum. */
        otsardb_wal_publisher_set_local_image_generation(&publisher, head.generation);
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
    otsardb_local_vfs_set_wal_reader_sync_hook(NULL, NULL);
    otsardb_unlink(SOURCE);
    otsardb_unlink(WAL_PATH);
    otsardb_unlink(SHM_PATH);
}

/* ---- local-cache helpers (real otsardb_local_cache_save / read_state) ---- */

static void cache_image_path(const char *cache_dir,
                             const char *db_id,
                             char *out,
                             size_t out_size) {
    snprintf(out, out_size, "%s/otsardb-cache-v1/%s/state.db", cache_dir, db_id);
}

/* Restore the current remote state into image_path and save it as the cache
 * entry for db_id at the CURRENT head (the exact close-time save contract). */
static void save_cache_at_head(otsardb_object_store *store,
                               const char *root,
                               const char *cache_dir,
                               const char *db_id,
                               const char *image_path) {
    otsardb_head head = read_head(store, root);
    assert(otsardb_recovery_restore_store(store, root, image_path));
    otsardb_local_cache_state state;
    memset(&state, 0, sizeof(state));
    state.generation = head.generation;
    snprintf(state.commit_id, sizeof(state.commit_id), "%s", head.commit_id);
    snprintf(state.commit_sha256, sizeof(state.commit_sha256), "%s", head.commit_sha256);
    assert(otsardb_local_cache_save(cache_dir, db_id, image_path, &state));
}

static void remove_cache_dir(const char *cache_dir, const char *db_id) {
    char buf[1100];
    snprintf(buf, sizeof(buf), "%s/otsardb-cache-v1/%s/state.db", cache_dir, db_id);
    otsardb_unlink(buf);
    snprintf(buf, sizeof(buf), "%s/otsardb-cache-v1/%s/cache.json", cache_dir, db_id);
    otsardb_unlink(buf);
    snprintf(buf, sizeof(buf), "%s/otsardb-cache-v1/%s/cache.json.tmp", cache_dir, db_id);
    otsardb_unlink(buf);
    snprintf(buf, sizeof(buf), "%s/otsardb-cache-v1/%s", cache_dir, db_id);
    otsardb_rmdir(buf);
    snprintf(buf, sizeof(buf), "%s/otsardb-cache-v1/otsardb-cache-index.json", cache_dir);
    otsardb_unlink(buf);
    snprintf(buf, sizeof(buf), "%s/otsardb-cache-v1/otsardb-cache-index.json.tmp", cache_dir);
    otsardb_unlink(buf);
    snprintf(buf, sizeof(buf), "%s/otsardb-cache-v1", cache_dir);
    otsardb_rmdir(buf);
    otsardb_rmdir(cache_dir);
}

/* ---- segment key sets (via otsardb_store_list) ---- */

typedef struct key_set {
    char prefix[OTSARDB_TEST_KEY_MAX + 1];
    char keys[OTSARDB_TEST_SET_MAX][OTSARDB_TEST_KEY_MAX + 1];
    size_t count;
} key_set;

static int collect_suffix(const char *suffix, void *ctx) {
    key_set *set = (key_set *)ctx;
    assert(set->count < OTSARDB_TEST_SET_MAX);
    snprintf(set->keys[set->count], OTSARDB_TEST_KEY_MAX + 1, "%s%s", set->prefix, suffix);
    ++set->count;
    return 0;
}

static void list_segment_keys(otsardb_object_store *store,
                              const char *root,
                              key_set *out) {
    memset(out, 0, sizeof(*out));
    snprintf(out->prefix, sizeof(out->prefix), "%s/segments/", root);
    assert(otsardb_store_list(store, out->prefix, collect_suffix, out) == OTSARDB_STORE_OK);
}

static int key_in_set(const key_set *set, const char *key) {
    for (size_t i = 0; i < set->count; ++i) {
        if (strcmp(set->keys[i], key) == 0) return 1;
    }
    return 0;
}

/* The segments added AFTER the cache was saved (all_set minus base_set). */
static void compute_new_set(const key_set *all, const key_set *base, key_set *out) {
    memset(out, 0, sizeof(*out));
    for (size_t i = 0; i < all->count; ++i) {
        if (!key_in_set(base, all->keys[i])) {
            assert(out->count < OTSARDB_TEST_SET_MAX);
            snprintf(out->keys[out->count], OTSARDB_TEST_KEY_MAX + 1, "%s", all->keys[i]);
            ++out->count;
        }
    }
}

/* ---- counting store (records every GET key) ---- */

#if defined(_WIN32)
#include <windows.h>
#endif

typedef struct counting_ctx {
    otsardb_object_store *inner;
    size_t get_count;
    size_t segment_get_count;
    char fetched[OTSARDB_TEST_SET_MAX][OTSARDB_TEST_KEY_MAX + 1];
    size_t fetched_count;
#if defined(_WIN32)
    CRITICAL_SECTION lock;
#else
    pthread_mutex_t lock;
#endif
} counting_ctx;

static counting_ctx *counting_ctx_of(otsardb_object_store *store) {
    return (counting_ctx *)store->context;
}

/* The lazy-hydration hydrate path may record GETs from the prefetch pool
 * workers AND the demand thread concurrently — the fetched array and the
 * counters must be serialized (an unsynchronized check-then-increment could
 * overflow the bounded array and corrupt the heap). */
static void counting_lock(counting_ctx *ctx) {
#if defined(_WIN32)
    EnterCriticalSection(&ctx->lock);
#else
    pthread_mutex_lock(&ctx->lock);
#endif
}

static void counting_unlock(counting_ctx *ctx) {
#if defined(_WIN32)
    LeaveCriticalSection(&ctx->lock);
#else
    pthread_mutex_unlock(&ctx->lock);
#endif
}

static void counting_record(counting_ctx *ctx, const char *key) {
    counting_lock(ctx);
    ++ctx->get_count;
    if (strstr(key, "/segments/") != NULL) ++ctx->segment_get_count;
    if (ctx->fetched_count < OTSARDB_TEST_SET_MAX) {
        snprintf(ctx->fetched[ctx->fetched_count], OTSARDB_TEST_KEY_MAX + 1, "%s", key);
        ++ctx->fetched_count;
    }
    counting_unlock(ctx);
}

static otsardb_store_result counting_get(otsardb_object_store *store,
                                       const char *key,
                                       otsardb_store_object *out) {
    counting_ctx *ctx = counting_ctx_of(store);
    counting_record(ctx, key);
    return otsardb_store_get(ctx->inner, key, out);
}

static otsardb_store_result counting_get_range(otsardb_object_store *store,
                                             const char *key,
                                             size_t offset,
                                             size_t length,
                                             otsardb_store_object *out) {
    counting_ctx *ctx = counting_ctx_of(store);
    counting_record(ctx, key);
    return otsardb_store_get_range(ctx->inner, key, offset, length, out);
}

static otsardb_store_result counting_put(otsardb_object_store *store,
                                       const char *key,
                                       const void *data,
                                       size_t size,
                                       const otsardb_store_put_options *options,
                                       char **out_etag) {
    return otsardb_store_put(counting_ctx_of(store)->inner, key, data, size, options, out_etag);
}

static void counting_release(otsardb_object_store *store, otsardb_store_object *object) {
    otsardb_store_release_object(counting_ctx_of(store)->inner, object);
}

static otsardb_store_result counting_delete(otsardb_object_store *store, const char *key) {
    return otsardb_store_delete(counting_ctx_of(store)->inner, key);
}

static otsardb_store_result counting_list(otsardb_object_store *store,
                                        const char *prefix,
                                        otsardb_store_list_callback callback,
                                        void *ctx) {
    return otsardb_store_list(counting_ctx_of(store)->inner, prefix, callback, ctx);
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
    counting_ctx *ctx = (counting_ctx *)calloc(1, sizeof(*ctx));
    assert(store && ctx);
    ctx->inner = inner;
#if defined(_WIN32)
    InitializeCriticalSection(&ctx->lock);
#else
    pthread_mutex_init(&ctx->lock, NULL);
#endif
    store->ops = &COUNTING_OPS;
    store->context = ctx;
    store->capabilities = inner->capabilities;
    return store;
}

static void counting_store_destroy(otsardb_object_store *store) {
    if (!store) return;
    counting_ctx *ctx = counting_ctx_of(store);
#if defined(_WIN32)
    DeleteCriticalSection(&ctx->lock);
#else
    pthread_mutex_destroy(&ctx->lock);
#endif
    free(ctx);
    free(store);
}

/* The delta path must have fetched EXACTLY the newer segments (each once,
 * none of the base ones): every fetched segment key is a new segment, and
 * the segment GET count equals the new-segment count. */
static void assert_delta_fetch_accounting(otsardb_object_store *counting,
                                          const key_set *new_segments) {
    counting_ctx *ctx = counting_ctx_of(counting);
    assert(ctx->segment_get_count == new_segments->count);
    for (size_t i = 0; i < ctx->fetched_count; ++i) {
        if (strstr(ctx->fetched[i], "/segments/") != NULL) {
            assert(key_in_set(new_segments, ctx->fetched[i]));
        }
    }
}

/* ---- SQLite read-through helper ---- */

static int collect_text(void *ctx, int argc, char **argv, char **columns) {
    (void)columns;
    if (argc != 1 || !argv || !argv[0]) return 1;
    snprintf((char *)ctx, 128, "%s", argv[0]);
    return 0;
}

static void assert_select_count(otsardb_object_store *counting,
                                const char *image_path,
                                const char *expected) {
    otsardb *db = NULL;
    assert(otsardb_open(image_path, &db) == OTSARDB_OK);
    char result[128] = {0};
    char *error_message = NULL;
    assert(otsardb_exec(db, "SELECT count(*) FROM t1;", (otsardb_row_callback)collect_text,
                      result, &error_message) == OTSARDB_OK);
    otsardb_free(error_message);
    assert(strcmp(result, expected) == 0);
    otsardb_close(db);
    if (counting) {
        /* A complete delta image needs NO hydrator: reading it through plain
         * SQLite must issue ZERO additional store GETs. */
        size_t before = counting_ctx_of(counting)->get_count;
        otsardb *db2 = NULL;
        assert(otsardb_open(image_path, &db2) == OTSARDB_OK);
        char result2[128] = {0};
        char *err2 = NULL;
        assert(otsardb_exec(db2, "SELECT count(*) FROM t1;", (otsardb_row_callback)collect_text,
                          result2, &err2) == OTSARDB_OK);
        otsardb_free(err2);
        assert(strcmp(result2, expected) == 0);
        otsardb_close(db2);
        assert(counting_ctx_of(counting)->get_count == before);
    }
}

/* The head manifest must carry a post-apply checksum so the whole-file gate
 * actually validates the delta image (0063: chained commits over a verified
 * restore keep publishing checksums). */
static void assert_head_has_checksum(otsardb_object_store *store, const otsardb_head *head) {
    otsardb_store_object manifest_obj = {0};
    assert(otsardb_store_get(store, head->commit_key, &manifest_obj) == OTSARDB_STORE_OK);
    otsardb_commit_manifest manifest;
    assert(otsardb_commit_manifest_decode_json((const char *)manifest_obj.data,
                                             manifest_obj.size,
                                             &manifest));
    assert(manifest.post_apply_checksum[0] != '\0');
    otsardb_commit_manifest_free(&manifest);
    otsardb_store_release_object(store, &manifest_obj);
}

int main(void) {
    cleanup_files();
    assert(otsardb_initialize() == OTSARDB_OK);
    otsardb_object_store *store = otsardb_test_memory_store_create();
    assert(store != NULL);

    /* ------------------------------------------------------------------ *
     * 1. Delta applies ONLY the newer segments (no checkpoint): the base
     * generations' segments are never re-fetched, the newer ones are
     * fetched exactly once, and the image is byte-identical to full
     * recovery + fully readable without any hydrator. */
    {
        const char *root1 = "delta/plain";
        const char *db1 = "delta-plain-db";
        const char *cache1 = "delta-cache-1";
        const char *CACHE1_IMG = "delta-cache-1-img.db";
        const char *DELTA1_IMG = "delta-1.img";
        const char *FULL1_IMG = "delta-1.full";

        publish_commit(store, root1, db1, "CREATE TABLE t1(id INTEGER PRIMARY KEY, v TEXT);");
        publish_commit(store, root1, db1, "INSERT INTO t1(v) VALUES('a'); INSERT INTO t1(v) VALUES('b');");
        publish_commit(store, root1, db1, "INSERT INTO t1(v) VALUES('c'); INSERT INTO t1(v) VALUES('d');");

        otsardb_head base_head = read_head(store, root1);
        assert(base_head.generation >= 3);
        save_cache_at_head(store, root1, cache1, db1, CACHE1_IMG);
        assert_head_has_checksum(store, &base_head);

        key_set base_segments;
        list_segment_keys(store, root1, &base_segments);
        assert(base_segments.count > 0);

        publish_commit(store, root1, db1, "INSERT INTO t1(v) VALUES('e'); INSERT INTO t1(v) VALUES('f');");
        publish_commit(store, root1, db1, "INSERT INTO t1(v) VALUES('g');");

        otsardb_head head = read_head(store, root1);
        assert(head.generation > base_head.generation);

        key_set all_segments;
        list_segment_keys(store, root1, &all_segments);
        key_set new_segments;
        compute_new_set(&all_segments, &base_segments, &new_segments);
        assert(new_segments.count > 0);

        /* The cached base image must be byte-preserved by the delta. */
        unsigned char *base_before = NULL;
        size_t base_before_size = 0;
        assert(read_file_bytes(CACHE1_IMG, &base_before, &base_before_size));

        otsardb_object_store *counting1 = counting_store_create(store);
        assert(counting1 != NULL);

        otsardb_local_cache_state cached;
        assert(otsardb_local_cache_read_state(cache1, db1, &cached) == 1);
        assert(cached.generation == base_head.generation);
        assert(strlen(cached.commit_sha256) == 64);
        char cached_path[1100];
        cache_image_path(cache1, db1, cached_path, sizeof(cached_path));

        otsardb_head out_head;
        memset(&out_head, 0, sizeof(out_head));
        assert(otsardb_recovery_materialize_delta(counting1, root1, cached_path,
                                                cached.generation,
                                                cached.commit_sha256,
                                                DELTA1_IMG, &out_head) == 1);
        assert(out_head.generation == head.generation);
        assert(strcmp(out_head.commit_id, head.commit_id) == 0);

        /* GET accounting: newer segments exactly once, base segments never. */
        assert_delta_fetch_accounting(counting1, &new_segments);

        /* Full recovery as the reference image. */
        assert(otsardb_recovery_restore_store(store, root1, FULL1_IMG));
        assert_files_identical(DELTA1_IMG, FULL1_IMG);

        /* The cached base is untouched and no temp file survives. */
        unsigned char *base_after = NULL;
        size_t base_after_size = 0;
        assert(read_file_bytes(CACHE1_IMG, &base_after, &base_after_size));
        assert(base_after_size == base_before_size);
        assert(memcmp(base_before, base_after, base_before_size) == 0);
        free(base_before);
        free(base_after);
        assert_no_file("delta-cache-1-img.db.deltatmp");

        /* Complete image: reads through plain SQLite cost ZERO additional
         * GETs (a lazy session would need no hydrator — no double work). */
        assert_select_count(counting1, DELTA1_IMG, "7");
        assert(counting_ctx_of(counting1)->segment_get_count == new_segments.count);

        counting_store_destroy(counting1);
        otsardb_unlink(CACHE1_IMG);
        otsardb_unlink(DELTA1_IMG);
        otsardb_unlink(FULL1_IMG);
        remove_cache_dir(cache1, db1);
    }

    /* ------------------------------------------------------------------ *
     * 2. Fail-closed: any chain/cache mismatch returns 0 without touching
     * the destination, and the FULL path still produces the correct
     * image. */
    {
        const char *root2 = "delta/mismatch";
        const char *db2 = "delta-mismatch-db";
        const char *cache2 = "delta-cache-2";
        const char *CACHE2_IMG = "delta-cache-2-img.db";
        const char *DELTA2_IMG = "delta-2.img";
        const char *FULL2_IMG = "delta-2.full";

        publish_commit(store, root2, db2, "CREATE TABLE t1(id INTEGER PRIMARY KEY, v TEXT);");
        publish_commit(store, root2, db2, "INSERT INTO t1(v) VALUES('a'); INSERT INTO t1(v) VALUES('b');");
        publish_commit(store, root2, db2, "INSERT INTO t1(v) VALUES('c'); INSERT INTO t1(v) VALUES('d');");
        save_cache_at_head(store, root2, cache2, db2, CACHE2_IMG);
        publish_commit(store, root2, db2, "INSERT INTO t1(v) VALUES('e');");
        publish_commit(store, root2, db2, "INSERT INTO t1(v) VALUES('f');");

        otsardb_head head2 = read_head(store, root2);
        assert_head_has_checksum(store, &head2);
        assert(otsardb_recovery_restore_store(store, root2, FULL2_IMG));

        otsardb_local_cache_state cached2;
        assert(otsardb_local_cache_read_state(cache2, db2, &cached2) == 1);
        char cached2_path[1100];
        cache_image_path(cache2, db2, cached2_path, sizeof(cached2_path));

        otsardb_head out2;
        memset(&out2, 0, sizeof(out2));

        /* (a) stale base manifest sha (cache metadata does not match the
         * chain's manifest at generation G) -> fail closed. */
        char stale_sha[65];
        memset(stale_sha, '0', 64);
        stale_sha[64] = '\0';
        assert(otsardb_recovery_materialize_delta(store, root2, cached2_path,
                                                cached2.generation, stale_sha,
                                                DELTA2_IMG, &out2) == 0);
        assert_no_file(DELTA2_IMG);

        /* (b) malformed base sha -> fail closed. */
        assert(otsardb_recovery_materialize_delta(store, root2, cached2_path,
                                                cached2.generation, "short-sha",
                                                DELTA2_IMG, &out2) == 0);
        assert_no_file(DELTA2_IMG);

        /* (c) base_generation not strictly below the head -> fail closed. */
        assert(otsardb_recovery_materialize_delta(store, root2, cached2_path,
                                                head2.generation,
                                                cached2.commit_sha256,
                                                DELTA2_IMG, &out2) == 0);
        assert(otsardb_recovery_materialize_delta(store, root2, cached2_path,
                                                head2.generation + 1,
                                                cached2.commit_sha256,
                                                DELTA2_IMG, &out2) == 0);
        assert_no_file(DELTA2_IMG);

        /* (d) in-range base_generation that is NOT the cached image's
         * generation (the manifest at gen head-1 does not hash to the
         * cached sha) -> fail closed. */
        assert(otsardb_recovery_materialize_delta(store, root2, cached2_path,
                                                head2.generation - 1,
                                                cached2.commit_sha256,
                                                DELTA2_IMG, &out2) == 0);
        assert_no_file(DELTA2_IMG);

        /* (e) base_image_path == database_path -> fail closed. */
        assert(otsardb_recovery_materialize_delta(store, root2, cached2_path,
                                                cached2.generation,
                                                cached2.commit_sha256,
                                                cached2_path, &out2) == 0);

        /* (f) corrupt base image bytes: the delta applies but the head
         * whole-file checksum rejects the wrong base -> fail closed,
         * then the FULL path yields the correct image. */
        {
            FILE *f = fopen(cached2_path, "r+b");
            assert(f);
            unsigned char byte = 0;
            assert(fread(&byte, 1, 1, f) == 1);
            byte ^= 0xff;
            assert(fseek(f, 0, SEEK_SET) == 0);
            assert(fwrite(&byte, 1, 1, f) == 1);
            fclose(f);
        }
        assert(otsardb_recovery_materialize_delta(store, root2, cached2_path,
                                                cached2.generation,
                                                cached2.commit_sha256,
                                                DELTA2_IMG, &out2) == 0);
        assert_no_file(DELTA2_IMG);
        assert_no_file("delta-cache-2-img.db.deltatmp");
        assert(otsardb_recovery_restore_store(store, root2, DELTA2_IMG));
        assert_files_identical(DELTA2_IMG, FULL2_IMG);
        otsardb_unlink(DELTA2_IMG);

        otsardb_unlink(CACHE2_IMG);
        otsardb_unlink(FULL2_IMG);
        remove_cache_dir(cache2, db2);
    }

    /* ------------------------------------------------------------------ *
     * 3. Delta + lazy hydration interplay (checkpoint at G): both paths
     * reach the SAME head image, and neither re-fetches the other's
     * base source — no double work. */
    {
        const char *root3 = "delta/lazy";
        const char *db3 = "delta-lazy-db";
        const char *cache3 = "delta-cache-3";
        const char *CACHE3_IMG = "delta-cache-3-img.db";
        const char *DELTA3_IMG = "delta-3.delta";
        const char *LAZY3_IMG = "delta-3.lazy";
        const char *FULL3_IMG = "delta-3.full";

        publish_commit(store, root3, db3, "CREATE TABLE t1(id INTEGER PRIMARY KEY, v TEXT);");
        publish_commit(store, root3, db3, "INSERT INTO t1(v) VALUES('a'); INSERT INTO t1(v) VALUES('b');");
        publish_commit(store, root3, db3, "INSERT INTO t1(v) VALUES('c'); INSERT INTO t1(v) VALUES('d');");

        /* Checkpoint covering the current head. */
        assert(otsardb_recovery_restore_store(store, root3, SOURCE));
        assert(otsardb_checkpoint_build(store, root3, SOURCE));
        otsardb_unlink(SOURCE);
        otsardb_unlink(WAL_PATH);
        otsardb_unlink(SHM_PATH);
        save_cache_at_head(store, root3, cache3, db3, CACHE3_IMG);

        key_set base3_segments;
        list_segment_keys(store, root3, &base3_segments);

        publish_commit(store, root3, db3, "INSERT INTO t1(v) VALUES('e');");
        publish_commit(store, root3, db3, "INSERT INTO t1(v) VALUES('f');");
        publish_commit(store, root3, db3, "INSERT INTO t1(v) VALUES('g');");

        otsardb_head head3 = read_head(store, root3);
        assert(head3.generation > 3);
        assert_head_has_checksum(store, &head3);

        key_set all3_segments;
        list_segment_keys(store, root3, &all3_segments);
        key_set new3_segments;
        compute_new_set(&all3_segments, &base3_segments, &new3_segments);
        assert(new3_segments.count > 0);

        otsardb_object_store *counting3 = counting_store_create(store);
        assert(counting3 != NULL);

        /* Delta path: cached image (gen G) + newer segments -> head. */
        otsardb_local_cache_state cached3;
        assert(otsardb_local_cache_read_state(cache3, db3, &cached3) == 1);
        assert(cached3.generation >= 3);
        char cached3_path[1100];
        cache_image_path(cache3, db3, cached3_path, sizeof(cached3_path));
        otsardb_head out3;
        memset(&out3, 0, sizeof(out3));
        assert(otsardb_recovery_materialize_delta(counting3, root3, cached3_path,
                                                cached3.generation,
                                                cached3.commit_sha256,
                                                DELTA3_IMG, &out3) == 1);
        assert(out3.generation == head3.generation);
        assert_delta_fetch_accounting(counting3, &new3_segments);

        /* Lazy path on the SAME chain: checkpoint base + on-demand
         * hydration covers the rest; must reach the identical head image
         * and never re-fetch the base segments either. */
        assert(otsardb_recovery_restore_store(store, root3, FULL3_IMG));
        otsardb_lazy_hydrator *hydrator = NULL;
        assert(otsardb_lazy_restore_store_ex(counting3, root3, LAZY3_IMG, &head3,
                                           &hydrator, NULL));
        assert(otsardb_lazy_hydrate_range(hydrator, 1, 4096) == 1);
        assert(otsardb_lazy_is_complete(hydrator));
        otsardb_lazy_destroy(hydrator);

        assert_files_identical(LAZY3_IMG, DELTA3_IMG);
        assert_files_identical(LAZY3_IMG, FULL3_IMG);

        /* No double work: across BOTH paths the base segments were never
         * fetched (the delta used the cached image, the lazy path used the
         * checkpoint base). */
        counting_ctx *ctx3 = counting_ctx_of(counting3);
        for (size_t i = 0; i < base3_segments.count; ++i) {
            for (size_t j = 0; j < ctx3->fetched_count; ++j) {
                if (strcmp(ctx3->fetched[j], base3_segments.keys[i]) == 0) {
                    assert(!"base-generation segment was fetched by the session");
                }
            }
        }

        counting_store_destroy(counting3);
        otsardb_unlink(CACHE3_IMG);
        otsardb_unlink(DELTA3_IMG);
        otsardb_unlink(LAZY3_IMG);
        otsardb_unlink(FULL3_IMG);
        remove_cache_dir(cache3, db3);
    }

    /* ------------------------------------------------------------------ *
     * 4. Base missing -> full path: a cache entry whose IMAGE FILE is
     * gone reports no state (read_state 0), the delta fails closed and
     * the full path produces the correct image. */
    {
        const char *root4 = "delta/missing";
        const char *db4 = "delta-missing-db";
        const char *cache4 = "delta-cache-4";
        const char *CACHE4_IMG = "delta-cache-4-img.db";
        const char *IMG4 = "delta-4.img";
        const char *FULL4_IMG = "delta-4.full";

        publish_commit(store, root4, db4, "CREATE TABLE t1(id INTEGER PRIMARY KEY, v TEXT);");
        publish_commit(store, root4, db4, "INSERT INTO t1(v) VALUES('a'); INSERT INTO t1(v) VALUES('b');");
        publish_commit(store, root4, db4, "INSERT INTO t1(v) VALUES('c');");
        save_cache_at_head(store, root4, cache4, db4, CACHE4_IMG);
        publish_commit(store, root4, db4, "INSERT INTO t1(v) VALUES('d');");

        char cached4_path[1100];
        cache_image_path(cache4, db4, cached4_path, sizeof(cached4_path));
        otsardb_local_cache_state cached4;
        assert(otsardb_local_cache_read_state(cache4, db4, &cached4) == 1);

        /* The cache image disappears (crash/eviction mid-session). */
        assert(otsardb_unlink(cached4_path) == 0);

        otsardb_local_cache_state missing4;
        memset(&missing4, 0, sizeof(missing4));
        assert(otsardb_local_cache_read_state(cache4, db4, &missing4) == 0);
        assert(otsardb_recovery_materialize_delta(store, root4, cached4_path,
                                                cached4.generation,
                                                cached4.commit_sha256,
                                                IMG4, NULL) == 0);
        assert_no_file(IMG4);

        assert(otsardb_recovery_restore_store(store, root4, FULL4_IMG));
        assert(otsardb_recovery_restore_store(store, root4, IMG4));
        assert_files_identical(IMG4, FULL4_IMG);

        otsardb_unlink(CACHE4_IMG);
        otsardb_unlink(IMG4);
        otsardb_unlink(FULL4_IMG);
        remove_cache_dir(cache4, db4);
    }

    /* ------------------------------------------------------------------ *
     * 5. Determinism: two delta runs on the same base produce byte-
     * identical images (and the same GET counts), both equal to full
     * recovery. */
    {
        const char *root5 = "delta/det";
        const char *db5 = "delta-det-db";
        const char *cache5 = "delta-cache-5";
        const char *CACHE5_IMG = "delta-cache-5-img.db";
        const char *DET1_IMG = "delta-5.run1";
        const char *DET2_IMG = "delta-5.run2";
        const char *FULL5_IMG = "delta-5.full";

        publish_commit(store, root5, db5, "CREATE TABLE t1(id INTEGER PRIMARY KEY, v TEXT);");
        publish_commit(store, root5, db5, "INSERT INTO t1(v) VALUES('a');");
        publish_commit(store, root5, db5, "INSERT INTO t1(v) VALUES('b'); INSERT INTO t1(v) VALUES('c');");
        save_cache_at_head(store, root5, cache5, db5, CACHE5_IMG);
        publish_commit(store, root5, db5, "INSERT INTO t1(v) VALUES('d'); INSERT INTO t1(v) VALUES('e');");
        publish_commit(store, root5, db5, "INSERT INTO t1(v) VALUES('f');");

        otsardb_head head5 = read_head(store, root5);
        otsardb_local_cache_state cached5;
        assert(otsardb_local_cache_read_state(cache5, db5, &cached5) == 1);
        char cached5_path[1100];
        cache_image_path(cache5, db5, cached5_path, sizeof(cached5_path));

        otsardb_object_store *counting5a = counting_store_create(store);
        assert(counting5a != NULL);
        otsardb_head out5a;
        memset(&out5a, 0, sizeof(out5a));
        assert(otsardb_recovery_materialize_delta(counting5a, root5, cached5_path,
                                                cached5.generation,
                                                cached5.commit_sha256,
                                                DET1_IMG, &out5a) == 1);
        size_t seg_gets_a = counting_ctx_of(counting5a)->segment_get_count;
        counting_store_destroy(counting5a);

        otsardb_object_store *counting5b = counting_store_create(store);
        assert(counting5b != NULL);
        otsardb_head out5b;
        memset(&out5b, 0, sizeof(out5b));
        assert(otsardb_recovery_materialize_delta(counting5b, root5, cached5_path,
                                                cached5.generation,
                                                cached5.commit_sha256,
                                                DET2_IMG, &out5b) == 1);
        size_t seg_gets_b = counting_ctx_of(counting5b)->segment_get_count;
        counting_store_destroy(counting5b);

        assert(out5a.generation == out5b.generation);
        assert(seg_gets_a == seg_gets_b);
        assert(seg_gets_a > 0);
        assert_files_identical(DET1_IMG, DET2_IMG);

        assert(otsardb_recovery_restore_store(store, root5, FULL5_IMG));
        assert_files_identical(DET1_IMG, FULL5_IMG);
        assert_select_count(NULL, DET1_IMG, "6");

        otsardb_unlink(CACHE5_IMG);
        otsardb_unlink(DET1_IMG);
        otsardb_unlink(DET2_IMG);
        otsardb_unlink(FULL5_IMG);
        remove_cache_dir(cache5, db5);
    }

    printf("delta materialization test: all assertions passed\n");
    otsardb_test_memory_store_destroy(store);
    cleanup_files();
    return 0;
}
