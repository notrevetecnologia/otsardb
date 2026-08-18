#include "commit_manifest.h"
#include "commit_publish.h"
#include "db_metadata.h"
#include "fault_store.h"
#include "journal.h"
#include "memory_store.h"
#include "sqlite3.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* M3 conflict detection: deterministic multi-writer
 * scenarios over memory_store + fault_store. Two (or three) writers race
 * from the same parent; the CAS winner publishes, the losers are classified
 * by otsardb_publish_commit_detect_conflict into page-overlap conflicts
 * (SQLITE_BUSY_SNAPSHOT) vs disjoint stale writes (plain SQLITE_BUSY). */

/* Mirrors the mapping in publish_one_commit (wal_publisher.c): page overlap
 * -> extended code SQLITE_BUSY_SNAPSHOT (517), else plain SQLITE_BUSY (5). */
static int conflict_sqlite_rc(int page_overlap) {
    return page_overlap ? SQLITE_BUSY_SNAPSHOT : SQLITE_BUSY;
}

static void create_metadata(otsardb_object_store *store,
                            const char *root,
                            const char *database_id) {
    otsardb_db_metadata metadata = {0};
    metadata.format_version = 1;
    metadata.page_size = 4096;
    snprintf(metadata.database_id, sizeof(metadata.database_id), "%s", database_id);
    assert(otsardb_db_metadata_create(store, root, &metadata) == OTSARDB_METADATA_OK);
}

static otsardb_segment_input segment_input(const void *payload,
                                         size_t payload_size,
                                         const uint32_t *pages,
                                         uint32_t page_count) {
    otsardb_segment_input input;
    memset(&input, 0, sizeof(input));
    input.payload = payload;
    input.payload_size = payload_size;
    input.pages = pages;
    input.page_count = page_count;
    return input;
}

/* Single-segment request via the v4 segments array. Pages for the input may
 * be NULL/0 (unknown page set) or a sorted, deduped list. */
static otsardb_commit_request commit_request(const char *root,
                                           const char *database_id,
                                           const char *commit_id,
                                           uint64_t parent_generation,
                                           const char *parent_commit_id,
                                           const char *parent_sha,
                                           const char *head_etag,
                                           const otsardb_segment_input *input,
                                           uint32_t segment_count,
                                           int create_head_if_missing) {
    otsardb_commit_request request;
    memset(&request, 0, sizeof(request));
    request.database_root = root;
    request.database_id = database_id;
    request.commit_id = commit_id;
    request.parent_generation = parent_generation;
    request.parent_commit_id = parent_commit_id;
    request.parent_manifest_sha256 = parent_sha;
    request.expected_head_etag = head_etag;
    request.create_head_if_missing = create_head_if_missing;
    request.page_size = 4096;
    request.database_size_pages = 64;
    request.segments = input;
    request.segment_count = segment_count;
    return request;
}

static otsardb_commit_request legacy_request(const char *root,
                                           const char *database_id,
                                           const char *commit_id,
                                           uint64_t parent_generation,
                                           const char *parent_commit_id,
                                           const char *parent_sha,
                                           const char *head_etag,
                                           const void *payload,
                                           size_t payload_size,
                                           int create_head_if_missing) {
    otsardb_commit_request request;
    memset(&request, 0, sizeof(request));
    request.database_root = root;
    request.database_id = database_id;
    request.commit_id = commit_id;
    request.parent_generation = parent_generation;
    request.parent_commit_id = parent_commit_id;
    request.parent_manifest_sha256 = parent_sha;
    request.expected_head_etag = head_etag;
    request.create_head_if_missing = create_head_if_missing;
    request.page_size = 4096;
    request.database_size_pages = 64;
    request.segment_payload = payload;
    request.segment_payload_size = payload_size;
    return request;
}

static void assert_detected(otsardb_object_store *store,
                            const char *root,
                            const otsardb_commit_request *request,
                            otsardb_commit_state expected_state,
                            int expected_overlap) {
    otsardb_commit_state state = OTSARDB_COMMIT_STATE_UNKNOWN;
    int overlap = -1;
    assert(otsardb_publish_commit_detect_conflict(store, root, request, &state, &overlap) ==
           OTSARDB_JOURNAL_OK);
    assert(state == expected_state);
    assert(overlap == expected_overlap);
    assert(conflict_sqlite_rc(overlap) == (expected_overlap ? SQLITE_BUSY_SNAPSHOT
                                                            : SQLITE_BUSY));
}

/* Scenario A: two commits from the same parent with DISJOINT page sets.
 * First publish succeeds; second fails the CAS; detection reports no page
 * overlap -> plain SQLITE_BUSY (stale writer, retryable). */
static void scenario_disjoint_pages(otsardb_object_store *store) {
    const char *root = "conflict/a";
    create_metadata(store, root, "conf-a");

    const unsigned char payload_genesis[] = {0x10};
    const uint32_t genesis_pages[] = {10};
    otsardb_segment_input genesis_input =
        segment_input(payload_genesis, sizeof(payload_genesis), genesis_pages, 1);
    otsardb_commit_request genesis = commit_request(root,
                                                  "conf-a",
                                                  "gen-a",
                                                  0,
                                                  NULL,
                                                  NULL,
                                                  NULL,
                                                  &genesis_input,
                                                  1,
                                                  1);
    otsardb_commit_result r_genesis;
    assert(otsardb_publish_commit(store, &genesis, &r_genesis) == OTSARDB_JOURNAL_OK);
    assert(r_genesis.head.generation == 1);

    const unsigned char payload_w1[] = {0x21};
    const uint32_t w1_pages[] = {1, 2};
    otsardb_segment_input w1_input =
        segment_input(payload_w1, sizeof(payload_w1), w1_pages, 2);
    otsardb_commit_request w1 = commit_request(root,
                                             "conf-a",
                                             "w1-a",
                                             1,
                                             "gen-a",
                                             r_genesis.head.commit_sha256,
                                             r_genesis.head_etag,
                                             &w1_input,
                                             1,
                                             0);
    otsardb_commit_result r_w1;
    assert(otsardb_publish_commit(store, &w1, &r_w1) == OTSARDB_JOURNAL_OK);
    assert(r_w1.head.generation == 2);

    const unsigned char payload_w2[] = {0x31};
    const uint32_t w2_pages[] = {3, 4};
    otsardb_segment_input w2_input =
        segment_input(payload_w2, sizeof(payload_w2), w2_pages, 2);
    otsardb_commit_request w2 = commit_request(root,
                                             "conf-a",
                                             "w2-a",
                                             1,
                                             "gen-a",
                                             r_genesis.head.commit_sha256,
                                             r_genesis.head_etag,
                                             &w2_input,
                                             1,
                                             0);
    otsardb_commit_result r_w2;
    assert(otsardb_publish_commit(store, &w2, &r_w2) == OTSARDB_JOURNAL_CONFLICT);

    assert_detected(store, root, &w2, OTSARDB_COMMIT_STATE_SUPERSEDED, 0);

    /* The winning commit's own page set {1,2} intersects neither the parent
     * (not interleaved) nor the loser: state COMMITTED and no overlap. */
    assert_detected(store, root, &w1, OTSARDB_COMMIT_STATE_COMMITTED, 0);

    /* A request based on the CURRENT head sees nothing interleaved. */
    otsardb_commit_request settled = commit_request(root,
                                                  "conf-a",
                                                  "w3-a",
                                                  2,
                                                  "w1-a",
                                                  r_w1.head.commit_sha256,
                                                  r_w1.head_etag,
                                                  &w2_input,
                                                  1,
                                                  0);
    assert_detected(store, root, &settled, OTSARDB_COMMIT_STATE_NOT_COMMITTED, 0);
}

/* Scenario B: two commits from the same parent SHARING at least one page.
 * Second publish fails the CAS; detection reports page overlap ->
 * SQLITE_BUSY_SNAPSHOT. */
static void scenario_overlapping_pages(otsardb_object_store *store) {
    const char *root = "conflict/b";
    create_metadata(store, root, "conf-b");

    const unsigned char payload_genesis[] = {0x10};
    const uint32_t genesis_pages[] = {10};
    otsardb_segment_input genesis_input =
        segment_input(payload_genesis, sizeof(payload_genesis), genesis_pages, 1);
    otsardb_commit_request genesis = commit_request(root,
                                                  "conf-b",
                                                  "gen-b",
                                                  0,
                                                  NULL,
                                                  NULL,
                                                  NULL,
                                                  &genesis_input,
                                                  1,
                                                  1);
    otsardb_commit_result r_genesis;
    assert(otsardb_publish_commit(store, &genesis, &r_genesis) == OTSARDB_JOURNAL_OK);

    const unsigned char payload_w1[] = {0x22};
    const uint32_t w1_pages[] = {1, 2};
    otsardb_segment_input w1_input =
        segment_input(payload_w1, sizeof(payload_w1), w1_pages, 2);
    otsardb_commit_request w1 = commit_request(root,
                                             "conf-b",
                                             "w1-b",
                                             1,
                                             "gen-b",
                                             r_genesis.head.commit_sha256,
                                             r_genesis.head_etag,
                                             &w1_input,
                                             1,
                                             0);
    otsardb_commit_result r_w1;
    assert(otsardb_publish_commit(store, &w1, &r_w1) == OTSARDB_JOURNAL_OK);

    /* w2 shares page 2 with the interleaved w1 commit. */
    const unsigned char payload_w2[] = {0x32};
    const uint32_t w2_pages[] = {2, 3};
    otsardb_segment_input w2_input =
        segment_input(payload_w2, sizeof(payload_w2), w2_pages, 2);
    otsardb_commit_request w2 = commit_request(root,
                                             "conf-b",
                                             "w2-b",
                                             1,
                                             "gen-b",
                                             r_genesis.head.commit_sha256,
                                             r_genesis.head_etag,
                                             &w2_input,
                                             1,
                                             0);
    otsardb_commit_result r_w2;
    assert(otsardb_publish_commit(store, &w2, &r_w2) == OTSARDB_JOURNAL_CONFLICT);

    assert_detected(store, root, &w2, OTSARDB_COMMIT_STATE_SUPERSEDED, 1);
}

/* Scenario C: three writers race from the same parent. The winner commits
 * pages 1-2; the overlapping loser (pages 2-3) is a snapshot conflict; the
 * disjoint loser (pages 4-5) is a plain stale write. */
static void scenario_three_interleaved(otsardb_object_store *store) {
    const char *root = "conflict/c";
    create_metadata(store, root, "conf-c");

    const unsigned char payload_genesis[] = {0x10};
    const uint32_t genesis_pages[] = {10};
    otsardb_segment_input genesis_input =
        segment_input(payload_genesis, sizeof(payload_genesis), genesis_pages, 1);
    otsardb_commit_request genesis = commit_request(root,
                                                  "conf-c",
                                                  "gen-c",
                                                  0,
                                                  NULL,
                                                  NULL,
                                                  NULL,
                                                  &genesis_input,
                                                  1,
                                                  1);
    otsardb_commit_result r_genesis;
    assert(otsardb_publish_commit(store, &genesis, &r_genesis) == OTSARDB_JOURNAL_OK);

    const unsigned char payload_w1[] = {0x24};
    const uint32_t w1_pages[] = {1, 2};
    otsardb_segment_input w1_input =
        segment_input(payload_w1, sizeof(payload_w1), w1_pages, 2);
    otsardb_commit_request w1 = commit_request(root,
                                             "conf-c",
                                             "w1-c",
                                             1,
                                             "gen-c",
                                             r_genesis.head.commit_sha256,
                                             r_genesis.head_etag,
                                             &w1_input,
                                             1,
                                             0);
    otsardb_commit_result r_w1;
    assert(otsardb_publish_commit(store, &w1, &r_w1) == OTSARDB_JOURNAL_OK);

    const unsigned char payload_w2[] = {0x34};
    const uint32_t w2_pages[] = {2, 3};
    otsardb_segment_input w2_input =
        segment_input(payload_w2, sizeof(payload_w2), w2_pages, 2);
    otsardb_commit_request w2 = commit_request(root,
                                             "conf-c",
                                             "w2-c",
                                             1,
                                             "gen-c",
                                             r_genesis.head.commit_sha256,
                                             r_genesis.head_etag,
                                             &w2_input,
                                             1,
                                             0);
    otsardb_commit_result r_w2;
    assert(otsardb_publish_commit(store, &w2, &r_w2) == OTSARDB_JOURNAL_CONFLICT);

    const unsigned char payload_w3[] = {0x44};
    const uint32_t w3_pages[] = {4, 5};
    otsardb_segment_input w3_input =
        segment_input(payload_w3, sizeof(payload_w3), w3_pages, 2);
    otsardb_commit_request w3 = commit_request(root,
                                             "conf-c",
                                             "w3-c",
                                             1,
                                             "gen-c",
                                             r_genesis.head.commit_sha256,
                                             r_genesis.head_etag,
                                             &w3_input,
                                             1,
                                             0);
    otsardb_commit_result r_w3;
    assert(otsardb_publish_commit(store, &w3, &r_w3) == OTSARDB_JOURNAL_CONFLICT);

    assert_detected(store, root, &w2, OTSARDB_COMMIT_STATE_SUPERSEDED, 1);
    assert_detected(store, root, &w3, OTSARDB_COMMIT_STATE_SUPERSEDED, 0);
}

/* Scenario D: the LOSER carries no page info (legacy single-segment path):
 * overlap-unknown is conservative -> BUSY_SNAPSHOT even though the winner's
 * pages are known. */
static void scenario_legacy_loser(otsardb_object_store *store) {
    const char *root = "conflict/d";
    create_metadata(store, root, "conf-d");

    const unsigned char payload_genesis[] = {0x10};
    const uint32_t genesis_pages[] = {10};
    otsardb_segment_input genesis_input =
        segment_input(payload_genesis, sizeof(payload_genesis), genesis_pages, 1);
    otsardb_commit_request genesis = commit_request(root,
                                                  "conf-d",
                                                  "gen-d",
                                                  0,
                                                  NULL,
                                                  NULL,
                                                  NULL,
                                                  &genesis_input,
                                                  1,
                                                  1);
    otsardb_commit_result r_genesis;
    assert(otsardb_publish_commit(store, &genesis, &r_genesis) == OTSARDB_JOURNAL_OK);

    const unsigned char payload_w1[] = {0x25};
    const uint32_t w1_pages[] = {1, 2};
    otsardb_segment_input w1_input =
        segment_input(payload_w1, sizeof(payload_w1), w1_pages, 2);
    otsardb_commit_request w1 = commit_request(root,
                                             "conf-d",
                                             "w1-d",
                                             1,
                                             "gen-d",
                                             r_genesis.head.commit_sha256,
                                             r_genesis.head_etag,
                                             &w1_input,
                                             1,
                                             0);
    otsardb_commit_result r_w1;
    assert(otsardb_publish_commit(store, &w1, &r_w1) == OTSARDB_JOURNAL_OK);

    const unsigned char payload_w2[] = {0x35};
    otsardb_commit_request w2 = legacy_request(root,
                                             "conf-d",
                                             "w2-d",
                                             1,
                                             "gen-d",
                                             r_genesis.head.commit_sha256,
                                             r_genesis.head_etag,
                                             payload_w2,
                                             sizeof(payload_w2),
                                             0);
    otsardb_commit_result r_w2;
    assert(otsardb_publish_commit(store, &w2, &r_w2) == OTSARDB_JOURNAL_CONFLICT);

    assert_detected(store, root, &w2, OTSARDB_COMMIT_STATE_SUPERSEDED, 1);
}

/* Scenario E: the interleaved WINNER was published without page info (its
 * v4 manifest records "pages":[], decoded as page_count 0 / NULL): the
 * unknown page set of an interleaved manifest conservatively overlaps
 * everything -> BUSY_SNAPSHOT. */
static void scenario_legacy_winner(otsardb_object_store *store) {
    const char *root = "conflict/e";
    create_metadata(store, root, "conf-e");

    const unsigned char payload_genesis[] = {0x10};
    const uint32_t genesis_pages[] = {10};
    otsardb_segment_input genesis_input =
        segment_input(payload_genesis, sizeof(payload_genesis), genesis_pages, 1);
    otsardb_commit_request genesis = commit_request(root,
                                                  "conf-e",
                                                  "gen-e",
                                                  0,
                                                  NULL,
                                                  NULL,
                                                  NULL,
                                                  &genesis_input,
                                                  1,
                                                  1);
    otsardb_commit_result r_genesis;
    assert(otsardb_publish_commit(store, &genesis, &r_genesis) == OTSARDB_JOURNAL_OK);

    const unsigned char payload_w1[] = {0x26};
    otsardb_commit_request w1 = legacy_request(root,
                                             "conf-e",
                                             "w1-e",
                                             1,
                                             "gen-e",
                                             r_genesis.head.commit_sha256,
                                             r_genesis.head_etag,
                                             payload_w1,
                                             sizeof(payload_w1),
                                             0);
    otsardb_commit_result r_w1;
    assert(otsardb_publish_commit(store, &w1, &r_w1) == OTSARDB_JOURNAL_OK);

    /* The winner's manifest must actually carry an empty (unknown) page set. */
    otsardb_store_object manifest_object = {0};
    assert(otsardb_store_get(store, r_w1.head.commit_key, &manifest_object) == OTSARDB_STORE_OK);
    otsardb_commit_manifest decoded;
    assert(otsardb_commit_manifest_decode_json((const char *)manifest_object.data,
                                             manifest_object.size,
                                             &decoded));
    assert(decoded.format_version == OTSARDB_COMMIT_MANIFEST_VERSION);
    assert(decoded.segment_count == 1);
    assert(decoded.segments[0].page_count == 0);
    assert(decoded.segments[0].pages == NULL);
    otsardb_commit_manifest_free(&decoded);
    otsardb_store_release_object(store, &manifest_object);

    const unsigned char payload_w2[] = {0x36};
    const uint32_t w2_pages[] = {4, 5};
    otsardb_segment_input w2_input =
        segment_input(payload_w2, sizeof(payload_w2), w2_pages, 2);
    otsardb_commit_request w2 = commit_request(root,
                                             "conf-e",
                                             "w2-e",
                                             1,
                                             "gen-e",
                                             r_genesis.head.commit_sha256,
                                             r_genesis.head_etag,
                                             &w2_input,
                                             1,
                                             0);
    otsardb_commit_result r_w2;
    assert(otsardb_publish_commit(store, &w2, &r_w2) == OTSARDB_JOURNAL_CONFLICT);

    assert_detected(store, root, &w2, OTSARDB_COMMIT_STATE_SUPERSEDED, 1);
}

/* Scenario F: detection errors propagate instead of guessing. A failed HEAD
 * read or a failed interleaved-manifest read must surface as an error (the
 * publisher maps it through journal_result_to_sqlite), never as a bogus
 * overlap verdict. Also: a genesis whose HEAD put timed out (AMBIGUOUS) but
 * actually won resolves COMMITTED with no overlap. Uses its own fault-wrapped
 * store. */
static void scenario_faults(void) {
    otsardb_object_store *inner = otsardb_test_memory_store_create();
    assert(inner != NULL);
    otsardb_object_store *store = otsardb_test_fault_store_create(inner);
    assert(store != NULL);
    const char *root = "conflict/f";
    create_metadata(store, root, "conf-f");

    const unsigned char payload_genesis[] = {0x10};
    const uint32_t genesis_pages[] = {10};
    otsardb_segment_input genesis_input =
        segment_input(payload_genesis, sizeof(payload_genesis), genesis_pages, 1);
    otsardb_commit_request genesis = commit_request(root,
                                                  "conf-f",
                                                  "gen-f",
                                                  0,
                                                  NULL,
                                                  NULL,
                                                  NULL,
                                                  &genesis_input,
                                                  1,
                                                  1);
    otsardb_commit_result r_genesis;
    assert(otsardb_publish_commit(store, &genesis, &r_genesis) == OTSARDB_JOURNAL_OK);

    const unsigned char payload_w1[] = {0x27};
    const uint32_t w1_pages[] = {1, 2};
    otsardb_segment_input w1_input =
        segment_input(payload_w1, sizeof(payload_w1), w1_pages, 2);
    otsardb_commit_request w1 = commit_request(root,
                                             "conf-f",
                                             "w1-f",
                                             1,
                                             "gen-f",
                                             r_genesis.head.commit_sha256,
                                             r_genesis.head_etag,
                                             &w1_input,
                                             1,
                                             0);
    otsardb_commit_result r_w1;
    assert(otsardb_publish_commit(store, &w1, &r_w1) == OTSARDB_JOURNAL_OK);

    const unsigned char payload_w2[] = {0x37};
    const uint32_t w2_pages[] = {3, 4};
    otsardb_segment_input w2_input =
        segment_input(payload_w2, sizeof(payload_w2), w2_pages, 2);
    otsardb_commit_request w2 = commit_request(root,
                                             "conf-f",
                                             "w2-f",
                                             1,
                                             "gen-f",
                                             r_genesis.head.commit_sha256,
                                             r_genesis.head_etag,
                                             &w2_input,
                                             1,
                                             0);
    otsardb_commit_result r_w2;
    assert(otsardb_publish_commit(store, &w2, &r_w2) == OTSARDB_JOURNAL_CONFLICT);

    /* GET #1 is the HEAD read: detection must fail loudly, not guess. */
    otsardb_test_fault_store_fail_get(store, 1, OTSARDB_STORE_ERROR);
    otsardb_commit_state state = OTSARDB_COMMIT_STATE_UNKNOWN;
    int overlap = -1;
    assert(otsardb_publish_commit_detect_conflict(store, root, &w2, &state, &overlap) ==
           OTSARDB_JOURNAL_ERROR);
    otsardb_test_fault_store_clear(store);

    /* GET #2 is the first interleaved manifest read (HEAD's manifest). */
    otsardb_test_fault_store_fail_get(store, 2, OTSARDB_STORE_ERROR);
    assert(otsardb_publish_commit_detect_conflict(store, root, &w2, &state, &overlap) ==
           OTSARDB_JOURNAL_ERROR);
    otsardb_test_fault_store_clear(store);

    /* Detection still works after the injected faults. */
    assert_detected(store, root, &w2, OTSARDB_COMMIT_STATE_SUPERSEDED, 0);

    /* A HEAD put that times out after delegating (AMBIGUOUS) on a genesis
     * that actually won: detection resolves it COMMITTED with no overlap. */
    const char *root2 = "conflict/f2";
    create_metadata(store, root2, "conf-f2");
    otsardb_commit_request genesis2 = commit_request(root2,
                                                   "conf-f2",
                                                   "gen-f2",
                                                   0,
                                                   NULL,
                                                   NULL,
                                                   NULL,
                                                   &genesis_input,
                                                   1,
                                                   1);
    otsardb_test_fault_store_fail_put(store, 3, OTSARDB_STORE_TIMEOUT, 1);
    otsardb_commit_result r_genesis2;
    assert(otsardb_publish_commit(store, &genesis2, &r_genesis2) == OTSARDB_JOURNAL_AMBIGUOUS);
    otsardb_test_fault_store_clear(store);
    assert_detected(store, root2, &genesis2, OTSARDB_COMMIT_STATE_COMMITTED, 0);

    otsardb_test_fault_store_destroy(store);
    otsardb_test_memory_store_destroy(inner);
}

int main(void) {
    otsardb_object_store *store = otsardb_test_memory_store_create();
    assert(store != NULL);

    scenario_disjoint_pages(store);
    scenario_overlapping_pages(store);
    scenario_three_interleaved(store);
    scenario_legacy_loser(store);
    scenario_legacy_winner(store);
    scenario_faults();

    otsardb_test_memory_store_destroy(store);
    puts("page-level conflict detection tests passed");
    return 0;
}
