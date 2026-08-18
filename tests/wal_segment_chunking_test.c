#include "otsardb.h"
#include "commit_manifest.h"
#include "journal.h"
#include "local_vfs.h"
#include "memory_store.h"
#include "recovery.h"
#include "sha256.h"
#include "wal_batch.h"
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

/*
 * Bounded/streaming segment construction test: 
 *
 * A single very large transaction must not require transaction-sized memory.
 * The publisher splits one commit into multiple bounded .otsseg objects
 * (manifest format v4 segments array) and zero-local-state recovery must
 * reapply all of them oldest -> newest.
 *
 * The segment payload limit is forced tiny (one frame per chunk) so even a
 * modest INSERT provably spans several segments of a single commit.
 */

#define CHUNK_FRAMES 1u
#define ROWS 1500u

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

int main(void) {
    const char *source_path = "otsardb-segment-chunk-source.db";
    const char *wal_path = "otsardb-segment-chunk-source.db-wal";
    const char *shm_path = "otsardb-segment-chunk-source.db-shm";
    const char *recovered_path = "otsardb-segment-chunk-recovered.db";
    const char *database_root = "chunk/orders";
    const char *database_id = "chunk-orders";

    otsardb_unlink(source_path);
    otsardb_unlink(wal_path);
    otsardb_unlink(shm_path);
    otsardb_unlink(recovered_path);
    assert(otsardb_initialize() == OTSARDB_OK);

    otsardb_object_store *store = otsardb_test_memory_store_create();
    assert(store != NULL);

    otsardb_wal_publisher publisher;
    assert(otsardb_wal_publisher_init(&publisher,
                                    store,
                                    database_root,
                                    database_id,
                                    OTSARDB_WRITE_STRATEGY_DEFAULT));
    assert(publisher.strategy == OTSARDB_WRITE_STRATEGY_CAS);
    otsardb_wal_publisher_set_segment_payload_limit(
        &publisher,
        CHUNK_FRAMES * (uint64_t)(OTSARDB_WAL_BATCH_FRAME_HEADER_SIZE + 4096u));
    otsardb_local_vfs_set_wal_reader_sync_hook(otsardb_wal_publisher_sync_reader, &publisher);

    otsardb *db = NULL;
    assert(otsardb_open(source_path, &db) == OTSARDB_OK);
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
                      "CREATE TABLE items(id INTEGER PRIMARY KEY, value TEXT);",
                      NULL,
                      NULL,
                      &error_message) == OTSARDB_OK);
    otsardb_free(error_message);
    error_message = NULL;

    char big_insert[512];
    int sql_len = snprintf(big_insert,
                           sizeof(big_insert),
                           "WITH RECURSIVE cnt(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM cnt WHERE x < %u) "
                           "INSERT INTO items(value) SELECT 'v' || x FROM cnt;",
                           ROWS);
    assert(sql_len > 0 && (size_t)sql_len < sizeof(big_insert));

    assert(otsardb_exec(db, big_insert, NULL, NULL, &error_message) == OTSARDB_OK);
    otsardb_free(error_message);
    error_message = NULL;

    assert(publisher.wal_bytes_read_total > 0);

    /* The single statement above is ONE commit; with a one-frame chunk budget
     * it must be published as several segments of one manifest v4 commit. */
    otsardb_head head = read_head(store, database_root);
    assert(head.generation == 2); /* CREATE TABLE + big INSERT */

    otsardb_store_object manifest_object = {0};
    assert(otsardb_store_get(store, head.commit_key, &manifest_object) == OTSARDB_STORE_OK);
    otsardb_commit_manifest manifest;
    assert(otsardb_commit_manifest_decode_json((const char *)manifest_object.data,
                                             manifest_object.size,
                                             &manifest));
    assert(manifest.format_version == OTSARDB_COMMIT_MANIFEST_VERSION);
    assert(manifest.segment_count >= 2);
    for (uint32_t i = 0; i < manifest.segment_count; ++i) {
        assert(manifest.segments[i].key[0] != '\0');
        assert(strlen(manifest.segments[i].sha256) == OTSARDB_MANIFEST_SHA256_HEX_SIZE);
        assert(manifest.segments[i].size > 0);
        /* The publisher always records the per-segment page set in v4. */
        assert(manifest.segments[i].page_count > 0);
        assert(manifest.segments[i].pages != NULL);
        /* every publisher segment records the per-frame
         * block offsets (sorted pages, strictly increasing, last block ends
         * inside the object) so lazy hydration can issue byte-range GETs. */
        assert(manifest.segments[i].frame_offsets != NULL);
        uint64_t prev_offset = 0;
        for (uint32_t p = 0; p < manifest.segments[i].page_count; ++p) {
            assert(manifest.segments[i].frame_offsets[p] > prev_offset);
            prev_offset = manifest.segments[i].frame_offsets[p];
        }
        assert(prev_offset < manifest.segments[i].size);
    }
    otsardb_commit_manifest_free(&manifest);
    otsardb_store_release_object(store, &manifest_object);

    /* Every published segment of that commit must be present and content
     * verified, each carrying the commit id of the manifest. v3 segment keys
     * are suffixes relative to the database root. */
    for (uint32_t i = 0; i < manifest.segment_count; ++i) {
        char absolute_key[OTSARDB_KEY_MAX + 1];
        int key_len = snprintf(absolute_key,
                               sizeof(absolute_key),
                               "%s/%s",
                               database_root,
                               manifest.segments[i].key);
        assert(key_len > 0 && (size_t)key_len < sizeof(absolute_key));

        otsardb_store_object segment_object = {0};
        assert(otsardb_store_get(store,
                               absolute_key,
                               &segment_object) == OTSARDB_STORE_OK);
        char actual[65];
        assert(otsardb_sha256_hex_data(segment_object.data, segment_object.size, actual));
        assert(strcmp(actual, manifest.segments[i].sha256) == 0);
        assert(segment_object.size == manifest.segments[i].size);
        otsardb_store_release_object(store, &segment_object);
    }

    otsardb_local_vfs_set_wal_reader_sync_hook(NULL, NULL);
    otsardb_close(db);

    otsardb_unlink(source_path);
    otsardb_unlink(wal_path);
    otsardb_unlink(shm_path);
    otsardb_unlink(recovered_path);

    /* Zero-local-state recovery reapplies every segment of the commit. */
    assert(otsardb_recovery_restore_store(store, database_root, recovered_path));

    otsardb *recovered = NULL;
    assert(otsardb_open(recovered_path, &recovered) == OTSARDB_OK);
    char count[64] = {0};
    error_message = NULL;
    assert(otsardb_exec(recovered,
                      "SELECT count(*) FROM items;",
                      collect_text,
                      count,
                      &error_message) == OTSARDB_OK);
    otsardb_free(error_message);
    char expected[64];
    snprintf(expected, sizeof(expected), "%u", ROWS);
    assert(strcmp(count, expected) == 0);
    otsardb_close(recovered);

    otsardb_unlink(recovered_path);
    otsardb_test_memory_store_destroy(store);

    puts("bounded multi-segment commit publication and recovery passed");
    return 0;
}
