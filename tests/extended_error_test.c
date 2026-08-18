#include "otsardb.h"
#include "otsardb_internal.h"
#include "commit_publish.h"
#include "fault_store.h"
#include "journal.h"
#include "local_vfs.h"
#include "memory_store.h"
#include "object_store.h"
#include "sqlite3.h"
#include "sqlite_wal.h"
#include "wal_publisher.h"

#include <assert.h>
#include <limits.h>
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

/* deterministic extended-error-code test: the application
 * boundary of the conflict classification.
 *
 * 0030 classified CAS failures into page-overlap conflicts
 * (SQLITE_BUSY_SNAPSHOT, extended code 517) vs disjoint stale writes
 * (plain SQLITE_BUSY, 5) but explicitly recorded that the propagation of
 * 517 through a real WAL sync to the application was NOT observed; 0095
 * re-observed that the CLI cannot print extended codes (5 and 517 produce
 * byte-identical stderr). This test closes that gap at the C API boundary:
 *
 * S1/S2 drive otsardb_wal_publisher_sync_reader directly (memory_store,
 * synthetic WAL, wal_publisher_retry_test pattern) and reproduce
 * the 0095 scenario-D same-generation refresh->CAS race
 * DETERMINISTICALLY: the read_at callback publishes the winning
 * commit INSIDE the loser's sync - after the loser's refresh_head
 * read, before the loser's HEAD CAS. The loser's CAS then fails
 * for real (etag changed mid-sync) and publish_one_commit runs the
 * genuine 0030 conflict walk over the interleaved manifest.
 * Assertions: rc == SQLITE_BUSY_SNAPSHOT (517) on overlap,
 * rc == SQLITE_BUSY (5) on disjoint, the publisher's
 * sqlite_extended_conflict flag matches, HEAD advanced exactly to
 * the winner, the loser's commit is NOT acked.
 *
 * S3/S4 exercise the PHYSICAL propagation path through a real sqlite3
 * connection (otsardb_open on the local VFS, which enables
 * sqlite3_extended_result_codes): a foreign chain exists (gen 1 +
 * a winning gen 2 committed directly on the store) and the
 * refresh's HEAD read is faulted to NOT_FOUND so the loser's sync
 * attempts the create-CAS against the existing foreign head. The
 * real 412 -> CONFLICT is classified by the real 0030 walk, the
 * xSync hook returns the extended code, and the assertion is
 * sqlite3_extended_errcode() == SQLITE_BUSY_SNAPSHOT (overlap) /
 * SQLITE_BUSY (disjoint), with sqlite3_errcode() == 5 in both.
 *
 * On the PRE-0030 behavior every CAS failure mapped to plain SQLITE_BUSY:
 * S1 and S3 fail deterministically (they assert 517) while S2/S4 pass -
 * so the test is a real regression gate, not a tautology.
 */

#define TEST_PAGE_SIZE 4096u

/* ---- synthetic WAL construction (wal_publisher_retry_test pattern) ------ */

static uint32_t get_be32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void put_be32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static void wal_checksum(const unsigned char *data, size_t size,
                         uint32_t *s0, uint32_t *s1) {
    for (size_t i = 0; i + 8 <= size; i += 8) {
        uint32_t w0 = (uint32_t)data[i] | ((uint32_t)data[i + 1] << 8) |
                      ((uint32_t)data[i + 2] << 16) | ((uint32_t)data[i + 3] << 24);
        uint32_t w1 = (uint32_t)data[i + 4] | ((uint32_t)data[i + 5] << 8) |
                      ((uint32_t)data[i + 6] << 16) | ((uint32_t)data[i + 7] << 24);
        *s0 += w0 + *s1;
        *s1 += w1 + *s0;
    }
}

static void build_wal_header(unsigned char *hdr, uint32_t page_size,
                             uint32_t salt1, uint32_t salt2) {
    memset(hdr, 0, OTSARDB_SQLITE_WAL_HEADER_SIZE);
    put_be32(hdr, 0x377f0682u);
    put_be32(hdr + 4, 3007000u);
    put_be32(hdr + 8, page_size);
    put_be32(hdr + 12, 1u);
    put_be32(hdr + 16, salt1);
    put_be32(hdr + 20, salt2);
    uint32_t s0 = 0;
    uint32_t s1 = 0;
    wal_checksum(hdr, OTSARDB_SQLITE_WAL_HEADER_SIZE - 8u, &s0, &s1);
    put_be32(hdr + 24, s0);
    put_be32(hdr + 28, s1);
}

static void build_wal_frame(unsigned char *frame, uint32_t page_no,
                            uint32_t n_truncate, const unsigned char *page,
                            uint32_t page_size, uint32_t salt1, uint32_t salt2,
                            uint32_t *s0, uint32_t *s1) {
    memset(frame, 0, OTSARDB_SQLITE_WAL_FRAME_HEADER_SIZE + page_size);
    put_be32(frame, page_no);
    put_be32(frame + 4, n_truncate);
    put_be32(frame + 8, salt1);
    put_be32(frame + 12, salt2);
    memcpy(frame + OTSARDB_SQLITE_WAL_FRAME_HEADER_SIZE, page, page_size);
    wal_checksum(frame, 8, s0, s1);
    wal_checksum(frame + OTSARDB_SQLITE_WAL_FRAME_HEADER_SIZE, page_size, s0, s1);
    put_be32(frame + 16, *s0);
    put_be32(frame + 20, *s1);
}

static const char *WAL_PATH = "otsardb-extended-error.wal";

/* Two commits: genesis frames 1-3 (pages 1,2,3), child frames 4-6 (pages
 * 7,8,9) - the retry-test WAL. sync 1 publishes only the genesis; sync 2
 * publishes the child (or races it against the injected winner). */
static void build_synthetic_wal(uint32_t salt1, uint32_t salt2,
                                size_t *out_sync1_size, size_t *out_full_size) {
    size_t frame_size = (size_t)OTSARDB_SQLITE_WAL_FRAME_HEADER_SIZE + TEST_PAGE_SIZE;
    size_t full_size = (size_t)OTSARDB_SQLITE_WAL_HEADER_SIZE + 6u * frame_size;
    unsigned char *wal = (unsigned char *)malloc(full_size);
    assert(wal != NULL);
    build_wal_header(wal, TEST_PAGE_SIZE, salt1, salt2);

    unsigned char *page = (unsigned char *)malloc(TEST_PAGE_SIZE);
    assert(page != NULL);
    uint32_t s0 = 0;
    uint32_t s1 = 0;
    wal_checksum(wal, OTSARDB_SQLITE_WAL_HEADER_SIZE - 8u, &s0, &s1);
    for (uint32_t i = 0; i < 6u; ++i) {
        uint32_t page_no = i < 3u ? i + 1u : i + 4u;
        memset(page, (int)(page_no * 7u), TEST_PAGE_SIZE);
        uint32_t n_truncate = i == 2u ? 3u : (i == 5u ? 9u : 0u);
        build_wal_frame(wal + OTSARDB_SQLITE_WAL_HEADER_SIZE + i * frame_size,
                        page_no, n_truncate, page, TEST_PAGE_SIZE,
                        salt1, salt2, &s0, &s1);
    }
    free(page);

    FILE *f = fopen(WAL_PATH, "wb");
    assert(f != NULL);
    assert(fwrite(wal, 1, full_size, f) == full_size);
    assert(fclose(f) == 0);
    free(wal);

    *out_sync1_size = (size_t)OTSARDB_SQLITE_WAL_HEADER_SIZE + 3u * frame_size;
    *out_full_size = full_size;
}

typedef struct wal_file_reader {
    FILE *file;
} wal_file_reader;

static int wal_file_read_at(void *ctx, uint64_t offset, void *buffer, size_t size) {
    wal_file_reader *reader = (wal_file_reader *)ctx;
    if (!reader || !reader->file || offset > (uint64_t)LONG_MAX) return 0;
    if (fseek(reader->file, (long)offset, SEEK_SET) != 0) return 0;
    return fread(buffer, 1, size, reader->file) == size;
}

/* ---- store helpers ------------------------------------------------------- */

static void expect_head_generation(otsardb_object_store *store,
                                   const char *root,
                                   uint64_t expected) {
    otsardb_head head;
    char *etag = NULL;
    otsardb_journal_result rc = otsardb_journal_read_head(store, root, &head, &etag);
    free(etag);
    assert(rc == OTSARDB_JOURNAL_OK);
    assert(head.generation == expected);
}

static otsardb_head read_head(otsardb_object_store *store, const char *root) {
    otsardb_head head;
    char *etag = NULL;
    assert(otsardb_journal_read_head(store, root, &head, &etag) == OTSARDB_JOURNAL_OK);
    free(etag);
    return head;
}

static int count_cb(const char *key, void *ctx) {
    (void)key;
    ++*(int *)ctx;
    return 0;
}

static int count_manifests(otsardb_object_store *store, const char *root) {
    char prefix[OTSARDB_KEY_MAX + 1];
    int n = snprintf(prefix, sizeof(prefix), "%s/commits/", root);
    assert(n > 0 && (size_t)n < sizeof(prefix));
    int count = 0;
    assert(otsardb_store_list(store, prefix, count_cb, &count) == OTSARDB_STORE_OK);
    return count;
}

/* ---- the "other writer": a direct commit on the shared store ------------- */

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

/* Publish a foreign commit. `parent` describes the chain state the foreign
 * writer believes it is on (NULL parent fields + create=1 for a genesis).
 * The foreign writer races the CURRENT holder: it wins when it publishes
 * while the holder's publish cycle is still in flight. */
static otsardb_commit_result publish_foreign(otsardb_object_store *store,
                                           const char *root,
                                           const char *database_id,
                                           const char *commit_id,
                                           uint64_t parent_generation,
                                           const char *parent_commit_id,
                                           const char *parent_sha,
                                           const char *head_etag,
                                           int create_head_if_missing,
                                           const uint32_t *pages,
                                           uint32_t page_count) {
    const unsigned char payload[] = {0x42, 0x41};
    otsardb_segment_input input = segment_input(payload, sizeof(payload), pages, page_count);

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
    request.page_size = TEST_PAGE_SIZE;
    request.database_size_pages = 64;
    request.segments = &input;
    request.segment_count = 1;

    otsardb_commit_result result;
    assert(otsardb_publish_commit(store, &request, &result) == OTSARDB_JOURNAL_OK);
    return result;
}

/* ---- S1/S2: deterministic same-generation CAS race -----------------------
 *
 * The read_at wrapper publishes the winning commit when the loser's sync
 * first reads the CHILD commit's frames - i.e. strictly AFTER the loser's
 * refresh_head read (which happens before the WAL scan) and strictly
 * BEFORE the loser's publish cycle (which happens after the scan). The
 * loser's HEAD CAS then fails against the winner's new etag and
 * publish_one_commit classifies the conflict with the REAL 0030 walk. */

typedef struct race_reader {
    wal_file_reader file;
    otsardb_object_store *store;
    const char *root;
    const char *database_id;
    const char *winner_id;
    const char *parent_commit_id;
    const char *parent_sha;
    const char *parent_etag;
    const uint32_t *winner_pages;
    uint32_t winner_page_count;
    uint64_t trigger_offset;
    int fired;
} race_reader;

static int race_read_at(void *ctx, uint64_t offset, void *buffer, size_t size) {
    race_reader *race = (race_reader *)ctx;
    if (!race->fired && offset >= race->trigger_offset) {
        race->fired = 1;
        otsardb_commit_result r = publish_foreign(race->store,
                                                race->root,
                                                race->database_id,
                                                race->winner_id,
                                                1,
                                                race->parent_commit_id,
                                                race->parent_sha,
                                                race->parent_etag,
                                                0,
                                                race->winner_pages,
                                                race->winner_page_count);
        assert(r.head.generation == 2);
        assert(strcmp(r.head.commit_id, race->winner_id) == 0);
    }
    return wal_file_read_at(&race->file, offset, buffer, size);
}

static int drive_race_sync(size_t wal_size,
                           race_reader *race,
                           otsardb_wal_publisher *publisher) {
    race->file.file = fopen(WAL_PATH, "rb");
    assert(race->file.file != NULL);
    int rc = otsardb_wal_publisher_sync_reader(WAL_PATH,
                                             (uint64_t)wal_size,
                                             race_read_at,
                                             race,
                                             NULL,
                                             NULL,
                                             publisher);
    fclose(race->file.file);
    race->file.file = NULL;
    return rc;
}

/* Shared setup: fresh store + publisher, genesis committed (generation 1).
 * Returns the trigger offset of the child commit's first frame. */
static uint64_t race_scenario_setup(otsardb_object_store **out_store,
                                    otsardb_wal_publisher *publisher,
                                    const char *root,
                                    const char *database_id,
                                    size_t *out_sync1_size,
                                    size_t *out_full_size) {
    otsardb_unlink(WAL_PATH);
    *out_store = otsardb_test_memory_store_create();
    assert(*out_store != NULL);
    assert(otsardb_wal_publisher_init(publisher,
                                    *out_store,
                                    root,
                                    database_id,
                                    OTSARDB_WRITE_STRATEGY_DEFAULT));
    assert(publisher->strategy == OTSARDB_WRITE_STRATEGY_CAS);
    build_synthetic_wal(0x5157u, 0x5172u, out_sync1_size, out_full_size);

    wal_file_reader reader = {NULL};
    reader.file = fopen(WAL_PATH, "rb");
    assert(reader.file != NULL);
    assert(otsardb_wal_publisher_sync_reader(WAL_PATH,
                                           (uint64_t)*out_sync1_size,
                                           wal_file_read_at,
                                           &reader,
                                           NULL,
                                           NULL,
                                           publisher) == SQLITE_OK);
    fclose(reader.file);
    expect_head_generation(*out_store, root, 1);
    assert(publisher->current_generation == 1);
    assert(publisher->head_loaded == 1);

    size_t frame_size = (size_t)OTSARDB_SQLITE_WAL_FRAME_HEADER_SIZE + TEST_PAGE_SIZE;
    return (uint64_t)OTSARDB_SQLITE_WAL_HEADER_SIZE + 3u * (uint64_t)frame_size;
}

/* S1: the injected winner touches pages 7-8, which OVERLAP the loser's
 * child pages 7-9 -> genuine page-overlap conflict -> SQLITE_BUSY_SNAPSHOT
 * (extended 517), flag set, HEAD advanced only to the winner. */
static void test_same_generation_overlap(void) {
    const char *root = "ext-error/s1";
    const char *db_id = "ext-s1";
    const char *winner_id = "ext-s1-winner";
    otsardb_object_store *store = NULL;
    otsardb_wal_publisher publisher;
    size_t sync1_size = 0;
    size_t full_size = 0;
    uint64_t trigger = race_scenario_setup(&store, &publisher, root, db_id,
                                           &sync1_size, &full_size);

    const uint32_t winner_pages[] = {7, 8}; /* overlaps loser {7,8,9} */
    race_reader race;
    memset(&race, 0, sizeof(race));
    race.store = store;
    race.root = root;
    race.database_id = db_id;
    race.winner_id = winner_id;
    race.parent_commit_id = publisher.current_commit_id;
    race.parent_sha = publisher.current_manifest_sha256;
    race.parent_etag = publisher.current_head_etag;
    race.winner_pages = winner_pages;
    race.winner_page_count = 2;
    race.trigger_offset = trigger;

    int rc = drive_race_sync(full_size, &race, &publisher);
    fprintf(stderr,
            "S1: race sync rc=%d (expect SQLITE_BUSY_SNAPSHOT=%d) "
            "extended_conflict=%d fired=%d\n",
            rc, SQLITE_BUSY_SNAPSHOT, publisher.sqlite_extended_conflict,
            race.fired);
    assert(race.fired == 1);
    assert(rc == SQLITE_BUSY_SNAPSHOT); /* 517; pre-0030 code returned 5 */
    assert((rc & 0xff) == SQLITE_BUSY);
    assert(publisher.sqlite_extended_conflict == 1);

    /* HEAD advanced exactly to the winner; the loser's commit is NOT acked. */
    otsardb_head head = read_head(store, root);
    assert(head.generation == 2);
    assert(strcmp(head.commit_id, winner_id) == 0);
    /* genesis + winner + the loser's orphan manifest (PUT before the CAS). */
    assert(count_manifests(store, root) == 3);

    otsardb_test_memory_store_destroy(store);
    otsardb_unlink(WAL_PATH);
}

/* S2: the injected winner touches pages 11-12, DISJOINT from the loser's
 * pages 7-9 -> plain SQLITE_BUSY (5), flag clear. */
static void test_same_generation_disjoint(void) {
    const char *root = "ext-error/s2";
    const char *db_id = "ext-s2";
    const char *winner_id = "ext-s2-winner";
    otsardb_object_store *store = NULL;
    otsardb_wal_publisher publisher;
    size_t sync1_size = 0;
    size_t full_size = 0;
    uint64_t trigger = race_scenario_setup(&store, &publisher, root, db_id,
                                           &sync1_size, &full_size);

    const uint32_t winner_pages[] = {11, 12}; /* disjoint from loser {7,8,9} */
    race_reader race;
    memset(&race, 0, sizeof(race));
    race.store = store;
    race.root = root;
    race.database_id = db_id;
    race.winner_id = winner_id;
    race.parent_commit_id = publisher.current_commit_id;
    race.parent_sha = publisher.current_manifest_sha256;
    race.parent_etag = publisher.current_head_etag;
    race.winner_pages = winner_pages;
    race.winner_page_count = 2;
    race.trigger_offset = trigger;

    int rc = drive_race_sync(full_size, &race, &publisher);
    fprintf(stderr,
            "S2: race sync rc=%d (expect SQLITE_BUSY=%d) "
            "extended_conflict=%d fired=%d\n",
            rc, SQLITE_BUSY, publisher.sqlite_extended_conflict, race.fired);
    assert(race.fired == 1);
    assert(rc == SQLITE_BUSY); /* 5 */
    assert(publisher.sqlite_extended_conflict == 0);

    otsardb_head head = read_head(store, root);
    assert(head.generation == 2);
    assert(strcmp(head.commit_id, winner_id) == 0);
    assert(count_manifests(store, root) == 3);

    otsardb_test_memory_store_destroy(store);
    otsardb_unlink(WAL_PATH);
}

/* ---- S3/S4: the physical path - extended code through a real connection ----
 *
 * A real sqlite3 connection (otsardb_open on the local VFS, which enables
 * sqlite3_extended_result_codes) commits against a store where a FOREIGN
 * chain already exists: gen 1 seeded directly, plus a winning gen 2.
 *
 * Connection-flow mechanics (observed during test development, recorded
 * here): the first WAL write of the connection (the BEGIN of the first
 * transaction) creates the WAL file and produces a header-only sync whose
 * refresh consumes the FIRST store GET after arming. The scenarios
 * therefore run a WARMUP transaction first (real refresh, chains the
 * loser's schema commit onto the foreign head), and arm the fault only
 * for the target transaction - its refresh is then the first store GET.
 *
 * S3 (overlap -> 517): the target refresh is faulted to NOT_FOUND, so the
 * sync starts "headless" and the loser's create-CAS fails against the
 * existing foreign head (a real 412). The 0030 walk classifies the
 * interleaved chain - which includes the WINNING gen 2 with page 2, which
 * OVERLAPS the loser's INSERT page 2 - and the xSync hook returns 517.
 * The application observes it via sqlite3_extended_errcode().
 *
 * S4 (disjoint -> 5): the target's HEAD CAS PUT is faulted with
 * PRECONDITION_FAILED (the real 412 the store produces when the etag is
 * stale). The walk then finds the request based on the CURRENT head -
 * nothing interleaved, no page overlap -> plain SQLITE_BUSY (5). */

typedef struct physical_scenario {
    otsardb_object_store *inner;
    otsardb_object_store *store;
    otsardb_wal_publisher publisher;
    const char *root;
    const char *db_id;
    const char *db_path;
    const char *wal_path;
    const char *shm_path;
} physical_scenario;

static void physical_setup(physical_scenario *sc,
                           const char *root,
                           const char *db_id,
                           const char *db_path) {
    memset(sc, 0, sizeof(*sc));
    sc->root = root;
    sc->db_id = db_id;
    sc->db_path = db_path;
    sc->wal_path = malloc(strlen(db_path) + 5u);
    assert(sc->wal_path != NULL);
    sprintf((char *)sc->wal_path, "%s-wal", db_path);
    sc->shm_path = malloc(strlen(db_path) + 5u);
    assert(sc->shm_path != NULL);
    sprintf((char *)sc->shm_path, "%s-shm", db_path);
    otsardb_unlink(db_path);
    otsardb_unlink(sc->wal_path);
    otsardb_unlink(sc->shm_path);

    sc->inner = otsardb_test_memory_store_create();
    assert(sc->inner != NULL);
    sc->store = otsardb_test_fault_store_create(sc->inner);
    assert(sc->store != NULL);
    assert(otsardb_wal_publisher_init(&sc->publisher,
                                    sc->store,
                                    root,
                                    db_id,
                                    OTSARDB_WRITE_STRATEGY_DEFAULT));
    assert(sc->publisher.strategy == OTSARDB_WRITE_STRATEGY_CAS);
    otsardb_local_vfs_set_wal_reader_sync_hook(otsardb_wal_publisher_sync_reader,
                                             &sc->publisher);
}

static void physical_teardown(physical_scenario *sc) {
    otsardb_local_vfs_set_wal_reader_sync_hook(NULL, NULL);
    otsardb_test_fault_store_destroy(sc->store);
    otsardb_test_memory_store_destroy(sc->inner);
    otsardb_unlink(sc->db_path);
    otsardb_unlink(sc->wal_path);
    otsardb_unlink(sc->shm_path);
    free((void *)sc->wal_path);
    free((void *)sc->shm_path);
}

/* S3: foreign gen 2 touches page 2 - OVERLAP with the loser's INSERT page
 * 2 -> 517 observable through sqlite3_extended_errcode(). */
static void test_real_connection_overlap(void) {
    assert(otsardb_initialize() == OTSARDB_OK);
    physical_scenario sc;
    physical_setup(&sc, "ext-error/s3", "ext-s3", "otsardb-extended-error-s3.db");

    /* Foreign chain: genesis pages 50-51, winner pages {2} (overlap). */
    otsardb_commit_result gen1 = publish_foreign(sc.store, sc.root, sc.db_id,
                                               "ext-s3-gen1", 0, NULL, NULL, NULL, 1,
                                               (const uint32_t[]){50, 51}, 2);
    assert(gen1.head.generation == 1);
    publish_foreign(sc.store, sc.root, sc.db_id,
                    "ext-s3-winner", 1, "ext-s3-gen1",
                    gen1.head.commit_sha256, gen1.head_etag, 0,
                    (const uint32_t[]){2}, 1);

    otsardb *db = NULL;
    assert(otsardb_open(sc.db_path, &db) == OTSARDB_OK);
    char *error_message = NULL;
    assert(otsardb_exec(db,
                      "PRAGMA journal_mode=WAL;"
                      "PRAGMA wal_autocheckpoint=0;",
                      NULL,
                      NULL,
                      &error_message) == OTSARDB_OK);
    otsardb_free(error_message);
    error_message = NULL;

    /* Warmup: creates the WAL file + the loser's schema commit (chains onto
     * the foreign head at generation 3). No fault armed yet. */
    assert(otsardb_exec(db,
                      "BEGIN;"
                      "CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT);"
                      "COMMIT;",
                      NULL,
                      NULL,
                      &error_message) == OTSARDB_OK);
    otsardb_free(error_message);
    error_message = NULL;
    assert(sc.publisher.current_generation == 3);
    char warmup_commit_id[OTSARDB_COMMIT_ID_MAX + 1];
    snprintf(warmup_commit_id, sizeof(warmup_commit_id), "%s",
             sc.publisher.current_commit_id);
    assert(warmup_commit_id[0] != '\0');

    /* The target refresh sees "no head" (faulted); its create-CAS then
     * collides with the real foreign chain -> CONFLICT -> 0030 walk, which
     * finds the winner's page 2 overlapping the INSERT's page 2. */
    otsardb_test_fault_store_fail_get(sc.store, 1, OTSARDB_STORE_NOT_FOUND);

    int rc = otsardb_exec(db,
                        "BEGIN;"
                        "INSERT INTO t(v) VALUES('loser');"
                        "COMMIT;",
                        NULL,
                        NULL,
                        &error_message);
    otsardb_free(error_message);
    sqlite3 *handle = otsardb_internal_sqlite_handle(db);
    int extended = sqlite3_extended_errcode(handle);
    int base = sqlite3_errcode(handle);
    fprintf(stderr,
            "S3: commit rc=%d sqlite3_extended_errcode=%d sqlite3_errcode=%d "
            "(expect %d / %d) extended_conflict=%d\n",
            rc, extended, base, SQLITE_BUSY_SNAPSHOT, SQLITE_BUSY,
            sc.publisher.sqlite_extended_conflict);
    assert(rc == OTSARDB_ERROR);
    assert(extended == SQLITE_BUSY_SNAPSHOT); /* 517 at the app boundary */
    assert((base & 0xff) == SQLITE_BUSY);        /* primary code 5 */
    assert(sc.publisher.sqlite_extended_conflict == 1);

    /* HEAD stayed at the warmup commit (gen 3): the INSERT was NOT acked.
     * The failed attempt's orphan manifest exists next to the chain. */
    otsardb_head head = read_head(sc.store, sc.root);
    assert(head.generation == 3);
    assert(strcmp(head.commit_id, warmup_commit_id) == 0);
    assert(count_manifests(sc.store, sc.root) == 4);

    otsardb_close(db);
    physical_teardown(&sc);
}

/* S4: the HEAD CAS of the loser's INSERT is faulted with PRECONDITION_FAILED
 * (the real 412 a stale etag produces). The walk classifies against the
 * CURRENT head - nothing interleaved -> plain SQLITE_BUSY (5) observable
 * through sqlite3_extended_errcode(). */
static void test_real_connection_disjoint(void) {
    assert(otsardb_initialize() == OTSARDB_OK);
    physical_scenario sc;
    physical_setup(&sc, "ext-error/s4", "ext-s4", "otsardb-extended-error-s4.db");

    otsardb_commit_result gen1 = publish_foreign(sc.store, sc.root, sc.db_id,
                                               "ext-s4-gen1", 0, NULL, NULL, NULL, 1,
                                               (const uint32_t[]){50, 51}, 2);
    assert(gen1.head.generation == 1);
    publish_foreign(sc.store, sc.root, sc.db_id,
                    "ext-s4-winner", 1, "ext-s4-gen1",
                    gen1.head.commit_sha256, gen1.head_etag, 0,
                    (const uint32_t[]){60}, 1);

    otsardb *db = NULL;
    assert(otsardb_open(sc.db_path, &db) == OTSARDB_OK);
    char *error_message = NULL;
    assert(otsardb_exec(db,
                      "PRAGMA journal_mode=WAL;"
                      "PRAGMA wal_autocheckpoint=0;",
                      NULL,
                      NULL,
                      &error_message) == OTSARDB_OK);
    otsardb_free(error_message);
    error_message = NULL;

    assert(otsardb_exec(db,
                      "BEGIN;"
                      "CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT);"
                      "COMMIT;",
                      NULL,
                      NULL,
                      &error_message) == OTSARDB_OK);
    otsardb_free(error_message);
    error_message = NULL;
    assert(sc.publisher.current_generation == 3);
    char warmup_commit_id[OTSARDB_COMMIT_ID_MAX + 1];
    snprintf(warmup_commit_id, sizeof(warmup_commit_id), "%s",
             sc.publisher.current_commit_id);
    assert(warmup_commit_id[0] != '\0');

    /* The target commit's HEAD CAS PUT (segments PUT #1, manifest PUT #2,
     * HEAD CAS PUT #3) is faulted with the store's real 412 class. */
    const size_t fail_head[] = {3};
    otsardb_test_fault_store_fail_puts(sc.store, fail_head, 1,
                                     OTSARDB_STORE_PRECONDITION_FAILED, 0);

    int rc = otsardb_exec(db,
                        "BEGIN;"
                        "INSERT INTO t(v) VALUES('loser');"
                        "COMMIT;",
                        NULL,
                        NULL,
                        &error_message);
    otsardb_free(error_message);
    sqlite3 *handle = otsardb_internal_sqlite_handle(db);
    int extended = sqlite3_extended_errcode(handle);
    int base = sqlite3_errcode(handle);
    fprintf(stderr,
            "S4: commit rc=%d sqlite3_extended_errcode=%d sqlite3_errcode=%d "
            "(expect %d / %d) extended_conflict=%d\n",
            rc, extended, base, SQLITE_BUSY, SQLITE_BUSY,
            sc.publisher.sqlite_extended_conflict);
    assert(rc == OTSARDB_ERROR);
    assert(extended == SQLITE_BUSY); /* 5 */
    assert((base & 0xff) == SQLITE_BUSY);
    assert(sc.publisher.sqlite_extended_conflict == 0);

    otsardb_head head = read_head(sc.store, sc.root);
    assert(head.generation == 3);
    assert(strcmp(head.commit_id, warmup_commit_id) == 0);
    assert(count_manifests(sc.store, sc.root) == 4);

    otsardb_close(db);
    physical_teardown(&sc);
}

int main(void) {
    test_same_generation_overlap();
    test_same_generation_disjoint();
    test_real_connection_overlap();
    test_real_connection_disjoint();

    puts("extended error codes through the publisher sync hook "
         "(BUSY_SNAPSHOT 517 on page overlap, BUSY 5 on disjoint) passed");
    return 0;
}

