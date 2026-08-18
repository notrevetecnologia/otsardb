#include "commit_publish.h"
#include "fault_store.h"
#include "journal.h"
#include "memory_store.h"
#include "object_store.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Commit publication order: PUT segment 1..N, PUT manifest, PUT HEAD.
 * With three segments the immutable objects before HEAD are PUTs 1..4. */

static const unsigned char PAYLOAD_A[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
static const unsigned char PAYLOAD_B[] = {0x11, 0x12, 0x13, 0x14};
static const unsigned char PAYLOAD_C[] = {0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a};

static const uint32_t PAGES_A[] = {1, 2, 3};
static const uint32_t PAGES_B[] = {4};
static const uint32_t PAGES_C[] = {5, 6};

static const otsardb_segment_input THREE_SEGMENTS[3] = {
    {PAYLOAD_A, sizeof(PAYLOAD_A), PAGES_A, 3},
    {PAYLOAD_B, sizeof(PAYLOAD_B), PAGES_B, 1},
    {PAYLOAD_C, sizeof(PAYLOAD_C), PAGES_C, 2},
};

static void init_request(otsardb_commit_request *req,
                         const char *root,
                         const char *database_id,
                         const char *commit_id,
                         uint64_t parent_generation,
                         const char *parent_commit_id,
                         const char *parent_manifest_sha256,
                         const char *expected_head_etag,
                         int create_head_if_missing) {
    memset(req, 0, sizeof(*req));
    req->database_root = root;
    req->database_id = database_id;
    req->commit_id = commit_id;
    req->parent_commit_id = parent_commit_id;
    req->parent_manifest_sha256 = parent_manifest_sha256;
    req->parent_generation = parent_generation;
    req->page_size = 4096;
    /* (audit F07): THREE_SEGMENTS' page sets span pages 1..6,
     * so the declared database size must be >= 6 — page numbers beyond
     * database_size_pages are now rejected at decode. */
    req->database_size_pages = 6;
    req->segments = THREE_SEGMENTS;
    req->segment_count = 3;
    req->expected_head_etag = expected_head_etag;
    req->create_head_if_missing = create_head_if_missing;
}

static void publish_parent(otsardb_object_store *fault,
                           const char *root,
                           const char *database_id,
                           const char *commit_id,
                           otsardb_commit_result *out) {
    otsardb_commit_request req;
    init_request(&req, root, database_id, commit_id, 0, NULL, NULL, NULL, 1);
    assert(otsardb_publish_commit(fault, &req, out) == OTSARDB_JOURNAL_OK);
}

static void expect_no_head(otsardb_object_store *store, const char *root) {
    otsardb_head head;
    char *etag = NULL;
    assert(otsardb_journal_read_head(store, root, &head, &etag) == OTSARDB_JOURNAL_NOT_FOUND);
    free(etag);
}

static void expect_head(otsardb_object_store *store,
                        const char *root,
                        uint64_t generation,
                        const char *commit_id) {
    otsardb_head head;
    char *etag = NULL;
    assert(otsardb_journal_read_head(store, root, &head, &etag) == OTSARDB_JOURNAL_OK);
    assert(head.generation == generation);
    assert(strcmp(head.commit_id, commit_id) == 0);
    free(etag);
}

static void manifest_key_for(const char *root,
                             const char *commit_id,
                             char *out,
                             size_t out_size) {
    int n = snprintf(out, out_size, "%s/commits/%s.json", root, commit_id);
    assert(n > 0 && (size_t)n < out_size);
}

/* Failing any of the 3 segment PUTs or the manifest PUT of a multi-segment
 * genesis commit must abort publication with no HEAD appearing. */
static void test_genesis_segment_faults(void) {
    for (size_t fail_at = 1; fail_at <= 4; ++fail_at) {
        otsardb_object_store *inner = otsardb_test_memory_store_create();
        assert(inner != NULL);
        otsardb_object_store *fault = otsardb_test_fault_store_create(inner);
        assert(fault != NULL);
        otsardb_test_fault_store_fail_put(fault, fail_at, OTSARDB_STORE_ERROR, 0);

        otsardb_commit_request req;
        init_request(&req, "fault/genesis", "fault-genesis", "genesis-fail",
                     0, NULL, NULL, NULL, 1);
        otsardb_commit_result result;
        assert(otsardb_publish_commit(fault, &req, &result) != OTSARDB_JOURNAL_OK);
        expect_no_head(inner, "fault/genesis");

        otsardb_test_fault_store_destroy(fault);
        otsardb_test_memory_store_destroy(inner);
    }
}

/* Failing any of the 3 segment PUTs or the manifest PUT of a multi-segment
 * CHILD commit (parent published, expected_head_etag set) must return an
 * error and leave HEAD pointing at the parent. The failed commit's manifest
 * must not exist. */
static void test_child_segment_faults(void) {
    for (size_t fail_at = 1; fail_at <= 4; ++fail_at) {
        otsardb_object_store *inner = otsardb_test_memory_store_create();
        assert(inner != NULL);
        otsardb_object_store *fault = otsardb_test_fault_store_create(inner);
        assert(fault != NULL);

        otsardb_commit_result parent;
        publish_parent(fault, "fault/child", "fault-child", "parent-c1", &parent);

        otsardb_test_fault_store_fail_put(fault, fail_at, OTSARDB_STORE_ERROR, 0);

        otsardb_commit_request child;
        init_request(&child, "fault/child", "fault-child", "child-fail",
                     1, "parent-c1", parent.head.commit_sha256, parent.head_etag, 0);
        otsardb_commit_result result;
        assert(otsardb_publish_commit(fault, &child, &result) != OTSARDB_JOURNAL_OK);
        expect_head(inner, "fault/child", parent.head.generation, "parent-c1");

        char key[OTSARDB_KEY_MAX + 1];
        manifest_key_for("fault/child", "child-fail", key, sizeof(key));
        otsardb_store_object object = {0};
        assert(otsardb_store_get(inner, key, &object) == OTSARDB_STORE_NOT_FOUND);

        otsardb_test_fault_store_destroy(fault);
        otsardb_test_memory_store_destroy(inner);
    }
}

/* Segment PUTs succeed but the manifest PUT fails: HEAD stays on the parent
 * and the manifest object does not exist; the parent's own objects remain. */
static void test_manifest_fault(void) {
    otsardb_object_store *inner = otsardb_test_memory_store_create();
    assert(inner != NULL);
    otsardb_object_store *fault = otsardb_test_fault_store_create(inner);
    assert(fault != NULL);

    otsardb_commit_result parent;
    publish_parent(fault, "fault/manifest", "fault-manifest", "parent-m1", &parent);

    otsardb_test_fault_store_fail_put(fault, 4, OTSARDB_STORE_ERROR, 0);

    otsardb_commit_request child;
    init_request(&child, "fault/manifest", "fault-manifest", "child-m2",
                 1, "parent-m1", parent.head.commit_sha256, parent.head_etag, 0);
    otsardb_commit_result result;
    assert(otsardb_publish_commit(fault, &child, &result) == OTSARDB_JOURNAL_ERROR);
    expect_head(inner, "fault/manifest", parent.head.generation, "parent-m1");

    char key[OTSARDB_KEY_MAX + 1];
    manifest_key_for("fault/manifest", "child-m2", key, sizeof(key));
    otsardb_store_object object = {0};
    assert(otsardb_store_get(inner, key, &object) == OTSARDB_STORE_NOT_FOUND);

    assert(otsardb_store_get(inner, parent.head.commit_key, &object) == OTSARDB_STORE_OK);
    otsardb_store_release_object(inner, &object);

    otsardb_test_fault_store_destroy(fault);
    otsardb_test_memory_store_destroy(inner);
}

/* HEAD PUT times out after the delegate succeeded on a multi-segment CHILD:
 * publication is AMBIGUOUS but the commit is actually durable, so resolve
 * reports COMMITTED and HEAD points at the child. */
static void test_head_ambiguous_timeout(void) {
    otsardb_object_store *inner = otsardb_test_memory_store_create();
    assert(inner != NULL);
    otsardb_object_store *fault = otsardb_test_fault_store_create(inner);
    assert(fault != NULL);

    otsardb_commit_result parent;
    publish_parent(fault, "fault/timeout", "fault-timeout", "parent-t1", &parent);

    otsardb_test_fault_store_fail_put(fault, 5, OTSARDB_STORE_TIMEOUT, 1);

    otsardb_commit_request child;
    init_request(&child, "fault/timeout", "fault-timeout", "child-t2",
                 1, "parent-t1", parent.head.commit_sha256, parent.head_etag, 0);
    otsardb_commit_result result;
    assert(otsardb_publish_commit(fault, &child, &result) == OTSARDB_JOURNAL_AMBIGUOUS);
    expect_head(inner, "fault/timeout", parent.head.generation + 1, "child-t2");

    otsardb_commit_state state = OTSARDB_COMMIT_STATE_UNKNOWN;
    assert(otsardb_resolve_commit(inner, "fault/timeout", "child-t2", 1, &state) ==
           OTSARDB_JOURNAL_OK);
    assert(state == OTSARDB_COMMIT_STATE_COMMITTED);

    otsardb_test_fault_store_destroy(fault);
    otsardb_test_memory_store_destroy(inner);
}

/* A segment PUT that times out AFTER the delegate succeeded is resolved by
 * immutable_put reading the object back: it is durable, so publication
 * completes, every object is present, and resolve reports COMMITTED. */
static void test_segment_timeout_durable(void) {
    otsardb_object_store *inner = otsardb_test_memory_store_create();
    assert(inner != NULL);
    otsardb_object_store *fault = otsardb_test_fault_store_create(inner);
    assert(fault != NULL);

    otsardb_test_fault_store_fail_put(fault, 1, OTSARDB_STORE_TIMEOUT, 1);

    otsardb_commit_request req;
    init_request(&req, "fault/segtimeout", "fault-segtimeout", "seg-t1",
                 0, NULL, NULL, NULL, 1);
    otsardb_commit_result result;
    assert(otsardb_publish_commit(fault, &req, &result) == OTSARDB_JOURNAL_OK);
    assert(result.head.generation == 1);

    char key[OTSARDB_KEY_MAX + 1];
    int n = snprintf(key, sizeof(key), "%s/%s", "fault/segtimeout",
                     result.manifest.segments[0].key);
    assert(n > 0 && (size_t)n < sizeof(key));
    otsardb_store_object object = {0};
    assert(otsardb_store_get(inner, key, &object) == OTSARDB_STORE_OK);
    otsardb_store_release_object(inner, &object);

    assert(otsardb_store_get(inner, result.head.commit_key, &object) == OTSARDB_STORE_OK);
    otsardb_store_release_object(inner, &object);

    otsardb_commit_state state = OTSARDB_COMMIT_STATE_UNKNOWN;
    assert(otsardb_resolve_commit(inner, "fault/segtimeout", "seg-t1", 0, &state) ==
           OTSARDB_JOURNAL_OK);
    assert(state == OTSARDB_COMMIT_STATE_COMMITTED);

    otsardb_test_fault_store_destroy(fault);
    otsardb_test_memory_store_destroy(inner);
}

/* A segment PUT that times out BEFORE the delegate ran is genuinely
 * ambiguous: the object is NOT durable, publication returns AMBIGUOUS, no
 * HEAD appears, and resolve reports NOT_COMMITTED. */
static void test_segment_timeout_ambiguous(void) {
    otsardb_object_store *inner = otsardb_test_memory_store_create();
    assert(inner != NULL);
    otsardb_object_store *fault = otsardb_test_fault_store_create(inner);
    assert(fault != NULL);

    otsardb_test_fault_store_fail_put(fault, 2, OTSARDB_STORE_TIMEOUT, 0);

    otsardb_commit_request req;
    init_request(&req, "fault/segambig", "fault-segambig", "seg-a1",
                 0, NULL, NULL, NULL, 1);
    otsardb_commit_result result;
    assert(otsardb_publish_commit(fault, &req, &result) == OTSARDB_JOURNAL_AMBIGUOUS);
    expect_no_head(inner, "fault/segambig");

    otsardb_commit_state state = OTSARDB_COMMIT_STATE_UNKNOWN;
    assert(otsardb_resolve_commit(inner, "fault/segambig", "seg-a1", 0, &state) ==
           OTSARDB_JOURNAL_OK);
    assert(state == OTSARDB_COMMIT_STATE_NOT_COMMITTED);

    otsardb_test_fault_store_destroy(fault);
    otsardb_test_memory_store_destroy(inner);
}

int main(void) {
    test_genesis_segment_faults();
    test_child_segment_faults();
    test_manifest_fault();
    test_head_ambiguous_timeout();
    test_segment_timeout_durable();
    test_segment_timeout_ambiguous();
    puts("commit fault-injection tests passed");
    return 0;
}
