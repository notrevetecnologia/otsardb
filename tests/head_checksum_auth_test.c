/* head_checksum_auth_test.c — regression test for 
 * 0151, failure-mode audit finding B4 (recovery.c validate_head_checksum
 * decoded the object at head.commit_key without verifying its SHA-256 equals
 * head.commit_sha256).
 *
 * B4 (LOW, defense-in-depth/TOCTOU): the chain walk authenticates the head
 * manifest against HEAD's commit_sha256, but validate_head_checksum performs
 * a SECOND independent GET of head.commit_key and, pre-,
 * decoded whatever it read WITHOUT re-verifying the hash. A manifest swapped
 * between the walk and the checksum read (store fault, lying/corrupt provider,
 * bit-rot race) could skip the whole-file checksum gate entirely — e.g. a
 * swap to a checksum-less manifest makes validate_head_checksum return
 * "pass" without ever comparing the image.
 *
 * fix: validate_head_checksum now requires
 * sha256(object at head.commit_key) == head.commit_sha256 before decoding;
 * any mismatch fails the restore closed (recovery returns 0).
 *
 * This test asserts, on a REAL published chain (WAL publisher + memory store)
 * + REAL recovery path:
 * 1. Contract (end-to-end): a HEAD.json whose commit_sha256 does NOT match
 * the manifest at commit_key is refused at recovery; the matching HEAD
 * passes.
 * 2. B4 discrimination (TOCTOU injection): a store wrapper returns the true
 * head manifest for the walk/apply reads and a SWAPPED checksum-less
 * manifest for the final validate_head_checksum read. Pre-fix this was
 * accepted (the swapped manifest silently skipped the gate); post-fix
 * recovery fails closed on the hash mismatch.
 *
 * Deterministic, no network, no sleeps. No checkpoint is created, so
 * recovery exercises the full-chain path.
 *
 * Wiring (coordinator): NOT applied here; CMakeLists.txt is
 * out of scope for this change set):
 * add_executable(otsardb_head_checksum_auth_test
 * tests/head_checksum_auth_test.c tests/memory_store.c)
 * target_include_directories(otsardb_head_checksum_auth_test
 * PRIVATE include src/core src/crypto src/storage tests)
 * target_link_libraries(otsardb_head_checksum_auth_test
 * PRIVATE otsardb_core)
 * add_test(NAME otsardb_head_checksum_auth
 * COMMAND otsardb_head_checksum_auth_test)
 * set_tests_properties(otsardb_head_checksum_auth
 * PROPERTIES TIMEOUT 600)
 */
#include "commit_manifest.h"
#include "journal.h"
#include "local_vfs.h"
#include "memory_store.h"
#include "object_store.h"
#include "otsardb.h"
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

#define B4_ROOT "151b4"
#define B4_DB_ID "0151-b4-db"
#define SOURCE "m0151-src.db"
#define WAL_PATH "m0151-src.db-wal"
#define SHM_PATH "m0151-src.db-shm"

static void cleanup_files(void) {
    otsardb_unlink(SOURCE);
    otsardb_unlink(WAL_PATH);
    otsardb_unlink(SHM_PATH);
}

/* Publish one commit through the real WAL publisher (a genuine SQLite WAL
 * batch), so the chain is exactly what recovery materialises. */
static void publish_commit(otsardb_object_store *store,
                           const char *root,
                           const char *db_id,
                           const char *sql) {
    cleanup_files();
    otsardb_wal_publisher publisher;
    assert(otsardb_wal_publisher_init(&publisher, store, root, db_id,
                                      OTSARDB_WRITE_STRATEGY_DEFAULT));
    otsardb_local_vfs_set_wal_reader_sync_hook(otsardb_wal_publisher_sync_reader,
                                               &publisher);

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
                        NULL, NULL, &error_message) == OTSARDB_OK);
    otsardb_free(error_message);
    error_message = NULL;
    assert(otsardb_exec(db, sql, NULL, NULL, &error_message) == OTSARDB_OK);
    otsardb_free(error_message);
    otsardb_close(db);
    otsardb_local_vfs_set_wal_reader_sync_hook(NULL, NULL);
    cleanup_files();
}

static void read_head(otsardb_object_store *store, const char *root,
                      otsardb_head *out) {
    char *etag = NULL;
    assert(otsardb_journal_read_head(store, root, out, &etag) == OTSARDB_JOURNAL_OK);
    free(etag);
}

/* Test 1 — end-to-end contract: HEAD.json with a commit_sha256 that does NOT
 * match the manifest at commit_key must be refused at recovery; the matching
 * HEAD passes. */
static void test_head_sha_mismatch_refused(void) {
    otsardb_object_store *store = otsardb_test_memory_store_create();
    assert(store);
    publish_commit(store, B4_ROOT, B4_DB_ID,
                   "CREATE TABLE t(v INTEGER); INSERT INTO t VALUES (42);");

    otsardb_head head;
    read_head(store, B4_ROOT, &head);
    char head_key[OTSARDB_KEY_MAX + 1];
    snprintf(head_key, sizeof(head_key), "%s/HEAD.json", B4_ROOT);

    /* Sanity: the pristine chain recovers. */
    assert(otsardb_recovery_restore_store(store, B4_ROOT, SOURCE));

    /* Capture the pristine HEAD.json bytes for the restore control. */
    otsardb_store_object head_obj = {0};
    assert(otsardb_store_get(store, head_key, &head_obj) == OTSARDB_STORE_OK);
    unsigned char *pristine = (unsigned char *)malloc(head_obj.size);
    assert(pristine);
    memcpy(pristine, head_obj.data, head_obj.size);
    size_t pristine_size = head_obj.size;

    /* Tamper HEAD's commit_sha256 (flip one hex digit, keep length/shape). */
    otsardb_head tampered = {0};
    assert(otsardb_head_decode_json((const char *)head_obj.data, head_obj.size,
                                    &tampered));
    otsardb_store_release_object(store, &head_obj);
    assert(strlen(tampered.commit_sha256) == OTSARDB_MANIFEST_SHA256_HEX_SIZE);
    char flipped = tampered.commit_sha256[0];
    tampered.commit_sha256[0] = (char)(flipped == '0' ? '1' : '0');

    char *tampered_json = NULL;
    size_t tampered_size = 0;
    assert(otsardb_head_encode_json(&tampered, &tampered_json, &tampered_size));
    otsardb_store_put_options opts = {0};
    assert(otsardb_store_put(store, head_key, tampered_json, tampered_size,
                             &opts, NULL) == OTSARDB_STORE_OK);
    free(tampered_json);

    /* The head manifest no longer matches commit_key's object hash: recovery
     * MUST fail closed. */
    assert(!otsardb_recovery_restore_store(store, B4_ROOT, SOURCE));

    /* Restore the pristine HEAD -> recovery passes again. */
    assert(otsardb_store_put(store, head_key, pristine, pristine_size,
                             &opts, NULL) == OTSARDB_STORE_OK);
    assert(otsardb_recovery_restore_store(store, B4_ROOT, SOURCE));

    free(pristine);
    otsardb_test_memory_store_destroy(store);
    cleanup_files();
}

/* ---- B4 TOCTOU injection: swap the head manifest on the
 * validate_head_checksum re-read ---- */

typedef struct swap_ctx {
    otsardb_object_store *inner;
    char head_key[OTSARDB_KEY_MAX + 1];
    unsigned char *swap_bytes;
    size_t swap_size;
    unsigned long long head_reads;
    unsigned long long swap_on_read; /* 0 = never swap */
} swap_ctx;

static otsardb_store_result swap_get(otsardb_object_store *store,
                                    const char *key,
                                    otsardb_store_object *out) {
    swap_ctx *ctx = (swap_ctx *)store->context;
    if (strcmp(key, ctx->head_key) == 0) {
        ++ctx->head_reads;
        if (ctx->swap_on_read != 0 && ctx->head_reads >= ctx->swap_on_read) {
            memset(out, 0, sizeof(*out));
            out->data = (unsigned char *)malloc(ctx->swap_size ? ctx->swap_size : 1u);
            assert(out->data);
            if (ctx->swap_size) memcpy(out->data, ctx->swap_bytes, ctx->swap_size);
            out->size = ctx->swap_size;
            out->etag = (char *)malloc(2u);
            assert(out->etag);
            out->etag[0] = 'v';
            out->etag[1] = '\0';
            return OTSARDB_STORE_OK;
        }
    }
    return otsardb_store_get(ctx->inner, key, out);
}

static otsardb_store_result swap_get_range(otsardb_object_store *store,
                                          const char *key,
                                          size_t offset,
                                          size_t length,
                                          otsardb_store_object *out) {
    swap_ctx *ctx = (swap_ctx *)store->context;
    return otsardb_store_get_range(ctx->inner, key, offset, length, out);
}

static otsardb_store_result swap_put(otsardb_object_store *store,
                                    const char *key,
                                    const void *data,
                                    size_t size,
                                    const otsardb_store_put_options *options,
                                    char **out_etag) {
    swap_ctx *ctx = (swap_ctx *)store->context;
    return otsardb_store_put(ctx->inner, key, data, size, options, out_etag);
}

static otsardb_store_result swap_put_stream(otsardb_object_store *store,
                                           const char *key,
                                           otsardb_store_source_read source_read,
                                           void *source_ctx,
                                           uint64_t expected_size,
                                           const otsardb_store_put_options *options,
                                           char **out_etag) {
    swap_ctx *ctx = (swap_ctx *)store->context;
    return otsardb_store_put_stream(ctx->inner, key, source_read, source_ctx,
                                    expected_size, options, out_etag);
}

static void swap_release(otsardb_object_store *store, otsardb_store_object *object) {
    swap_ctx *ctx = (swap_ctx *)store->context;
    otsardb_store_release_object(ctx->inner, object);
}

static otsardb_store_result swap_delete(otsardb_object_store *store, const char *key) {
    swap_ctx *ctx = (swap_ctx *)store->context;
    return otsardb_store_delete(ctx->inner, key);
}

static otsardb_store_result swap_list(otsardb_object_store *store,
                                     const char *prefix,
                                     otsardb_store_list_callback callback,
                                     void *ctx) {
    swap_ctx *c = (swap_ctx *)store->context;
    return otsardb_store_list(c->inner, prefix, callback, ctx);
}

static const otsardb_object_store_ops SWAP_OPS = {
    .get = swap_get,
    .get_range = swap_get_range,
    .put = swap_put,
    .put_stream = swap_put_stream,
    .release_object = swap_release,
    .delete_object = swap_delete,
    .list = swap_list,
};

static otsardb_object_store *swap_store_create(otsardb_object_store *inner,
                                               swap_ctx *ctx) {
    otsardb_object_store *store = (otsardb_object_store *)calloc(1, sizeof(*store));
    assert(store);
    store->ops = &SWAP_OPS;
    store->context = ctx;
    store->capabilities = inner->capabilities;
    return store;
}

/* Build the swapped manifest: the REAL head manifest re-encoded with its
 * post_apply_checksum dropped. Valid manifest, different bytes/hash — exactly
 * the object a TOCTOU swap would substitute to skip the checksum gate. */
static void build_swap_manifest(otsardb_object_store *inner,
                                const char *head_key,
                                const otsardb_head *head,
                                unsigned char **out,
                                size_t *out_size) {
    otsardb_store_object obj = {0};
    assert(otsardb_store_get(inner, head_key, &obj) == OTSARDB_STORE_OK);
    otsardb_commit_manifest manifest = {0};
    assert(otsardb_commit_manifest_decode_json((const char *)obj.data, obj.size,
                                               &manifest));
    otsardb_store_release_object(inner, &obj);

    /* The head manifest MUST carry a checksum, so dropping it changes the
     * bytes (otherwise the swap would be byte-identical and not a swap). */
    assert(manifest.post_apply_checksum[0] != '\0');

    manifest.post_apply_checksum[0] = '\0';
    char *json = NULL;
    size_t json_size = 0;
    char swap_sha[OTSARDB_MANIFEST_SHA256_HEX_SIZE + 1];
    assert(otsardb_commit_manifest_encode_json(&manifest, &json, &json_size,
                                               swap_sha));
    otsardb_commit_manifest_free(&manifest);

    assert(strcmp(swap_sha, head->commit_sha256) != 0);
    *out = (unsigned char *)json;
    *out_size = json_size;
}

/* Test 2 — B4 discrimination: the head manifest swapped between the chain
 * walk/apply reads and the validate_head_checksum re-read must FAIL the
 * restore closed (the object no longer hashes to head.commit_sha256).
 * Pre- the swapped checksum-less manifest silently passed the
 * gate and the restore succeeded. The wrapper asserts it saw exactly the
 * expected number of head-manifest reads (walk pass + apply pass + apply +
 * checksum re-read = 4 in the full-chain path), so a recovery-internal change
 * that alters the read pattern fails this test loudly instead of silently. */
static void test_swapped_head_manifest_refused(void) {
    otsardb_object_store *inner = otsardb_test_memory_store_create();
    assert(inner);
    publish_commit(inner, B4_ROOT, B4_DB_ID,
                   "CREATE TABLE t(v INTEGER); INSERT INTO t VALUES (7);");

    otsardb_head head;
    read_head(inner, B4_ROOT, &head);

    swap_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.inner = inner;
    snprintf(ctx.head_key, sizeof(ctx.head_key), "%s", head.commit_key);
    build_swap_manifest(inner, ctx.head_key, &head, &ctx.swap_bytes, &ctx.swap_size);
    /* The full-chain recovery path reads the head manifest exactly four
     * times: collect(validate) + collect(apply) + apply(newest) +
     * validate_head_checksum. The LAST read is the checksum re-read. */
    ctx.swap_on_read = 4;

    otsardb_object_store *store = swap_store_create(inner, &ctx);
    assert(store);

    /* Post-fix: the swapped (non-matching) manifest fails closed. */
    assert(!otsardb_recovery_restore_store(store, B4_ROOT, SOURCE));
    assert(ctx.head_reads == 4);

    free(ctx.swap_bytes);
    free(store);
    otsardb_test_memory_store_destroy(inner);
    cleanup_files();
}

int main(void) {
    assert(otsardb_initialize() == OTSARDB_OK);
    test_head_sha_mismatch_refused();
    test_swapped_head_manifest_refused();

    puts("B4 fix test passed: validate_head_checksum authenticates "
         "sha256(commit_key) == commit_sha256; a head manifest whose SHA does "
         "not match its key is refused at recovery ");
    return 0;
}
