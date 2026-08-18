#include "capability_probe.h"
#include "commit_manifest.h"
#include "commit_publish.h"
#include "db_metadata.h"
#include "fault_store.h"
#include "journal.h"
#include "memory_store.h"
#include "segment.h"
#include "sha256.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void verify_object_hash(otsardb_object_store *store,
                               const char *key,
                               const char *expected_hash) {
    otsardb_store_object object = {0};
    assert(otsardb_store_get(store, key, &object) == OTSARDB_STORE_OK);
    char hash[65];
    assert(otsardb_sha256_hex_data(object.data, object.size, hash));
    assert(strcmp(hash, expected_hash) == 0);
    otsardb_store_release_object(store, &object);
}

/* Manifest v4 stores segment keys relative to the database root. */
static void verify_segment_object(otsardb_object_store *store,
                                  const char *database_root,
                                  const otsardb_manifest_segment_ref *ref) {
    char absolute_key[OTSARDB_KEY_MAX + 1];
    int n = snprintf(absolute_key,
                     sizeof(absolute_key),
                     "%s/%s",
                     database_root,
                     ref->key);
    assert(n > 0 && (size_t)n < sizeof(absolute_key));
    verify_object_hash(store, absolute_key, ref->sha256);

    otsardb_store_object object = {0};
    assert(otsardb_store_get(store, absolute_key, &object) == OTSARDB_STORE_OK);
    assert(object.size == ref->size);
    otsardb_store_release_object(store, &object);
}

int main(void) {
    otsardb_object_store *store = otsardb_test_memory_store_create();
    assert(store != NULL);

    otsardb_db_metadata metadata = {0};
    metadata.format_version = 1;
    metadata.page_size = 4096;
    snprintf(metadata.database_id, sizeof(metadata.database_id), "db-orders");
    assert(otsardb_db_metadata_create(store, "tenant/orders", &metadata) == OTSARDB_METADATA_OK);
    assert(otsardb_db_metadata_create(store, "tenant/orders", &metadata) == OTSARDB_METADATA_CONFLICT);

    otsardb_db_metadata loaded = {0};
    assert(otsardb_db_metadata_read(store, "tenant/orders", &loaded) == OTSARDB_METADATA_OK);
    assert(strcmp(loaded.database_id, "db-orders") == 0);
    assert(loaded.page_size == 4096);

    const unsigned char payload1[] = {0x01, 0x02, 0x03, 0x04, 0x05};
    otsardb_commit_request first = {0};
    first.database_root = "tenant/orders";
    first.database_id = "db-orders";
    first.commit_id = "c1";
    first.parent_generation = 0;
    first.page_size = 4096;
    first.database_size_pages = 2;
    first.segment_payload = payload1;
    first.segment_payload_size = sizeof(payload1);
    first.create_head_if_missing = 1;

    otsardb_commit_result r1;
    assert(otsardb_publish_commit(store, &first, &r1) == OTSARDB_JOURNAL_OK);
    assert(r1.head.generation == 1);
    assert(strcmp(r1.head.commit_id, "c1") == 0);
    assert(r1.head_etag[0] != '\0');
    assert(r1.manifest.format_version == OTSARDB_COMMIT_MANIFEST_VERSION);
    assert(r1.manifest.parent_generation == 0);
    assert(r1.manifest.parent_commit_id[0] == '\0');
    assert(r1.manifest.parent_manifest_sha256[0] == '\0');
    verify_object_hash(store, r1.head.commit_key, r1.head.commit_sha256);
    assert(r1.manifest.segment_count == 1);
    verify_segment_object(store, "tenant/orders", &r1.manifest.segments[0]);

    otsardb_store_object segment_object = {0};
    char first_segment_key[OTSARDB_KEY_MAX + 1];
    int key_len = snprintf(first_segment_key,
                           sizeof(first_segment_key),
                           "%s/%s",
                           "tenant/orders",
                           r1.manifest.segments[0].key);
    assert(key_len > 0 && (size_t)key_len < sizeof(first_segment_key));
    assert(otsardb_store_get(store, first_segment_key, &segment_object) == OTSARDB_STORE_OK);
    otsardb_segment_view view;
    assert(otsardb_segment_decode(segment_object.data, segment_object.size, &view));
    assert(strcmp(view.commit_id, "c1") == 0);
    assert(view.payload_size == sizeof(payload1));
    if (view.compressed) {
        unsigned char *raw_payload = NULL;
        size_t raw_payload_size = 0;
        assert(otsardb_segment_decompress(&view, (void **)&raw_payload, &raw_payload_size));
        assert(raw_payload_size == sizeof(payload1));
        assert(memcmp(raw_payload, payload1, sizeof(payload1)) == 0);
        free(raw_payload);
    } else {
        assert(memcmp(view.payload, payload1, sizeof(payload1)) == 0);
    }
    otsardb_store_release_object(store, &segment_object);

    otsardb_store_object manifest_object = {0};
    assert(otsardb_store_get(store, r1.head.commit_key, &manifest_object) == OTSARDB_STORE_OK);
    otsardb_commit_manifest decoded_manifest;
    assert(otsardb_commit_manifest_decode_json((const char *)manifest_object.data,
                                             manifest_object.size,
                                             &decoded_manifest));
    assert(decoded_manifest.format_version == OTSARDB_COMMIT_MANIFEST_VERSION);
    assert(decoded_manifest.generation == 1);
    assert(decoded_manifest.parent_generation == 0);
    assert(strcmp(decoded_manifest.commit_id, "c1") == 0);
    assert(decoded_manifest.parent_manifest_sha256[0] == '\0');
    /* Legacy single-segment path carries no page set: the v4 manifest records
     * "pages":[] which decodes to an empty (unknown) page set. */
    assert(decoded_manifest.segments[0].page_count == 0);
    assert(decoded_manifest.segments[0].pages == NULL);
    assert(decoded_manifest.segments[0].frame_offsets == NULL);
    otsardb_commit_manifest_free(&decoded_manifest);
    otsardb_store_release_object(store, &manifest_object);

    const unsigned char payload2a[] = {0xaa, 0xbb};
    const unsigned char payload2b[] = {0xcc, 0xdd};

    otsardb_commit_request a = {0};
    a.database_root = "tenant/orders";
    a.database_id = "db-orders";
    a.commit_id = "c2-a";
    a.parent_commit_id = "c1";
    a.parent_manifest_sha256 = r1.head.commit_sha256;
    a.parent_generation = 1;
    a.page_size = 4096;
    a.database_size_pages = 3;
    a.segment_payload = payload2a;
    a.segment_payload_size = sizeof(payload2a);
    a.expected_head_etag = r1.head_etag;

    otsardb_commit_request missing_parent_hash = a;
    missing_parent_hash.commit_id = "c2-missing-hash";
    missing_parent_hash.parent_manifest_sha256 = NULL;
    otsardb_commit_result missing_hash_result;
    assert(otsardb_publish_commit(store,
                                &missing_parent_hash,
                                &missing_hash_result) == OTSARDB_JOURNAL_INVALID);

    otsardb_commit_request b = a;
    b.commit_id = "c2-b";
    b.segment_payload = payload2b;
    b.segment_payload_size = sizeof(payload2b);

    otsardb_commit_result r2a;
    otsardb_commit_result r2b;
    assert(otsardb_publish_commit(store, &a, &r2a) == OTSARDB_JOURNAL_OK);
    assert(strcmp(r2a.manifest.parent_manifest_sha256, r1.head.commit_sha256) == 0);
    assert(otsardb_publish_commit(store, &b, &r2b) == OTSARDB_JOURNAL_CONFLICT);

    otsardb_head final_head;
    char *final_etag = NULL;
    assert(otsardb_journal_read_head(store, "tenant/orders", &final_head, &final_etag) == OTSARDB_JOURNAL_OK);
    assert(final_head.generation == 2);
    assert(strcmp(final_head.commit_id, "c2-a") == 0);
    free(final_etag);

    otsardb_test_memory_store_destroy(store);

    /* S3 Core without conditional PUT still supports OtsarDB via SINGLE_WRITER. */
    otsardb_store_capabilities core_caps =
        OTSARDB_STORE_CAP_ETAG |
        OTSARDB_STORE_CAP_RANGE_GET |
        OTSARDB_STORE_CAP_READ_AFTER_WRITE;
    otsardb_object_store *core_store = otsardb_test_memory_store_create_with_capabilities(core_caps);
    assert(core_store != NULL);

    otsardb_store_probe_report probe = {0};
    assert(otsardb_store_probe_capabilities(core_store, "__probe__/object", &probe) == OTSARDB_STORE_OK);
    assert(probe.s3_core_ok == 1);
    assert(probe.s3_cas_ok == 0);
    assert(!otsardb_store_supports(core_store, OTSARDB_STORE_CAP_CONDITIONAL_CREATE));
    assert(!otsardb_store_supports(core_store, OTSARDB_STORE_CAP_CONDITIONAL_UPDATE));

    otsardb_db_metadata core_metadata = {0};
    core_metadata.format_version = 1;
    core_metadata.page_size = 4096;
    snprintf(core_metadata.database_id, sizeof(core_metadata.database_id), "db-core-only");
    assert(otsardb_db_metadata_create(core_store, "core/orders", &core_metadata) == OTSARDB_METADATA_OK);

    otsardb_commit_request core_first = {0};
    core_first.database_root = "core/orders";
    core_first.database_id = "db-core-only";
    core_first.commit_id = "core-c1";
    core_first.parent_generation = 0;
    core_first.page_size = 4096;
    core_first.database_size_pages = 1;
    core_first.segment_payload = payload1;
    core_first.segment_payload_size = sizeof(payload1);
    core_first.create_head_if_missing = 1;
    core_first.write_strategy = OTSARDB_WRITE_STRATEGY_SINGLE_WRITER;

    otsardb_commit_result core_r1;
    assert(otsardb_publish_commit(core_store, &core_first, &core_r1) == OTSARDB_JOURNAL_OK);
    assert(core_r1.head.generation == 1);

    otsardb_commit_request core_second = {0};
    core_second.database_root = "core/orders";
    core_second.database_id = "db-core-only";
    core_second.commit_id = "core-c2";
    core_second.parent_commit_id = "core-c1";
    core_second.parent_manifest_sha256 = core_r1.head.commit_sha256;
    core_second.parent_generation = 1;
    core_second.page_size = 4096;
    core_second.database_size_pages = 2;
    core_second.segment_payload = payload2a;
    core_second.segment_payload_size = sizeof(payload2a);
    core_second.write_strategy = OTSARDB_WRITE_STRATEGY_SINGLE_WRITER;

    otsardb_commit_result core_r2;
    assert(otsardb_publish_commit(core_store, &core_second, &core_r2) == OTSARDB_JOURNAL_OK);
    assert(core_r2.head.generation == 2);
    assert(strcmp(core_r2.head.commit_id, "core-c2") == 0);
    assert(strcmp(core_r2.manifest.parent_manifest_sha256,
                  core_r1.head.commit_sha256) == 0);

    otsardb_commit_request stale_core = core_second;
    stale_core.commit_id = "core-stale";
    otsardb_commit_result stale_result;
    assert(otsardb_publish_commit(core_store, &stale_core, &stale_result) == OTSARDB_JOURNAL_CONFLICT);

    otsardb_test_memory_store_destroy(core_store);

    /* Failures before HEAD publication never create committed state. */
    for (size_t fail_at = 1; fail_at <= 3; ++fail_at) {
        otsardb_object_store *inner = otsardb_test_memory_store_create();
        assert(inner != NULL);
        otsardb_object_store *fault = otsardb_test_fault_store_create(inner);
        assert(fault != NULL);
        otsardb_test_fault_store_fail_put(fault, fail_at, OTSARDB_STORE_ERROR, 0);

        otsardb_commit_request req = {0};
        req.database_root = "fault/db";
        req.database_id = "fault-db";
        req.commit_id = "fault-c1";
        req.parent_generation = 0;
        req.page_size = 4096;
        req.segment_payload = payload1;
        req.segment_payload_size = sizeof(payload1);
        req.create_head_if_missing = 1;
        otsardb_commit_result result;
        assert(otsardb_publish_commit(fault, &req, &result) != OTSARDB_JOURNAL_OK);

        otsardb_head absent;
        char *absent_etag = NULL;
        assert(otsardb_journal_read_head(inner, "fault/db", &absent, &absent_etag) == OTSARDB_JOURNAL_NOT_FOUND);
        free(absent_etag);
        otsardb_test_fault_store_destroy(fault);
        otsardb_test_memory_store_destroy(inner);
    }

    /* HEAD may be durable even if its response times out. Resolve by commit id. */
    otsardb_object_store *inner = otsardb_test_memory_store_create();
    assert(inner != NULL);
    otsardb_object_store *fault = otsardb_test_fault_store_create(inner);
    assert(fault != NULL);
    otsardb_test_fault_store_fail_put(fault, 3, OTSARDB_STORE_TIMEOUT, 1);

    otsardb_commit_request ambiguous = {0};
    ambiguous.database_root = "timeout/db";
    ambiguous.database_id = "timeout-db";
    ambiguous.commit_id = "timeout-c1";
    ambiguous.parent_generation = 0;
    ambiguous.page_size = 4096;
    ambiguous.segment_payload = payload1;
    ambiguous.segment_payload_size = sizeof(payload1);
    ambiguous.create_head_if_missing = 1;
    otsardb_commit_result ambiguous_result;
    assert(otsardb_publish_commit(fault, &ambiguous, &ambiguous_result) == OTSARDB_JOURNAL_AMBIGUOUS);

    otsardb_commit_state state = OTSARDB_COMMIT_STATE_UNKNOWN;
    assert(otsardb_resolve_commit(inner, "timeout/db", "timeout-c1", 0, &state) == OTSARDB_JOURNAL_OK);
    assert(state == OTSARDB_COMMIT_STATE_COMMITTED);

    otsardb_test_fault_store_destroy(fault);
    otsardb_test_memory_store_destroy(inner);

    /* A single commit may publish multiple bounded segments (manifest v4). */
    otsardb_object_store *multi_store = otsardb_test_memory_store_create();
    assert(multi_store != NULL);

    otsardb_db_metadata multi_metadata = {0};
    multi_metadata.format_version = 1;
    multi_metadata.page_size = 4096;
    snprintf(multi_metadata.database_id, sizeof(multi_metadata.database_id), "db-multi");
    assert(otsardb_db_metadata_create(multi_store, "multi/db", &multi_metadata) == OTSARDB_METADATA_OK);

    const unsigned char multi_payload1[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    const unsigned char multi_payload2[] = {0x11, 0x12, 0x13};
    const unsigned char multi_payload3[] = {0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29};
    const uint32_t multi_pages1[] = {1, 5, 9};
    const uint32_t multi_pages2[] = {2, 3};
    const uint32_t multi_pages3[] = {4, 6, 7, 8};
    otsardb_segment_input multi_inputs[3];
    multi_inputs[0].payload = multi_payload1;
    multi_inputs[0].payload_size = sizeof(multi_payload1);
    multi_inputs[0].pages = multi_pages1;
    multi_inputs[0].page_count = 3;
    multi_inputs[1].payload = multi_payload2;
    multi_inputs[1].payload_size = sizeof(multi_payload2);
    multi_inputs[1].pages = multi_pages2;
    multi_inputs[1].page_count = 2;
    multi_inputs[2].payload = multi_payload3;
    multi_inputs[2].payload_size = sizeof(multi_payload3);
    multi_inputs[2].pages = multi_pages3;
    multi_inputs[2].page_count = 4;

    otsardb_commit_request multi = {0};
    multi.database_root = "multi/db";
    multi.database_id = "db-multi";
    multi.commit_id = "multi-c1";
    multi.parent_generation = 0;
    multi.page_size = 4096;
    /* (audit F07): the page set spans pages 1..9, so the
     * declared database size must be at least 9 — page numbers beyond
     * database_size_pages are now rejected at decode. */
    multi.database_size_pages = 9;
    multi.segments = multi_inputs;
    multi.segment_count = 3;
    multi.create_head_if_missing = 1;

    otsardb_commit_result multi_result;
    assert(otsardb_publish_commit(multi_store, &multi, &multi_result) == OTSARDB_JOURNAL_OK);
    assert(multi_result.manifest.segment_count == 3);
    assert(multi_result.manifest.format_version == OTSARDB_COMMIT_MANIFEST_VERSION);
    assert(multi_result.head.generation == 1);

    size_t expected_payloads[] = {sizeof(multi_payload1), sizeof(multi_payload2), sizeof(multi_payload3)};
    for (uint32_t i = 0; i < multi_result.manifest.segment_count; ++i) {
        verify_segment_object(multi_store, "multi/db", &multi_result.manifest.segments[i]);
        otsardb_store_object multi_segment = {0};
        char multi_key[OTSARDB_KEY_MAX + 1];
        int multi_key_len = snprintf(multi_key,
                                     sizeof(multi_key),
                                     "%s/%s",
                                     "multi/db",
                                     multi_result.manifest.segments[i].key);
        assert(multi_key_len > 0 && (size_t)multi_key_len < sizeof(multi_key));
        assert(otsardb_store_get(multi_store,
                               multi_key,
                               &multi_segment) == OTSARDB_STORE_OK);
        otsardb_segment_view multi_view;
        assert(otsardb_segment_decode(multi_segment.data, multi_segment.size, &multi_view));
        assert(strcmp(multi_view.commit_id, "multi-c1") == 0);
        assert(multi_view.payload_size == expected_payloads[i]);
        const unsigned char *expected_payload = multi_inputs[i].payload;
        if (multi_view.compressed) {
            unsigned char *raw_payload = NULL;
            size_t raw_payload_size = 0;
            assert(otsardb_segment_decompress(&multi_view,
                                            (void **)&raw_payload,
                                            &raw_payload_size));
            assert(raw_payload_size == expected_payloads[i]);
            assert(memcmp(raw_payload, expected_payload, expected_payloads[i]) == 0);
            free(raw_payload);
        } else {
            assert(memcmp(multi_view.payload, expected_payload, expected_payloads[i]) == 0);
        }
        otsardb_store_release_object(multi_store, &multi_segment);
    }

    otsardb_store_object multi_manifest_object = {0};
    assert(otsardb_store_get(multi_store,
                           multi_result.head.commit_key,
                           &multi_manifest_object) == OTSARDB_STORE_OK);
    otsardb_commit_manifest multi_decoded;
    assert(otsardb_commit_manifest_decode_json((const char *)multi_manifest_object.data,
                                             multi_manifest_object.size,
                                             &multi_decoded));
    assert(multi_decoded.format_version == OTSARDB_COMMIT_MANIFEST_VERSION);
    assert(multi_decoded.segment_count == 3);
    for (uint32_t i = 0; i < multi_decoded.segment_count; ++i) {
        assert(strcmp(multi_decoded.segments[i].key,
                      multi_result.manifest.segments[i].key) == 0);
        assert(strcmp(multi_decoded.segments[i].sha256,
                      multi_result.manifest.segments[i].sha256) == 0);
        assert(multi_decoded.segments[i].size == multi_result.manifest.segments[i].size);
        /* v4 round-trip: the decoded manifest must carry the per-segment page
         * sets that were passed in, and no frame offsets were recorded. */
        assert(multi_decoded.segments[i].page_count == multi_inputs[i].page_count);
        assert(multi_decoded.segments[i].frame_offsets == NULL);
        for (uint32_t j = 0; j < multi_decoded.segments[i].page_count; ++j) {
            assert(multi_decoded.segments[i].pages[j] == multi_inputs[i].pages[j]);
        }
    }
    otsardb_commit_manifest_free(&multi_decoded);
    otsardb_store_release_object(multi_store, &multi_manifest_object);

    /* Requests that mix legacy payload with the segments array, or declare a
     * segments array with no entries, are invalid. */
    otsardb_commit_request bad_mixed = multi;
    bad_mixed.segment_payload = multi_payload1;
    bad_mixed.segment_payload_size = sizeof(multi_payload1);
    otsardb_commit_result bad_result;
    assert(otsardb_publish_commit(multi_store, &bad_mixed, &bad_result) == OTSARDB_JOURNAL_INVALID);

    otsardb_commit_request bad_empty = multi;
    bad_empty.segment_count = 0;
    assert(otsardb_publish_commit(multi_store, &bad_empty, &bad_result) == OTSARDB_JOURNAL_INVALID);

    /* A child commit chains onto the multi-segment genesis via CAS. */
    otsardb_commit_request child = {0};
    child.database_root = "multi/db";
    child.database_id = "db-multi";
    child.commit_id = "multi-c2";
    child.parent_commit_id = "multi-c1";
    child.parent_manifest_sha256 = multi_result.head.commit_sha256;
    child.parent_generation = 1;
    child.page_size = 4096;
    /* F07: child reuses multi_inputs whose pages span 1..9. */
    child.database_size_pages = 9;
    child.segments = multi_inputs;
    child.segment_count = 2;
    child.expected_head_etag = multi_result.head_etag;
    otsardb_commit_result child_result;
    assert(otsardb_publish_commit(multi_store, &child, &child_result) == OTSARDB_JOURNAL_OK);
    assert(child_result.head.generation == 2);
    assert(strcmp(child_result.head.commit_id, "multi-c2") == 0);
    assert(child_result.manifest.segment_count == 2);
    assert(strcmp(child_result.manifest.parent_manifest_sha256,
                  multi_result.head.commit_sha256) == 0);

    otsardb_test_memory_store_destroy(multi_store);

    puts("commit publication tests passed");
    return 0;
}
