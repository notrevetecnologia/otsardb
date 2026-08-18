#include "otsardb.h"
#include "commit_manifest.h"
#include "journal.h"
#include "local_vfs.h"
#include "memory_store.h"
#include "recovery.h"
#include "wal_batch.h"
#include "wal_publisher.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#define otsardb_unlink _unlink
#else
#include <unistd.h>
#include <pthread.h>
#define otsardb_unlink unlink
#endif

/*
 * Parallel multipart segment publish test: 
 *
 * 1. A multi-segment commit (tiny segment payload limit) against a store
 * that reports OTSARDB_STORE_CAP_MULTIPART must pre-upload every segment
 * through otsardb_store_put_stream BEFORE any manifest PUT, with
 * OTSARDB_S3_PARALLEL_SEGMENTS bounding the concurrency. HEAD must advance
 * and zero-local recovery must apply every segment.
 * 2. The same workload against a store WITHOUT the multipart capability
 * must use zero put_stream calls (current sequential PUT path), still
 * publish, and still recover.
 * 3. A put_stream failure on the second segment must abort the pre-upload,
 * fail the commit, and leave HEAD untouched (no manifest for the failed
 * commit, no HEAD advance) - the durability contract.
 */

/* Two frames per chunk: the CREATE TABLE commit (2 frames, observed) stays
 * a single segment (no pre-upload), while the 1500-row INSERT below spans
 * several segments, so put_stream_calls == segment_count exactly. */
#define CHUNK_FRAMES 2u
#define ROWS 1500u
#define LOG_CAPACITY 256u

typedef struct call_log_entry {
    char kind; /* 'S' = put_stream, 'P' = put */
    char key[OTSARDB_KEY_MAX + 1];
} call_log_entry;

static void set_env(const char *name, const char *value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

typedef struct counting_store {
    otsardb_object_store *inner;
#if defined(_WIN32)
    CRITICAL_SECTION lock;
#else
    pthread_mutex_t lock;
#endif
    size_t put_stream_calls;
    size_t put_calls;
    size_t fail_put_stream_index;
    /* fail a RANGE of consecutive put_stream calls (first
     * 1-based call, count; 0 count = disabled) for transient-failure retry
     * scenarios. */
    size_t fail_put_stream_first;
    size_t fail_put_stream_count;
    size_t log_count;
    call_log_entry log[LOG_CAPACITY];
} counting_store;

static void counting_lock(counting_store *ctx) {
#if defined(_WIN32)
    EnterCriticalSection(&ctx->lock);
#else
    pthread_mutex_lock(&ctx->lock);
#endif
}

static void counting_unlock(counting_store *ctx) {
#if defined(_WIN32)
    LeaveCriticalSection(&ctx->lock);
#else
    pthread_mutex_unlock(&ctx->lock);
#endif
}

static void counting_store_fail_streams(otsardb_object_store *store,
                                        size_t first,
                                        size_t count) {
    counting_store *ctx = (counting_store *)store->context;
    counting_lock(ctx);
    ctx->fail_put_stream_first = first;
    ctx->fail_put_stream_count = count;
    counting_unlock(ctx);
}

static int log_push(counting_store *ctx, char kind, const char *key) {
    if (!ctx || !key) return 0;
    int ok = 1;
    counting_lock(ctx);
    if (ctx->log_count < LOG_CAPACITY) {
        call_log_entry *entry = &ctx->log[ctx->log_count++];
        entry->kind = kind;
        snprintf(entry->key, sizeof(entry->key), "%s", key);
    } else {
        ok = 0;
    }
    counting_unlock(ctx);
    return ok;
}

static otsardb_store_result counting_get(otsardb_object_store *store,
                                       const char *key,
                                       otsardb_store_object *out) {
    counting_store *ctx = (counting_store *)store->context;
    return otsardb_store_get(ctx->inner, key, out);
}

static otsardb_store_result counting_get_range(otsardb_object_store *store,
                                             const char *key,
                                             size_t offset,
                                             size_t length,
                                             otsardb_store_object *out) {
    counting_store *ctx = (counting_store *)store->context;
    return otsardb_store_get_range(ctx->inner, key, offset, length, out);
}

static otsardb_store_result counting_put(otsardb_object_store *store,
                                       const char *key,
                                       const void *data,
                                       size_t size,
                                       const otsardb_store_put_options *options,
                                       char **out_etag) {
    counting_store *ctx = (counting_store *)store->context;
    counting_lock(ctx);
    ++ctx->put_calls;
    counting_unlock(ctx);
    log_push(ctx, 'P', key);
    return otsardb_store_put(ctx->inner, key, data, size, options, out_etag);
}

static otsardb_store_result counting_put_stream(otsardb_object_store *store,
                                              const char *key,
                                              otsardb_store_source_read source_read,
                                              void *source_ctx,
                                              uint64_t expected_size,
                                              const otsardb_store_put_options *options,
                                              char **out_etag) {
    counting_store *ctx = (counting_store *)store->context;
    size_t index;
    counting_lock(ctx);
    index = ++ctx->put_stream_calls;
    counting_unlock(ctx);
    log_push(ctx, 'S', key);
    if (ctx->fail_put_stream_index != 0 && index == ctx->fail_put_stream_index) {
        return OTSARDB_STORE_ERROR;
    }
    if (ctx->fail_put_stream_count > 0 &&
        index >= ctx->fail_put_stream_first &&
        index < ctx->fail_put_stream_first + ctx->fail_put_stream_count) {
        return OTSARDB_STORE_ERROR;
    }
    return otsardb_store_put_stream(ctx->inner,
                                  key,
                                  source_read,
                                  source_ctx,
                                  expected_size,
                                  options,
                                  out_etag);
}

static void counting_release(otsardb_object_store *store, otsardb_store_object *object) {
    counting_store *ctx = (counting_store *)store->context;
    otsardb_store_release_object(ctx->inner, object);
}

static otsardb_store_result counting_delete(otsardb_object_store *store, const char *key) {
    counting_store *ctx = (counting_store *)store->context;
    return otsardb_store_delete(ctx->inner, key);
}

static otsardb_store_result counting_list(otsardb_object_store *store,
                                        const char *prefix,
                                        otsardb_store_list_callback callback,
                                        void *ctx) {
    counting_store *counting = (counting_store *)store->context;
    return otsardb_store_list(counting->inner, prefix, callback, ctx);
}

static const otsardb_object_store_ops COUNTING_OPS = {
    .get = counting_get,
    .get_range = counting_get_range,
    .put = counting_put,
    .put_stream = counting_put_stream,
    .release_object = counting_release,
    .delete_object = counting_delete,
    .list = counting_list,
};

static otsardb_object_store *counting_store_create(otsardb_store_capabilities caps,
                                                 size_t fail_put_stream_index) {
    otsardb_object_store *inner =
        otsardb_test_memory_store_create_with_capabilities(caps);
    if (!inner) return NULL;
    otsardb_object_store *store = (otsardb_object_store *)calloc(1, sizeof(*store));
    counting_store *ctx = (counting_store *)calloc(1, sizeof(*ctx));
    if (!store || !ctx) {
        free(store);
        free(ctx);
        otsardb_test_memory_store_destroy(inner);
        return NULL;
    }
    ctx->inner = inner;
    ctx->fail_put_stream_index = fail_put_stream_index;
#if defined(_WIN32)
    InitializeCriticalSection(&ctx->lock);
#else
    pthread_mutex_init(&ctx->lock, NULL);
#endif
    store->ops = &COUNTING_OPS;
    store->context = ctx;
    store->capabilities = caps;
    return store;
}

static void counting_store_destroy(otsardb_object_store *store) {
    if (!store) return;
    counting_store *ctx = (counting_store *)store->context;
    if (ctx) {
        otsardb_test_memory_store_destroy(ctx->inner);
#if defined(_WIN32)
        DeleteCriticalSection(&ctx->lock);
#else
        pthread_mutex_destroy(&ctx->lock);
#endif
    }
    free(ctx);
    free(store);
}

static int collect_text(void *ctx, int argc, char **argv, char **columns) {
    (void)columns;
    if (argc != 1 || !argv || !argv[0]) return 1;
    char *output = (char *)ctx;
    snprintf(output, 64, "%s", argv[0]);
    return 0;
}

static otsardb_head read_head(otsardb_object_store *store, const char *root) {
    otsardb_head head;
    char *etag = NULL;
    assert(otsardb_journal_read_head(store, root, &head, &etag) == OTSARDB_JOURNAL_OK);
    free(etag);
    return head;
}

/* Set up a publisher + database with a tiny segment budget so the big
 * INSERT statement below becomes one commit with several segments. */
static void run_scenario(otsardb_object_store *store,
                         const char *source_path,
                         const char *database_root,
                         const char *database_id,
                         otsardb_wal_publisher *publisher,
                         otsardb **db_out) {
    const char *wal_path = "otsardb-parallel-segments.db-wal";
    const char *shm_path = "otsardb-parallel-segments.db-shm";
    otsardb_unlink(source_path);
    otsardb_unlink(wal_path);
    otsardb_unlink(shm_path);

    assert(otsardb_wal_publisher_init(publisher,
                                    store,
                                    database_root,
                                    database_id,
                                    OTSARDB_WRITE_STRATEGY_DEFAULT));
    assert(publisher->strategy == OTSARDB_WRITE_STRATEGY_CAS);
    otsardb_wal_publisher_set_segment_payload_limit(
        publisher,
        CHUNK_FRAMES * (uint64_t)(OTSARDB_WAL_BATCH_FRAME_HEADER_SIZE + 4096u));
    otsardb_local_vfs_set_wal_reader_sync_hook(otsardb_wal_publisher_sync_reader, publisher);

    otsardb *db = NULL;
    assert(otsardb_open(source_path, &db) == OTSARDB_OK);
    char *error_message = NULL;
    assert(otsardb_exec(db,
                      "PRAGMA journal_mode=WAL;"
                      "PRAGMA wal_autocheckpoint=0;"
                      "CREATE TABLE items(id INTEGER PRIMARY KEY, value TEXT);",
                      NULL,
                      NULL,
                      &error_message) == OTSARDB_OK);
    otsardb_free(error_message);
    error_message = NULL;
    *db_out = db;
}

static void teardown(otsardb *db, const char *source_path) {
    otsardb_local_vfs_set_wal_reader_sync_hook(NULL, NULL);
    otsardb_close(db);
    otsardb_unlink(source_path);
    otsardb_unlink("otsardb-parallel-segments.db-wal");
    otsardb_unlink("otsardb-parallel-segments.db-shm");
}

int main(void) {
    const char *source_path = "otsardb-parallel-segments.db";
    const char *recovered_path = "otsardb-parallel-segments-recovered.db";
    const char *database_root = "parallel-segments/orders";
    const char *database_id = "parallel-segments-orders";

    char big_insert[512];
    int sql_len = snprintf(big_insert,
                           sizeof(big_insert),
                           "WITH RECURSIVE cnt(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM cnt WHERE x < %u) "
                           "INSERT INTO items(value) SELECT 'v' || x FROM cnt;",
                           ROWS);
    assert(sql_len > 0 && (size_t)sql_len < sizeof(big_insert));

    assert(otsardb_initialize() == OTSARDB_OK);
    otsardb_unlink(recovered_path);

    /* ------------------------------------------------------------------ */
    /* Scenario 1: MULTIPART-capable store -> segments pre-uploaded via
     * put_stream, ordered before the manifest PUT, HEAD advances, recovery
     * applies everything. */
    otsardb_object_store *store1 =
        counting_store_create(OTSARDB_STORE_CAP_ETAG |
                                  OTSARDB_STORE_CAP_RANGE_GET |
                                  OTSARDB_STORE_CAP_READ_AFTER_WRITE |
                                  OTSARDB_STORE_CAP_CONDITIONAL_CREATE |
                                  OTSARDB_STORE_CAP_CONDITIONAL_UPDATE |
                                  OTSARDB_STORE_CAP_MULTIPART,
                              0);
    assert(store1 != NULL);
    counting_store *ctx1 = (counting_store *)store1->context;

    otsardb_wal_publisher publisher1;
    otsardb *db1 = NULL;
    run_scenario(store1, source_path, database_root, database_id, &publisher1, &db1);

    char *error_message = NULL;
    assert(otsardb_exec(db1, big_insert, NULL, NULL, &error_message) == OTSARDB_OK);
    otsardb_free(error_message);
    error_message = NULL;

    otsardb_head head1 = read_head(store1, database_root);
    assert(head1.generation == 2); /* CREATE TABLE + big INSERT */

    otsardb_store_object manifest_object = {0};
    assert(otsardb_store_get(store1, head1.commit_key, &manifest_object) == OTSARDB_STORE_OK);
    otsardb_commit_manifest manifest1;
    assert(otsardb_commit_manifest_decode_json((const char *)manifest_object.data,
                                             manifest_object.size,
                                             &manifest1));
    assert(manifest1.segment_count >= 2);

    /* Every segment of the commit went through put_stream (exactly
     * segment_count streamed uploads). Ordering: all streamed segment
     * uploads of the commit are contiguous and happen BEFORE the commit's
     * manifest PUT and the HEAD CAS (the commit_publish verification PUTs
     * and the manifest/HEAD PUTs come after them). */
    assert(ctx1->put_stream_calls == manifest1.segment_count);
    size_t first_streamed = LOG_CAPACITY;
    size_t last_streamed = 0;
    size_t last_manifest_put = 0;
    for (size_t i = 0; i < ctx1->log_count; ++i) {
        const call_log_entry *entry = &ctx1->log[i];
        if (entry->kind == 'S') {
            assert(strstr(entry->key, "/segments/") != NULL);
            if (first_streamed == LOG_CAPACITY) first_streamed = i;
            last_streamed = i;
        } else if (entry->kind == 'P' && strstr(entry->key, "/commits/") != NULL) {
            last_manifest_put = i;
        }
    }
    assert(first_streamed != LOG_CAPACITY);
    assert(first_streamed <= last_streamed);
    /* No manifest PUT may interleave the streamed segment uploads... */
    for (size_t i = first_streamed; i <= last_streamed; ++i) {
        if (ctx1->log[i].kind == 'P' && strstr(ctx1->log[i].key, "/commits/") != NULL) {
            assert(0);
        }
    }
    /* ... and the commit's own manifest PUT follows the last streamed
     * segment (the HEAD CAS comes after the manifest PUT by construction of
     * otsardb_publish_commit, unchanged). */
    assert(last_manifest_put > last_streamed);

    otsardb_commit_manifest_free(&manifest1);
    otsardb_store_release_object(store1, &manifest_object);
    teardown(db1, source_path);

    /* Zero-local recovery applies every pre-uploaded segment. */
    assert(otsardb_recovery_restore_store(store1, database_root, recovered_path));
    otsardb *recovered1 = NULL;
    assert(otsardb_open(recovered_path, &recovered1) == OTSARDB_OK);
    char count1[64] = {0};
    error_message = NULL;
    assert(otsardb_exec(recovered1,
                      "SELECT count(*) FROM items;",
                      collect_text,
                      count1,
                      &error_message) == OTSARDB_OK);
    otsardb_free(error_message);
    char expected1[64];
    snprintf(expected1, sizeof(expected1), "%u", ROWS);
    assert(strcmp(count1, expected1) == 0);
    otsardb_close(recovered1);
    otsardb_unlink(recovered_path);
    counting_store_destroy(store1);

    /* ------------------------------------------------------------------ */
    /* Scenario 2: store WITHOUT the multipart capability -> zero put_stream
     * calls; the current sequential PUT path publishes and recovers. */
    otsardb_object_store *store2 =
        counting_store_create(OTSARDB_STORE_CAP_ETAG |
                                  OTSARDB_STORE_CAP_RANGE_GET |
                                  OTSARDB_STORE_CAP_READ_AFTER_WRITE |
                                  OTSARDB_STORE_CAP_CONDITIONAL_CREATE |
                                  OTSARDB_STORE_CAP_CONDITIONAL_UPDATE,
                              0);
    assert(store2 != NULL);
    counting_store *ctx2 = (counting_store *)store2->context;

    otsardb_wal_publisher publisher2;
    otsardb *db2 = NULL;
    run_scenario(store2, source_path, database_root, database_id, &publisher2, &db2);

    error_message = NULL;
    assert(otsardb_exec(db2, big_insert, NULL, NULL, &error_message) == OTSARDB_OK);
    otsardb_free(error_message);
    error_message = NULL;

    otsardb_head head2 = read_head(store2, database_root);
    assert(head2.generation == 2);
    assert(ctx2->put_stream_calls == 0);
    assert(ctx2->put_calls > 0);
    teardown(db2, source_path);
    counting_store_destroy(store2);

    /* ------------------------------------------------------------------ */
    /* Scenario 3: put_stream failure on the second segment -> the commit
     * fails, HEAD stays at generation 2, and no manifest is written for
     * the failed commit (durability contract: no HEAD advance on failure).
     * the bounded retry would mask a single transient
     * failure, so it is disabled (1 attempt) for this fail-fast scenario. */
    set_env("OTSARDB_PUBLISH_RETRIES", "1");
    set_env("OTSARDB_PUBLISH_RETRY_BASE_MS", "0");
    otsardb_object_store *store3 =
        counting_store_create(OTSARDB_STORE_CAP_ETAG |
                                  OTSARDB_STORE_CAP_RANGE_GET |
                                  OTSARDB_STORE_CAP_READ_AFTER_WRITE |
                                  OTSARDB_STORE_CAP_CONDITIONAL_CREATE |
                                  OTSARDB_STORE_CAP_CONDITIONAL_UPDATE |
                                  OTSARDB_STORE_CAP_MULTIPART,
                              2);
    assert(store3 != NULL);
    counting_store *ctx3 = (counting_store *)store3->context;

    otsardb_wal_publisher publisher3;
    otsardb *db3 = NULL;
    run_scenario(store3, source_path, database_root, database_id, &publisher3, &db3);

    otsardb_head head_before_fail = read_head(store3, database_root);
    /* Generation 1 = the CREATE TABLE commit; the multi-segment INSERT
     * below is the first commit that triggers the pre-upload. */
    assert(head_before_fail.generation == 1);

    error_message = NULL;
    assert(otsardb_exec(db3, big_insert, NULL, NULL, &error_message) != OTSARDB_OK);
    otsardb_free(error_message);
    error_message = NULL;

    otsardb_head head_after_fail = read_head(store3, database_root);
    assert(head_after_fail.generation == 1);
    assert(strcmp(head_after_fail.commit_id, head_before_fail.commit_id) == 0);
    /* The first segment upload succeeded before the failure; the rest never
     * ran. Only the gen-1 (CREATE TABLE) manifest may exist - no manifest
     * PUT for the failed commit, i.e. no HEAD advance and no commit that
     * could reference the half-uploaded segments. */
    assert(ctx3->put_stream_calls == 2);
    size_t manifest_puts = 0;
    for (size_t i = 0; i < ctx3->log_count; ++i) {
        if (ctx3->log[i].kind == 'P' && strstr(ctx3->log[i].key, "/commits/") != NULL) {
            ++manifest_puts;
        }
    }
    assert(manifest_puts == 1);
    teardown(db3, source_path);
    counting_store_destroy(store3);

    /* ------------------------------------------------------------------ */
    /* Scenario 4: transient put_stream failures on the
     * pre-upload are retried with bounded backoff (single worker so the
     * call indices are deterministic): segment 1's upload fails twice and
     * succeeds on its third attempt, every later segment uploads on its
     * first attempt -> put_stream_calls == segment_count + 2, the commit
     * succeeds and HEAD advances exactly once. */
    set_env("OTSARDB_PUBLISH_RETRIES", "3");
    set_env("OTSARDB_PUBLISH_RETRY_BASE_MS", "0");
    set_env("OTSARDB_S3_PARALLEL_SEGMENTS", "1");
    otsardb_object_store *store4 =
        counting_store_create(OTSARDB_STORE_CAP_ETAG |
                                  OTSARDB_STORE_CAP_RANGE_GET |
                                  OTSARDB_STORE_CAP_READ_AFTER_WRITE |
                                  OTSARDB_STORE_CAP_CONDITIONAL_CREATE |
                                  OTSARDB_STORE_CAP_CONDITIONAL_UPDATE |
                                  OTSARDB_STORE_CAP_MULTIPART,
                              0);
    assert(store4 != NULL);
    counting_store *ctx4 = (counting_store *)store4->context;
    counting_store_fail_streams(store4, 1, 2);

    otsardb_wal_publisher publisher4;
    otsardb *db4 = NULL;
    run_scenario(store4, source_path, database_root, database_id, &publisher4, &db4);

    error_message = NULL;
    assert(otsardb_exec(db4, big_insert, NULL, NULL, &error_message) == OTSARDB_OK);
    otsardb_free(error_message);
    error_message = NULL;

    otsardb_head head4 = read_head(store4, database_root);
    assert(head4.generation == 2);
    otsardb_store_object manifest_object4 = {0};
    assert(otsardb_store_get(store4, head4.commit_key, &manifest_object4) == OTSARDB_STORE_OK);
    otsardb_commit_manifest manifest4;
    assert(otsardb_commit_manifest_decode_json((const char *)manifest_object4.data,
                                             manifest_object4.size,
                                             &manifest4));
    assert(manifest4.segment_count >= 2);
    /* Segments 1..S: call 1 = attempt 1 of seg 1 (FAIL), call 2 = attempt 2
     * of seg 1 (FAIL), call 3 = attempt 3 of seg 1 (OK), calls 4..S+2 =
     * segs 2..S first attempts. */
    assert(ctx4->put_stream_calls == manifest4.segment_count + 2u);
    otsardb_commit_manifest_free(&manifest4);
    otsardb_store_release_object(store4, &manifest_object4);
    teardown(db4, source_path);
    counting_store_destroy(store4);

    /* ------------------------------------------------------------------ */
    /* Scenario 5: the pre-upload retry budget is exhausted
     * -> the sync fails and HEAD stays at generation 1 (the commit is not
     * acknowledged). Two attempts of the first segment = 2 put_stream calls
     * total. */
    set_env("OTSARDB_PUBLISH_RETRIES", "2");
    set_env("OTSARDB_PUBLISH_RETRY_BASE_MS", "0");
    set_env("OTSARDB_S3_PARALLEL_SEGMENTS", "1");
    otsardb_object_store *store5 =
        counting_store_create(OTSARDB_STORE_CAP_ETAG |
                                  OTSARDB_STORE_CAP_RANGE_GET |
                                  OTSARDB_STORE_CAP_READ_AFTER_WRITE |
                                  OTSARDB_STORE_CAP_CONDITIONAL_CREATE |
                                  OTSARDB_STORE_CAP_CONDITIONAL_UPDATE |
                                  OTSARDB_STORE_CAP_MULTIPART,
                              0);
    assert(store5 != NULL);
    counting_store *ctx5 = (counting_store *)store5->context;
    counting_store_fail_streams(store5, 1, 100);

    otsardb_wal_publisher publisher5;
    otsardb *db5 = NULL;
    run_scenario(store5, source_path, database_root, database_id, &publisher5, &db5);

    otsardb_head head_before_exhaust = read_head(store5, database_root);
    assert(head_before_exhaust.generation == 1);

    error_message = NULL;
    assert(otsardb_exec(db5, big_insert, NULL, NULL, &error_message) != OTSARDB_OK);
    otsardb_free(error_message);
    error_message = NULL;

    otsardb_head head_after_exhaust = read_head(store5, database_root);
    assert(head_after_exhaust.generation == 1);
    assert(strcmp(head_after_exhaust.commit_id, head_before_exhaust.commit_id) == 0);
    /* One segment job, two attempts, both failed. */
    assert(ctx5->put_stream_calls == 2);
    teardown(db5, source_path);
    counting_store_destroy(store5);

    otsardb_unlink(recovered_path);
    puts("parallel multipart segment publish (pre-upload, ordering, fallback, failure, retry) passed");
    return 0;
}
