/* conflict_chain_cap_test.c — regression tests for , audit
 * finding F05: the commit-conflict chain walks in commit_publish.c
 * (otsardb_resolve_commit and otsardb_publish_commit_detect_conflict) are
 * bounded by OTSARDB_RECOVERY_MAX_COMMITS and fail closed with
 * OTSARDB_JOURNAL_INVALID when a chain is at least as deep as the cap.
 *
 * Deterministic, no network, no sleeps: a purpose-built chain store holds a
 * valid hash-linked manifest chain of up to OTSARDB_RECOVERY_MAX_COMMITS + 1
 * generations in memory; each GET is an O(1) array lookup keyed by the
 * generation parsed out of the manifest key, so the deep walks run in
 * bounded time and memory instead of a quadratic scan.
 *
 * Discrimination: before the walks had no bound — a chain
 * deeper than the cap walked to the end and returned OK/SUPERSEDED. The
 * deep-chain assertions below fail on that behavior.
 *
 * Wiring (coordinator — , NOT applied here; CMakeLists.txt is
 * out of scope for this change set):
 * add_executable(otsardb_conflict_chain_cap_test
 * tests/conflict_chain_cap_test.c)
 * target_include_directories(otsardb_conflict_chain_cap_test
 * PRIVATE include src/core src/crypto src/storage tests)
 * target_link_libraries(otsardb_conflict_chain_cap_test PRIVATE otsardb_core)
 * add_test(NAME otsardb_conflict_chain_cap
 * COMMAND otsardb_conflict_chain_cap_test)
 * set_tests_properties(otsardb_conflict_chain_cap PROPERTIES TIMEOUT 600)
 */
#include "commit_manifest.h"
#include "commit_publish.h"
#include "journal.h"
#include "object_store.h"
#include "recovery.h"
#include "sha256.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define F05_ROOT "f05root"
#define F05_DB_ID "f05-db"
#define F05_SEG_SHA                                                              \
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"

/* ------------------------------------------------------------------ store */

typedef struct chain_object {
    unsigned char *data;
    size_t size;
} chain_object;

/* Manifest chain indexed by generation (manifests[0] unused). */
typedef struct chain_store_ctx {
    chain_object *manifests;
    uint64_t n_generations;
    unsigned char *head_json;
    size_t head_size;
} chain_store_ctx;

/* O(1) lookup: "f05root/commits/c<generation>.json". */
static chain_object *chain_find(chain_store_ctx *ctx, const char *key) {
    static const char prefix[] = F05_ROOT "/commits/c";
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
    if (strcmp(key, F05_ROOT "/HEAD.json") == 0) {
        data = ctx->head_json;
        size = ctx->head_size;
    } else {
        chain_object *obj = chain_find(ctx, key);
        if (!obj) return OTSARDB_STORE_NOT_FOUND;
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

/* ------------------------------------------------------------------ chain */

/* v2 manifest: flat single-segment layout, smallest valid JSON. The walk
 * only reads generation/commit-id continuity + parent_manifest_sha256, so
 * the segment fields are constants. */
static void build_manifest_json(uint64_t generation,
                                const char *parent_commit_id,
                                const char *parent_manifest_sha256,
                                unsigned char **out_data,
                                size_t *out_size) {
    char json[1024];
    int n = snprintf(json, sizeof(json),
                     "{\"otsardb_format\":2,\"database_id\":\"" F05_DB_ID
                     "\",\"generation\":%llu,\"commit_id\":\"c%llu\","
                     "\"parent_generation\":%llu,\"parent_commit_id\":\"%s\","
                     "\"parent_manifest_sha256\":\"%s\",\"page_size\":4096,"
                     "\"database_size_pages\":100,"
                     "\"segment_key\":\"segments/sha256/aa/aa.otsseg\","
                     "\"segment_sha256\":\"" F05_SEG_SHA
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

/* Build a hash-linked chain of `n_generations` manifests + HEAD. Each
 * manifest g embeds the exact SHA-256 of manifest g-1 (the chain link the
 * walk verifies). */
static otsardb_object_store *build_chain(uint64_t n_generations) {
    chain_store_ctx *ctx = (chain_store_ctx *)calloc(1, sizeof(*ctx));
    assert(ctx);
    ctx->n_generations = n_generations;
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
    snprintf(head.database_id, sizeof(head.database_id), F05_DB_ID);
    snprintf(head.commit_id, sizeof(head.commit_id), "c%llu",
             (unsigned long long)n_generations);
    int n = snprintf(head.commit_key, sizeof(head.commit_key),
                     F05_ROOT "/commits/c%llu.json",
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

/* ------------------------------------------------------------------ tests */

/* A chain deeper than the cap must be refused deterministically by BOTH
 * walks, with JOURNAL_INVALID and the state untouched (fail closed). */
static void test_deep_chain_refused(void) {
    const uint64_t n = (uint64_t)OTSARDB_RECOVERY_MAX_COMMITS + 1u;
    otsardb_object_store *store = build_chain(n);

    otsardb_commit_state state = OTSARDB_COMMIT_STATE_UNKNOWN;
    assert(otsardb_resolve_commit(store, F05_ROOT, "probe-commit", 0, &state) ==
           OTSARDB_JOURNAL_INVALID);
    assert(state == OTSARDB_COMMIT_STATE_UNKNOWN);

    otsardb_commit_request req;
    memset(&req, 0, sizeof(req));
    req.database_root = F05_ROOT;
    req.database_id = F05_DB_ID;
    req.commit_id = "probe-commit";
    req.parent_commit_id = "c1";
    req.parent_manifest_sha256 = F05_SEG_SHA;
    req.parent_generation = 1;
    req.page_size = 4096;
    req.database_size_pages = 100;
    req.segment_payload = (const void *)F05_SEG_SHA;
    req.segment_payload_size = 64;
    otsardb_commit_state conflict_state = OTSARDB_COMMIT_STATE_UNKNOWN;
    int overlap = 0;
    assert(otsardb_publish_commit_detect_conflict(store, F05_ROOT, &req,
                                                  &conflict_state, &overlap) ==
           OTSARDB_JOURNAL_INVALID);
    assert(conflict_state == OTSARDB_COMMIT_STATE_UNKNOWN);

    destroy_chain(store);
}

/* A chain exactly at the cap is refused too (the same >= semantics recovery
 * applies to its key_count guard), but a walk that stays BELOW the cap —
 * same chain, request parent at generation 1 — still resolves normally. */
static void test_chain_at_cap_boundary(void) {
    const uint64_t n = (uint64_t)OTSARDB_RECOVERY_MAX_COMMITS;
    otsardb_object_store *store = build_chain(n);

    otsardb_commit_state state = OTSARDB_COMMIT_STATE_UNKNOWN;
    assert(otsardb_resolve_commit(store, F05_ROOT, "probe-commit", 0, &state) ==
           OTSARDB_JOURNAL_INVALID);

    /* The walk from generation n down to generation 1 takes n-1 steps,
     * strictly below the cap: it must terminate with the normal result. */
    state = OTSARDB_COMMIT_STATE_UNKNOWN;
    assert(otsardb_resolve_commit(store, F05_ROOT, "probe-commit", 1, &state) ==
           OTSARDB_JOURNAL_OK);
    assert(state == OTSARDB_COMMIT_STATE_SUPERSEDED);

    destroy_chain(store);
}

/* Shallow chains are untouched by the cap: both walks behave exactly as
 * before the fix. */
static void test_shallow_chain_normal(void) {
    otsardb_object_store *store = build_chain(3);

    otsardb_commit_state state = OTSARDB_COMMIT_STATE_UNKNOWN;
    assert(otsardb_resolve_commit(store, F05_ROOT, "probe-commit", 0, &state) ==
           OTSARDB_JOURNAL_OK);
    assert(state == OTSARDB_COMMIT_STATE_SUPERSEDED);

    otsardb_commit_request req;
    memset(&req, 0, sizeof(req));
    req.database_root = F05_ROOT;
    req.database_id = F05_DB_ID;
    req.commit_id = "probe-commit";
    req.parent_commit_id = "c1";
    req.parent_manifest_sha256 = F05_SEG_SHA;
    req.parent_generation = 1;
    req.page_size = 4096;
    req.database_size_pages = 100;
    req.segment_payload = (const void *)F05_SEG_SHA;
    req.segment_payload_size = 64;
    otsardb_commit_state conflict_state = OTSARDB_COMMIT_STATE_UNKNOWN;
    int overlap = 0;
    assert(otsardb_publish_commit_detect_conflict(store, F05_ROOT, &req,
                                                  &conflict_state, &overlap) ==
           OTSARDB_JOURNAL_OK);
    assert(conflict_state == OTSARDB_COMMIT_STATE_SUPERSEDED);
    /* Legacy payload carries no page set: conservative overlap. */
    assert(overlap == 1);

    destroy_chain(store);
}

int main(void) {
    test_shallow_chain_normal();
    test_chain_at_cap_boundary();
    test_deep_chain_refused();
    puts("conflict chain cap regression tests passed (F05)");
    return 0;
}
