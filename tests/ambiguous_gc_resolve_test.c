/* ambiguous_gc_resolve_test.c — regression test for 
 * 0151, failure-mode audit finding A5 (ambiguous commit whose parent region
 * was GC'd): the resolve now returns a DEFINITIVE result when the walk
 * descends below the checkpoint-covered boundary.
 *
 * A5 (MEDIUM): otsardb_resolve_commit walks HEAD down to the request's
 * parent_generation and needs every intermediate manifest. GC deletes
 * everything strictly below the checkpoint's covered_generation (gc.c),
 * keeping exactly the covered manifest. A resolve whose parent_generation is
 * >= 2 generations below the covered boundary walked into the swept region,
 * hit a deleted manifest and returned OTSARDB_JOURNAL_NOT_FOUND (error-class,
 * state UNKNOWN) — the commit is durable and unacknowledgeable. 
 * 0146 confirmed the hazard with a deterministic discriminator test; the fix
 * was delegated to commit_publish.c/gc.c.
 *
 * The audit prescribes: "Resolve from the checkpoint base manifest (kept
 * alive)". implements exactly that in commit_publish.c: when
 * the walk hits a NOT_FOUND reading a manifest at generation g, it
 * establishes the covered boundary from the CHECKPOINT.json pointer (hash-
 * bound to the checkpoint manifest, like recovery/gc). If g < covered, the
 * missing manifest is the GC-swept region and the commit is DEFINITIVELY not
 * on the authenticated live chain [covered..head]; since head.generation >
 * parent_generation the definitive classification is SUPERSEDED (never an
 * error). Without a valid checkpoint the missing manifest is
 * indistinguishable from genuine corruption and the resolve keeps failing
 * closed with NOT_FOUND.
 *
 * This test asserts the NEW definitive behavior and the preserved fail-closed
 * controls. The discriminator (tests/ambiguous_gc_test.c)
 * keeps asserting the fail-closed behavior WITHOUT a checkpoint and still
 * passes unchanged.
 *
 * Deterministic, no network, no sleeps: a purpose-built chain store holds a
 * valid hash-linked manifest chain in memory, serves a real checkpoint
 * pointer + manifest at the covered generation, and returns NOT_FOUND for
 * the generations GC would have swept.
 *
 * Wiring (coordinator): NOT applied here; CMakeLists.txt is
 * out of scope for this change set):
 * add_executable(otsardb_ambiguous_gc_resolve_test
 * tests/ambiguous_gc_resolve_test.c)
 * target_include_directories(otsardb_ambiguous_gc_resolve_test
 * PRIVATE include src/core src/crypto src/storage tests)
 * target_link_libraries(otsardb_ambiguous_gc_resolve_test
 * PRIVATE otsardb_core)
 * add_test(NAME otsardb_ambiguous_gc_resolve
 * COMMAND otsardb_ambiguous_gc_resolve_test)
 * set_tests_properties(otsardb_ambiguous_gc_resolve
 * PROPERTIES TIMEOUT 600)
 */
#include "checkpoint_manifest.h"
#include "commit_manifest.h"
#include "commit_publish.h"
#include "journal.h"
#include "object_store.h"
#include "sha256.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define A5_ROOT "151a5"
#define A5_DB_ID "0151-a5-db"
#define A5_SEG_SHA                                                              \
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define A5_CHAIN_GENERATIONS 100u
#define A5_COVERED_GENERATION 90u /* GC keeps exactly covered and above */

typedef struct chain_object {
    unsigned char *data;
    size_t size;
} chain_object;

typedef struct chain_store_ctx {
    chain_object *manifests;
    uint64_t n_generations;
    uint64_t gc_min_generation; /* present: >= gc_min; absent below (swept) */
    unsigned char *head_json;
    size_t head_size;
    /* Optional checkpoint (pointer + content-addressed manifest), proving the
     * covered boundary the same way recovery/gc establish it. */
    unsigned char *cp_pointer;
    size_t cp_pointer_size;
    char cp_manifest_key[OTSARDB_KEY_MAX + 1];
    unsigned char *cp_manifest;
    size_t cp_manifest_size;
} chain_store_ctx;

static chain_object *chain_find(chain_store_ctx *ctx, const char *key) {
    static const char prefix[] = A5_ROOT "/commits/c";
    if (strncmp(key, prefix, sizeof(prefix) - 1u) == 0) {
        char *end = NULL;
        unsigned long long generation = strtoull(key + sizeof(prefix) - 1u, &end, 10);
        if (end && strcmp(end, ".json") == 0 && generation >= 1 &&
            generation <= ctx->n_generations) {
            return &ctx->manifests[generation];
        }
    }
    return NULL;
}

static otsardb_store_result chain_get(otsardb_object_store *store,
                                    const char *key,
                                    otsardb_store_object *out) {
    chain_store_ctx *ctx = (chain_store_ctx *)store->context;
    if (!ctx || !key || !out) return OTSARDB_STORE_ERROR;
    memset(out, 0, sizeof(*out));

    const unsigned char *data = NULL;
    size_t size = 0;
    if (strcmp(key, A5_ROOT "/HEAD.json") == 0) {
        data = ctx->head_json;
        size = ctx->head_size;
    } else if (strcmp(key, A5_ROOT "/CHECKPOINT.json") == 0) {
        if (!ctx->cp_pointer) return OTSARDB_STORE_NOT_FOUND;
        data = ctx->cp_pointer;
        size = ctx->cp_pointer_size;
    } else if (ctx->cp_manifest_key[0] != '\0' &&
               strcmp(key, ctx->cp_manifest_key) == 0) {
        data = ctx->cp_manifest;
        size = ctx->cp_manifest_size;
    } else {
        chain_object *obj = chain_find(ctx, key);
        if (!obj) return OTSARDB_STORE_NOT_FOUND;
        /* The key encodes the generation; simulate GC by making everything
         * strictly below the covered generation unreadable. */
        char *end = NULL;
        unsigned long long generation =
            strtoull(key + sizeof(A5_ROOT "/commits/c") - 1u, &end, 10);
        if (end && generation < ctx->gc_min_generation) {
            return OTSARDB_STORE_NOT_FOUND;
        }
        data = obj->data;
        size = obj->size;
    }

    out->data = (unsigned char *)malloc(size ? size : 1u);
    if (!out->data) return OTSARDB_STORE_ERROR;
    if (size) memcpy(out->data, data, size);
    out->size = size;
    out->etag = (char *)malloc(2u);
    if (!out->etag) {
        free(out->data);
        memset(out, 0, sizeof(*out));
        return OTSARDB_STORE_ERROR;
    }
    out->etag[0] = 'v';
    out->etag[1] = '\0';
    return OTSARDB_STORE_OK;
}

static void chain_release(otsardb_object_store *store, otsardb_store_object *object) {
    (void)store;
    if (!object) return;
    free(object->data);
    free(object->etag);
    memset(object, 0, sizeof(*object));
}

static const otsardb_object_store_ops CHAIN_OPS = {
    .get = chain_get,
    .release_object = chain_release,
};

static void build_manifest_json(uint64_t generation,
                                const char *parent_commit_id,
                                const char *parent_manifest_sha256,
                                unsigned char **out_data,
                                size_t *out_size) {
    char json[1024];
    int n = snprintf(json, sizeof(json),
                     "{\"otsardb_format\":2,\"database_id\":\"" A5_DB_ID
                     "\",\"generation\":%llu,\"commit_id\":\"c%llu\","
                     "\"parent_generation\":%llu,\"parent_commit_id\":\"%s\","
                     "\"parent_manifest_sha256\":\"%s\",\"page_size\":4096,"
                     "\"database_size_pages\":100,"
                     "\"segment_key\":\"segments/sha256/aa/aa.otsseg\","
                     "\"segment_sha256\":\"" A5_SEG_SHA
                     "\",\"segment_size\":1000}\n",
                     (unsigned long long)generation,
                     (unsigned long long)generation,
                     (unsigned long long)(generation - 1u),
                     parent_commit_id,
                     parent_manifest_sha256);
    assert(n > 0 && (size_t)n < sizeof(json));
    *out_data = (unsigned char *)malloc((size_t)n);
    assert(*out_data);
    memcpy(*out_data, json, (size_t)n);
    *out_size = (size_t)n;
}

static otsardb_object_store *build_chain(uint64_t n_generations) {
    chain_store_ctx *ctx = (chain_store_ctx *)calloc(1, sizeof(*ctx));
    assert(ctx);
    ctx->n_generations = n_generations;
    ctx->gc_min_generation = 0;
    ctx->manifests = (chain_object *)calloc((size_t)n_generations + 1u,
                                            sizeof(*ctx->manifests));
    assert(ctx->manifests);

    char previous_sha[OTSARDB_SHA256_HEX_SIZE + 1] = "";
    for (uint64_t g = 1; g <= n_generations; ++g) {
        char parent_id[OTSARDB_COMMIT_ID_MAX + 1] = "";
        if (g > 1) {
            int n = snprintf(parent_id, sizeof(parent_id), "c%llu",
                             (unsigned long long)(g - 1u));
            assert(n > 0 && (size_t)n < sizeof(parent_id));
        }
        build_manifest_json(g, parent_id, previous_sha,
                            &ctx->manifests[g].data, &ctx->manifests[g].size);
        assert(otsardb_sha256_hex_data(ctx->manifests[g].data,
                                       ctx->manifests[g].size,
                                       previous_sha));
    }

    otsardb_head head;
    memset(&head, 0, sizeof(head));
    head.format_version = 1;
    head.generation = n_generations;
    snprintf(head.database_id, sizeof(head.database_id), A5_DB_ID);
    snprintf(head.commit_id, sizeof(head.commit_id), "c%llu",
             (unsigned long long)n_generations);
    int n = snprintf(head.commit_key, sizeof(head.commit_key),
                     A5_ROOT "/commits/c%llu.json",
                     (unsigned long long)n_generations);
    assert(n > 0 && (size_t)n < sizeof(head.commit_key));
    snprintf(head.commit_sha256, sizeof(head.commit_sha256), "%s", previous_sha);
    char *json = NULL;
    size_t json_size = 0;
    assert(otsardb_head_encode_json(&head, &json, &json_size));
    ctx->head_json = (unsigned char *)json;
    ctx->head_size = json_size;

    otsardb_object_store *store = (otsardb_object_store *)calloc(1, sizeof(*store));
    assert(store);
    store->ops = &CHAIN_OPS;
    store->context = ctx;
    return store;
}

/* Publish a valid checkpoint at the covered generation, exactly as the
 * checkpoint builder + pointer publisher would: the checkpoint manifest is
 * content-addressed (key == sha256 of its JSON) and the pointer is hash-bound
 * to it. gc keeps the covered manifest alive; the boundary this proves is the
 * one resolves from. */
static void add_checkpoint(otsardb_object_store *store,
                           uint64_t covered_generation) {
    chain_store_ctx *ctx = (chain_store_ctx *)store->context;

    char base_sha[OTSARDB_SHA256_HEX_SIZE + 1];
    assert(otsardb_sha256_hex_data(ctx->manifests[covered_generation].data,
                                   ctx->manifests[covered_generation].size,
                                   base_sha));

    otsardb_checkpoint_manifest cp;
    memset(&cp, 0, sizeof(cp));
    cp.format_version = OTSARDB_CHECKPOINT_MANIFEST_FORMAT_VERSION;
    cp.covered_generation = covered_generation;
    cp.page_size = 4096;
    cp.database_size_pages = 100;
    snprintf(cp.database_id, sizeof(cp.database_id), A5_DB_ID);
    snprintf(cp.base_commit_id, sizeof(cp.base_commit_id), "c%llu",
             (unsigned long long)covered_generation);
    snprintf(cp.base_manifest_sha256, sizeof(cp.base_manifest_sha256), "%s", base_sha);
    cp.page_group_count = 1;
    snprintf(cp.page_groups[0].key, sizeof(cp.page_groups[0].key),
             "checkpoint_pages/c%llu.otschk",
             (unsigned long long)covered_generation);
    memset(cp.page_groups[0].sha256, '0', OTSARDB_CHECKPOINT_SHA256_HEX_SIZE);
    cp.page_groups[0].sha256[OTSARDB_CHECKPOINT_SHA256_HEX_SIZE] = '\0';
    cp.page_groups[0].size = 32u + 12u + (uint64_t)cp.page_size; /* 1 record */

    char *cp_json = NULL;
    size_t cp_json_size = 0;
    char cp_sha[OTSARDB_CHECKPOINT_SHA256_HEX_SIZE + 1];
    assert(otsardb_checkpoint_manifest_encode(&cp, &cp_json, &cp_json_size, cp_sha));

    int n = snprintf(ctx->cp_manifest_key, sizeof(ctx->cp_manifest_key),
                     A5_ROOT "/checkpoints/%s.json", cp_sha);
    assert(n > 0 && (size_t)n < sizeof(ctx->cp_manifest_key));
    ctx->cp_manifest = (unsigned char *)cp_json;
    ctx->cp_manifest_size = cp_json_size;

    otsardb_checkpoint_pointer ptr;
    memset(&ptr, 0, sizeof(ptr));
    ptr.covered_generation = covered_generation;
    snprintf(ptr.checkpoint_sha256, sizeof(ptr.checkpoint_sha256), "%s", cp_sha);
    snprintf(ptr.checkpoint_key, sizeof(ptr.checkpoint_key), "%s", ctx->cp_manifest_key);
    snprintf(ptr.database_id, sizeof(ptr.database_id), A5_DB_ID);

    char *ptr_json = NULL;
    size_t ptr_json_size = 0;
    assert(otsardb_checkpoint_ptr_encode(&ptr, &ptr_json, &ptr_json_size));
    ctx->cp_pointer = (unsigned char *)ptr_json;
    ctx->cp_pointer_size = ptr_json_size;
}

static void set_gc_region(otsardb_object_store *store, uint64_t min_generation) {
    chain_store_ctx *ctx = (chain_store_ctx *)store->context;
    ctx->gc_min_generation = min_generation;
}

static void destroy_chain(otsardb_object_store *store) {
    chain_store_ctx *ctx = (chain_store_ctx *)store->context;
    for (uint64_t g = 1; g <= ctx->n_generations; ++g) {
        free(ctx->manifests[g].data);
    }
    free(ctx->manifests);
    free(ctx->head_json);
    free(ctx->cp_pointer);
    free(ctx->cp_manifest);
    free(ctx);
    free(store);
}

/* A5 FIX: a resolve whose parent_generation sits >= 2 generations below the
 * covered boundary now returns a DEFINITIVE SUPERSEDED (the checkpoint base
 * manifest proves the swept region is checkpoint-covered, so the commit is
 * definitively not on the authenticated live chain). Pre- this
 * was an error-class NOT_FOUND with state UNKNOWN. */
static void test_resolve_below_gc_region_definitive(void) {
    otsardb_object_store *store = build_chain(A5_CHAIN_GENERATIONS);
    set_gc_region(store, A5_COVERED_GENERATION);
    add_checkpoint(store, A5_COVERED_GENERATION);

    otsardb_commit_state state = OTSARDB_COMMIT_STATE_UNKNOWN;
    assert(otsardb_resolve_commit(store, A5_ROOT, "stale-retry-commit",
                                  A5_COVERED_GENERATION - 2u, &state) ==
           OTSARDB_JOURNAL_OK);
    assert(state == OTSARDB_COMMIT_STATE_SUPERSEDED);

    destroy_chain(store);
}

/* Control: without a checkpoint, the same NOT_FOUND below the swept region
 * stays FAIL-CLOSED (NOT_FOUND, state UNKNOWN) — a missing manifest with no
 * proven checkpoint boundary is genuine corruption and must never be masked
 * as SUPERSEDED. This is the discriminator behavior (test)
 * preserved. */
static void test_resolve_below_swept_region_no_checkpoint_fails_closed(void) {
    otsardb_object_store *store = build_chain(A5_CHAIN_GENERATIONS);
    set_gc_region(store, A5_COVERED_GENERATION);

    otsardb_commit_state state = OTSARDB_COMMIT_STATE_UNKNOWN;
    assert(otsardb_resolve_commit(store, A5_ROOT, "stale-retry-commit",
                                  A5_COVERED_GENERATION - 2u, &state) !=
           OTSARDB_JOURNAL_OK);
    assert(state == OTSARDB_COMMIT_STATE_UNKNOWN);

    destroy_chain(store);
}

/* Boundary: a request whose parent is exactly one below the covered
 * generation stops at the covered manifest (still live) and resolves
 * normally — the hazard needs the parent at least two below. */
static void test_resolve_at_gc_boundary_ok(void) {
    otsardb_object_store *store = build_chain(A5_CHAIN_GENERATIONS);
    set_gc_region(store, A5_COVERED_GENERATION);
    add_checkpoint(store, A5_COVERED_GENERATION);

    otsardb_commit_state state = OTSARDB_COMMIT_STATE_UNKNOWN;
    assert(otsardb_resolve_commit(store, A5_ROOT, "stale-retry-commit",
                                  A5_COVERED_GENERATION - 1u, &state) ==
           OTSARDB_JOURNAL_OK);
    assert(state == OTSARDB_COMMIT_STATE_SUPERSEDED);

    destroy_chain(store);
}

/* Control: a resolve with parent_generation INSIDE the live region resolves
 * normally (SUPERSEDED for an unknown commit). */
static void test_resolve_above_gc_region_ok(void) {
    otsardb_object_store *store = build_chain(A5_CHAIN_GENERATIONS);
    set_gc_region(store, A5_COVERED_GENERATION);
    add_checkpoint(store, A5_COVERED_GENERATION);

    otsardb_commit_state state = OTSARDB_COMMIT_STATE_UNKNOWN;
    assert(otsardb_resolve_commit(store, A5_ROOT, "stale-retry-commit",
                                  A5_COVERED_GENERATION + 5u, &state) ==
           OTSARDB_JOURNAL_OK);
    assert(state == OTSARDB_COMMIT_STATE_SUPERSEDED);

    destroy_chain(store);
}

/* Control: a real committed commit inside the live region still resolves to
 * COMMITTED after GC (and with a checkpoint present). */
static void test_resolve_committed_above_gc_region(void) {
    otsardb_object_store *store = build_chain(A5_CHAIN_GENERATIONS);
    set_gc_region(store, A5_COVERED_GENERATION);
    add_checkpoint(store, A5_COVERED_GENERATION);

    otsardb_commit_state state = OTSARDB_COMMIT_STATE_UNKNOWN;
    assert(otsardb_resolve_commit(store, A5_ROOT, "c92",
                                  A5_COVERED_GENERATION + 1u, &state) ==
           OTSARDB_JOURNAL_OK);
    assert(state == OTSARDB_COMMIT_STATE_COMMITTED);

    destroy_chain(store);
}

int main(void) {
    test_resolve_below_gc_region_definitive();
    test_resolve_below_swept_region_no_checkpoint_fails_closed();
    test_resolve_at_gc_boundary_ok();
    test_resolve_above_gc_region_ok();
    test_resolve_committed_above_gc_region();

    puts("A5 fix test passed: below-GC-resolve is definitive SUPERSEDED via "
         "the checkpoint base manifest; without a checkpoint the fail-closed "
         "NOT_FOUND behavior is preserved ");
    return 0;
}
