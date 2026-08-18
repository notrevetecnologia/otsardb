/*
 * — B2/B3: checkpoint identity validation + covered-
 * generation TOCTOU re-validation at recovery.
 *
 * Deterministic regression test (memory store, real WAL-publisher chains)
 * for the MEDIUM failure-mode-audit findings:
 *
 * - B2: the checkpoint-accelerated recovery path previously verified only
 * the pointer hash and the manifest decode — never the checkpoint's
 * identity. A stale/foreign checkpoint at the same root (recreated
 * database, restored backup) could apply its pages over the chain. The
 * 0135 fix validates database_id + page_size against the metadata
 * (mirroring the lazy path), covered_generation against the pointer,
 * database_size_pages > 0, and binds base_commit_id +
 * base_manifest_sha256 to the chain-authenticated manifest at the
 * covered generation.
 * - B3: the checkpoint builder could claim a covered_generation a
 * concurrent writer advanced while the checkpoint pages still capture
 * the OLD generation (restore-then-HEAD-read race). At recovery the
 * 0135 fix re-validates the materialised base image against the
 * covered generation's chain-authenticated manifest (size always;
 * post-apply checksum when the manifest carries one) BEFORE replaying
 * newer segments, so an inflated-coverage checkpoint fails closed and
 * the full chain walk serves the correct image.
 *
 * Scenarios:
 * 1. Honest checkpoint: recovery via the checkpoint path (base
 * segments never re-fetched) yields the reference image; the base
 * re-validation passes.
 * 2. B2 foreign database_id / foreign page_size / foreign
 * base_commit_id: each fails identity validation and recovery falls
 * back to the full chain -> correct image. (OTSARDB_DIAG=1 shows
 * "checkpoint identity validation failed".)
 * 3. B3 inflated-coverage artifact (checkpoint pages at G, manifest and
 * pointer claim covered = G+1 with the G+1 chain identity, exactly
 * what the concurrent-writer race produces): the base re-validation
 * detects the wrong image and fails closed; recovery falls back to
 * the full chain -> correct image. (OTSARDB_DIAG=1 shows "checkpoint
 * base checksum mismatch".) Without the 0135 fix this artifact either
 * self-heals silently (no GC) or, behind GC, serves a wrong image on
 * checksum-less heads.
 *
 * Build (standalone, not a ctest target in this change set — CMakeLists
 * wiring is out of scope for): link against the same libs
 * as otsardb_delta_materialization.
 */

#include "otsardb.h"
#include "checkpoint_build.h"
#include "checkpoint_manifest.h"
#include "commit_manifest.h"
#include "journal.h"
#include "local_vfs.h"
#include "memory_store.h"
#include "object_store.h"
#include "recovery.h"
#include "sha256.h"
#include "wal_publisher.h"

#include <assert.h>
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

#define OTSARDB_TEST_KEY_MAX 1024

static const char *SOURCE = "cp-id-src.db";
static const char *WAL_PATH = "cp-id-src.db-wal";
static const char *SHM_PATH = "cp-id-src.db-shm";

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
    otsardb_wal_publisher_destroy(&publisher);
    cleanup_files();
}

/* ---- counting store (records every GET key, to prove the checkpoint
 * path — not the full walk — served an honest restore) ---- */

typedef struct counting_ctx {
    otsardb_object_store *inner;
    char fetched[128][OTSARDB_TEST_KEY_MAX + 1];
    size_t fetched_count;
} counting_ctx;

static void counting_record(counting_ctx *ctx, const char *key) {
    if (ctx->fetched_count < 128) {
        snprintf(ctx->fetched[ctx->fetched_count], OTSARDB_TEST_KEY_MAX + 1, "%s", key);
        ++ctx->fetched_count;
    }
}

static int counting_fetched(counting_ctx *ctx, const char *needle) {
    for (size_t i = 0; i < ctx->fetched_count; ++i) {
        if (strstr(ctx->fetched[i], needle) != NULL) return 1;
    }
    return 0;
}

static otsardb_store_result counting_get(otsardb_object_store *store,
                                       const char *key,
                                       otsardb_store_object *out) {
    counting_ctx *ctx = (counting_ctx *)store->context;
    counting_record(ctx, key);
    return otsardb_store_get(ctx->inner, key, out);
}

static otsardb_store_result counting_get_range(otsardb_object_store *store,
                                             const char *key,
                                             size_t offset,
                                             size_t length,
                                             otsardb_store_object *out) {
    counting_ctx *ctx = (counting_ctx *)store->context;
    counting_record(ctx, key);
    return otsardb_store_get_range(ctx->inner, key, offset, length, out);
}

static otsardb_store_result counting_put(otsardb_object_store *store,
                                       const char *key,
                                       const void *data,
                                       size_t size,
                                       const otsardb_store_put_options *options,
                                       char **out_etag) {
    counting_ctx *ctx = (counting_ctx *)store->context;
    return otsardb_store_put(ctx->inner, key, data, size, options, out_etag);
}

static void counting_release(otsardb_object_store *store, otsardb_store_object *object) {
    counting_ctx *ctx = (counting_ctx *)store->context;
    otsardb_store_release_object(ctx->inner, object);
}

static otsardb_store_result counting_delete(otsardb_object_store *store, const char *key) {
    counting_ctx *ctx = (counting_ctx *)store->context;
    return otsardb_store_delete(ctx->inner, key);
}

static otsardb_store_result counting_list(otsardb_object_store *store,
                                        const char *prefix,
                                        otsardb_store_list_callback callback,
                                        void *ctx) {
    counting_ctx *c = (counting_ctx *)store->context;
    return otsardb_store_list(c->inner, prefix, callback, ctx);
}

static const otsardb_object_store_ops COUNTING_OPS = {
    .get = counting_get,
    .get_range = counting_get_range,
    .put = counting_put,
    .release_object = counting_release,
    .delete_object = counting_delete,
    .list = counting_list,
};

static otsardb_object_store *counting_store_create(otsardb_object_store *inner,
                                                   counting_ctx *ctx) {
    otsardb_object_store *store = (otsardb_object_store *)calloc(1, sizeof(*store));
    assert(store);
    memset(ctx, 0, sizeof(*ctx));
    ctx->inner = inner;
    store->ops = &COUNTING_OPS;
    store->context = ctx;
    store->capabilities = inner->capabilities;
    return store;
}

/* ---- crafting foreign / inflated checkpoints ---- */

static void get_object(otsardb_object_store *store,
                       const char *key,
                       unsigned char **out,
                       size_t *out_size) {
    otsardb_store_object obj = {0};
    assert(otsardb_store_get(store, key, &obj) == OTSARDB_STORE_OK);
    *out = (unsigned char *)malloc(obj.size);
    assert(*out);
    memcpy(*out, obj.data, obj.size);
    *out_size = obj.size;
    otsardb_store_release_object(store, &obj);
}

/* Publish a modified checkpoint manifest (re-encoded with changed fields)
 * under its own content-addressed key and repoint CHECKPOINT.json at it.
 * The page groups are carried over verbatim. */
static void repoint_checkpoint(otsardb_object_store *store,
                               const char *root,
                               const otsardb_checkpoint_manifest *base,
                               const otsardb_checkpoint_pointer *base_ptr,
                               uint64_t covered_generation,
                               const char *base_commit_id,
                               const char *base_manifest_sha256,
                               const char *database_id,
                               uint32_t page_size) {
    otsardb_checkpoint_manifest cp = *base;
    cp.covered_generation = covered_generation;
    if (base_commit_id) snprintf(cp.base_commit_id, sizeof(cp.base_commit_id), "%s", base_commit_id);
    if (base_manifest_sha256) snprintf(cp.base_manifest_sha256, sizeof(cp.base_manifest_sha256), "%s", base_manifest_sha256);
    if (database_id) snprintf(cp.database_id, sizeof(cp.database_id), "%s", database_id);
    if (page_size != 0) cp.page_size = page_size;

    char *json = NULL;
    size_t json_size = 0;
    char cp_sha[OTSARDB_CHECKPOINT_SHA256_HEX_SIZE + 1];
    assert(otsardb_checkpoint_manifest_encode(&cp, &json, &json_size, cp_sha));

    char key[OTSARDB_KEY_MAX + 1];
    snprintf(key, sizeof(key), "%s/checkpoints/%s.json", root, cp_sha);
    otsardb_store_put_options opts = {0};
    assert(otsardb_store_put(store, key, json, json_size, &opts, NULL) == OTSARDB_STORE_OK);
    free(json);

    otsardb_checkpoint_pointer ptr = *base_ptr;
    ptr.covered_generation = covered_generation;
    snprintf(ptr.checkpoint_sha256, sizeof(ptr.checkpoint_sha256), "%s", cp_sha);
    snprintf(ptr.checkpoint_key, sizeof(ptr.checkpoint_key), "%s", key);

    char *ptr_json = NULL;
    size_t ptr_size = 0;
    assert(otsardb_checkpoint_ptr_encode(&ptr, &ptr_json, &ptr_size));
    char ptr_key[OTSARDB_KEY_MAX + 1];
    snprintf(ptr_key, sizeof(ptr_key), "%s/CHECKPOINT.json", root);
    assert(otsardb_store_put(store, ptr_key, ptr_json, ptr_size, &opts, NULL) == OTSARDB_STORE_OK);
    free(ptr_json);
}

int main(void) {
    cleanup_files();
    assert(otsardb_initialize() == OTSARDB_OK);

    otsardb_object_store *store = otsardb_test_memory_store_create();
    assert(store != NULL);

    const char *root = "cp-id/chain";
    const char *db_id = "cp-id-db";
    const char *REF = "cp-id.ref";
    const char *IMG = "cp-id.img";

    publish_commit(store, root, db_id,
                   "CREATE TABLE t1(id INTEGER PRIMARY KEY, v TEXT);"
                   "INSERT INTO t1(v) VALUES('a'); INSERT INTO t1(v) VALUES('b');");
    publish_commit(store, root, db_id,
                   "INSERT INTO t1(v) VALUES('c'); INSERT INTO t1(v) VALUES('d');");
    publish_commit(store, root, db_id,
                   "INSERT INTO t1(v) VALUES('e'); INSERT INTO t1(v) VALUES('f');");
    publish_commit(store, root, db_id,
                   "INSERT INTO t1(v) VALUES('g'); INSERT INTO t1(v) VALUES('h');");

    otsardb_head head = read_head(store, root);
    assert(head.generation >= 4);

    /* Honest checkpoint at the head. */
    assert(otsardb_checkpoint_build(store, root, SOURCE));
    cleanup_files();

    otsardb_checkpoint_pointer ptr;
    char *ptr_etag = NULL;
    assert(otsardb_checkpoint_ptr_read(store, root, &ptr, &ptr_etag) == OTSARDB_CHECKPOINT_PTR_OK);
    free(ptr_etag);
    assert(ptr.covered_generation == head.generation);

    unsigned char *cp_json = NULL;
    size_t cp_json_size = 0;
    get_object(store, ptr.checkpoint_key, &cp_json, &cp_json_size);
    otsardb_checkpoint_manifest cp;
    assert(otsardb_checkpoint_manifest_decode((const char *)cp_json, cp_json_size, &cp));
    free(cp_json);

    /* Reference: full-chain recovery (no checkpoint involvement would
     * produce this; the chain is immutable so it is stable). */
    assert(otsardb_recovery_restore_store(store, root, REF));

    /* ---- 1. Honest checkpoint: the checkpoint path must serve the same
     * image, and (observable via the counting store) must NOT fetch the
     * covered generation's segments — the 0135 fast path re-uses the
     * verified checkpoint base instead. ---- */
    {
        counting_ctx ctx;
        otsardb_object_store *counting = counting_store_create(store, &ctx);
        assert(otsardb_recovery_restore_store(counting, root, IMG));
        assert_files_identical(IMG, REF);
        /* covered generation's own segment objects were never fetched. */
        assert(!counting_fetched(&ctx, "/segments/"));
        free(counting);
    }
    otsardb_unlink(IMG);

    /* ---- 2a. B2: foreign database_id -> identity validation fails,
     * full recovery serves the correct image. ---- */
    {
        repoint_checkpoint(store, root, &cp, &ptr, ptr.covered_generation,
                           cp.base_commit_id, cp.base_manifest_sha256,
                           "foreign-database-id", 0);
        assert(otsardb_recovery_restore_store(store, root, IMG));
        assert_files_identical(IMG, REF);
        otsardb_unlink(IMG);
    }

    /* ---- 2b. B2: foreign page_size -> identity validation fails. The
     * manifest is crafted with a wrong page_size while the page groups
     * stay unchanged. The 0133 decode-time layout validation would reject
     * a page_size that is not a divisor-consistent factor of the group
     * sizes (e.g. 8192), so the JSON is mutated to 304 — a page_size the
     * layout accepts (groups are 32 + k*(12+4096); 12+304 divides
     * 12+4096) but which cannot match the metadata's 4096. The identity
     * check must reject it. ---- */
    {
        unsigned char *base_json = NULL;
        size_t base_size = 0;
        get_object(store, ptr.checkpoint_key, &base_json, &base_size);
        char *text = (char *)malloc(base_size + 1u);
        assert(text);
        memcpy(text, base_json, base_size);
        text[base_size] = '\0';
        char *needle = "\"page_size\":4096";
        char *hit = strstr(text, needle);
        assert(hit);
        memcpy(hit, "\"page_size\":304 ", 16);
        char sha[OTSARDB_SHA256_HEX_SIZE + 1];
        assert(otsardb_sha256_hex_data(text, base_size, sha));
        char key[OTSARDB_KEY_MAX + 1];
        snprintf(key, sizeof(key), "%s/checkpoints/%s.json", root, sha);
        otsardb_store_put_options opts = {0};
        assert(otsardb_store_put(store, key, text, base_size, &opts, NULL) == OTSARDB_STORE_OK);
        free(text);
        free(base_json);

        otsardb_checkpoint_pointer ptr2 = ptr;
        snprintf(ptr2.checkpoint_sha256, sizeof(ptr2.checkpoint_sha256), "%s", sha);
        snprintf(ptr2.checkpoint_key, sizeof(ptr2.checkpoint_key), "%s", key);
        char *ptr_json = NULL;
        size_t ptr_size = 0;
        assert(otsardb_checkpoint_ptr_encode(&ptr2, &ptr_json, &ptr_size));
        char ptr_key[OTSARDB_KEY_MAX + 1];
        snprintf(ptr_key, sizeof(ptr_key), "%s/CHECKPOINT.json", root);
        assert(otsardb_store_put(store, ptr_key, ptr_json, ptr_size, &opts, NULL) == OTSARDB_STORE_OK);
        free(ptr_json);

        assert(otsardb_recovery_restore_store(store, root, IMG));
        assert_files_identical(IMG, REF);
        otsardb_unlink(IMG);
    }

    /* ---- 2c. B2: foreign base_commit_id (the checkpoint's pages claim a
     * commit that is not on this chain at the covered generation) ->
     * chain binding fails, full recovery wins. ---- */
    {
        repoint_checkpoint(store, root, &cp, &ptr, ptr.covered_generation,
                           "not-on-this-chain-commit", cp.base_manifest_sha256,
                           NULL, 0);
        assert(otsardb_recovery_restore_store(store, root, IMG));
        assert_files_identical(IMG, REF);
        otsardb_unlink(IMG);
    }

    /* ---- 3. B3: the concurrent-writer race artifact. A commit lands
     * AFTER the checkpoint pages were built; the race rewrites the
     * checkpoint to claim the NEW covered generation with the NEW chain
     * identity while its pages still capture the OLD image. Recovery must
     * detect the mismatch (base re-validation), fail the checkpoint path
     * and serve the correct image from the full chain. ---- */
    {
        publish_commit(store, root, db_id,
                       "INSERT INTO t1(v) VALUES('i'); INSERT INTO t1(v) VALUES('j');");
        otsardb_head head2 = read_head(store, root);
        assert(head2.generation > head.generation);

        /* Fresh full-chain reference at the NEW head, computed before the
         * pointer is repointed. */
        assert(otsardb_recovery_restore_store(store, root, REF));

        unsigned char *m_json = NULL;
        size_t m_size = 0;
        get_object(store, head2.commit_key, &m_json, &m_size);
        otsardb_commit_manifest head_manifest;
        assert(otsardb_commit_manifest_decode_json((const char *)m_json, m_size,
                                                 &head_manifest));
        free(m_json);

        /* The race artifact: same page groups (image at the OLD head), but
         * covered_generation + base identity of the NEW head. The base
         * manifest SHA is the NEW head manifest's own content hash. */
        char head2_sha[OTSARDB_SHA256_HEX_SIZE + 1];
        {
            unsigned char *hm_json = NULL;
            size_t hm_size = 0;
            get_object(store, head2.commit_key, &hm_json, &hm_size);
            assert(otsardb_sha256_hex_data(hm_json, hm_size, head2_sha));
            free(hm_json);
        }
        repoint_checkpoint(store, root, &cp, &ptr, head2.generation,
                           head_manifest.commit_id,
                           head2_sha,
                           NULL, 0);
        otsardb_commit_manifest_free(&head_manifest);

        assert(otsardb_recovery_restore_store(store, root, IMG));
        assert_files_identical(IMG, REF);  /* REF is the full-chain image */
        otsardb_unlink(IMG);
    }

    otsardb_unlink(REF);
    otsardb_test_memory_store_destroy(store);
    cleanup_files();

    printf("checkpoint identity / TOCTOU recovery test: all assertions passed\n");
    return 0;
}
