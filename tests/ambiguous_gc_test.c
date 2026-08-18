/* ambiguous_gc_test.c — evidence test for the failure-mode finding
 * failure-mode audit finding A5 (ambiguous commit whose parent region was
 * GC'd).
 *
 * A5 (MEDIUM, PROJECTED, needs-test): otsardb_resolve_commit walks HEAD
 * down to `parent_generation` and needs every intermediate manifest. GC
 * deletes everything strictly below the checkpoint's covered_generation
 * (gc.c), keeping exactly the covered manifest. A resolve whose
 * parent_generation is below the covered generation (a long-lived retry or
 * a dual-publish retry whose parent predates the last checkpoint) walks
 * into the swept region, hits a deleted manifest and returns NOT_FOUND —
 * the commit is durable and unacknowledgeable (the sync fails, the 0072
 * path marks ABORTED, a client re-execute risks double-apply). Not silent
 * data loss, but it widens the 0021 ambiguity window.
 *
 * This test deterministically CONFIRMS the hazard on the current code: the
 * walk below the GC region fails with OTSARDB_JOURNAL_NOT_FOUND while the
 * same walk in the live region resolves normally (controls). The FIX (per
 * the audit: resolve from the checkpoint base manifest, or refuse GC below
 * the oldest unresolved parent) lives in commit_publish.c / gc.c — OUT OF
 * SCOPE for this change set (owner: the 0145 manifest agent / coordinator);
 * the evidence and the precise handoff are recorded in the change record
 *
 * Deterministic, no network, no sleeps: a purpose-built chain store holds a
 * valid hash-linked manifest chain in memory and returns NOT_FOUND for the
 * generations GC would have swept.
 *
 * Wiring (coordinator): NOT applied here; CMakeLists.txt is
 * out of scope for this change set):
 * add_executable(otsardb_ambiguous_gc_test
 * tests/ambiguous_gc_test.c)
 * target_include_directories(otsardb_ambiguous_gc_test
 * PRIVATE include src/core src/crypto src/storage tests)
 * target_link_libraries(otsardb_ambiguous_gc_test
 * PRIVATE otsardb_core)
 * add_test(NAME otsardb_ambiguous_gc
 * COMMAND otsardb_ambiguous_gc_test)
 * set_tests_properties(otsardb_ambiguous_gc
 * PROPERTIES TIMEOUT 600)
 */
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

#define A5_ROOT "146a5"
#define A5_DB_ID "0146-a5-db"
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
    free(ctx);
    free(store);
}

/* A5 hazard: resolving a commit whose parent_generation is BELOW the GC'd
 * region must fail (the walk descends past the covered boundary and hits a
 * swept manifest). Fail-closed, never a COMMITTED/SUPERSEDED verdict on
 * missing evidence. (The walk reads the manifest AT each generation; it
 * stops when the manifest's parent_generation == the requested one, so the
 * hazard triggers when the requested parent is at least two generations
 * below the covered boundary.) */
static void test_resolve_below_gc_region_fails(void) {
    otsardb_object_store *store = build_chain(A5_CHAIN_GENERATIONS);
    set_gc_region(store, A5_COVERED_GENERATION);

    otsardb_commit_state state = OTSARDB_COMMIT_STATE_UNKNOWN;
    otsardb_journal_result rc =
        otsardb_resolve_commit(store, A5_ROOT, "stale-retry-commit",
                               A5_COVERED_GENERATION - 2u, &state);
    /* The walk descends below the covered generation and hits a swept
     * manifest: NOT_FOUND is an error-class result, never OK. */
    assert(rc != OTSARDB_JOURNAL_OK);
    assert(state == OTSARDB_COMMIT_STATE_UNKNOWN);

    destroy_chain(store);
}

/* Boundary: a request whose parent is exactly one below the covered
 * generation stops at the covered manifest (still live) and resolves
 * normally — the hazard needs the parent at least two below. */
static void test_resolve_at_gc_boundary_ok(void) {
    otsardb_object_store *store = build_chain(A5_CHAIN_GENERATIONS);
    set_gc_region(store, A5_COVERED_GENERATION);

    otsardb_commit_state state = OTSARDB_COMMIT_STATE_UNKNOWN;
    assert(otsardb_resolve_commit(store, A5_ROOT, "stale-retry-commit",
                                  A5_COVERED_GENERATION - 1u, &state) ==
           OTSARDB_JOURNAL_OK);
    assert(state == OTSARDB_COMMIT_STATE_SUPERSEDED);

    destroy_chain(store);
}

/* Control: the same chain, resolving with parent_generation INSIDE the live
 * region resolves normally (SUPERSEDED for an unknown commit). */
static void test_resolve_above_gc_region_ok(void) {
    otsardb_object_store *store = build_chain(A5_CHAIN_GENERATIONS);
    set_gc_region(store, A5_COVERED_GENERATION);

    otsardb_commit_state state = OTSARDB_COMMIT_STATE_UNKNOWN;
    assert(otsardb_resolve_commit(store, A5_ROOT, "stale-retry-commit",
                                  A5_COVERED_GENERATION + 5u, &state) ==
           OTSARDB_JOURNAL_OK);
    assert(state == OTSARDB_COMMIT_STATE_SUPERSEDED);

    destroy_chain(store);
}

/* Control: a real committed commit inside the live region still resolves to
 * COMMITTED after GC. */
static void test_resolve_committed_above_gc_region(void) {
    otsardb_object_store *store = build_chain(A5_CHAIN_GENERATIONS);
    set_gc_region(store, A5_COVERED_GENERATION);

    otsardb_commit_state state = OTSARDB_COMMIT_STATE_UNKNOWN;
    assert(otsardb_resolve_commit(store, A5_ROOT, "c92",
                                  A5_COVERED_GENERATION + 1u, &state) ==
           OTSARDB_JOURNAL_OK);
    assert(state == OTSARDB_COMMIT_STATE_COMMITTED);

    destroy_chain(store);
}

int main(void) {
    test_resolve_below_gc_region_fails();
    test_resolve_at_gc_boundary_ok();
    test_resolve_above_gc_region_ok();
    test_resolve_committed_above_gc_region();

    puts("A5 evidence test passed: resolve below the GC region fails closed "
         "(NOT_FOUND), live region resolves normally; fix delegated "
         "(commit_publish.c/gc.c handoff,)");
    return 0;
}
