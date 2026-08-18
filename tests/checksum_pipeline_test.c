#include "otsardb.h"
#include "checksum.h"
#include "commit_manifest.h"
#include "journal.h"
#include "local_vfs.h"
#include "memory_store.h"
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

static int collect_text(void *ctx, int argc, char **argv, char **columns) {
    (void)columns;
    if (argc != 1 || !argv || !argv[0]) return 1;
    snprintf((char *)ctx, 128, "%s", argv[0]);
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
    const char *source_path = "otsardb-csum-source.db";
    const char *wal_path = "otsardb-csum-source.db-wal";
    const char *shm_path = "otsardb-csum-source.db-shm";
    const char *recovered_path = "otsardb-csum-recovered.db";
    const char *database_root = "csum/orders";
    const char *database_id = "csum-orders";

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
                      "CREATE TABLE items(id INTEGER PRIMARY KEY, value TEXT);"
                      "INSERT INTO items(value) VALUES('alpha');"
                      "INSERT INTO items(value) VALUES('beta');"
                      "INSERT INTO items(value) VALUES('gamma');",
                      NULL,
                      NULL,
                      &error_message) == OTSARDB_OK);
    otsardb_free(error_message);
    error_message = NULL;

    otsardb_head head = read_head(store, database_root);
    assert(head.generation >= 2);

    /* The manifests published through the VFS path must carry the rolling
     * post-apply checksum (db reader is available). */
    otsardb_store_object manifest_object = {0};
    assert(otsardb_store_get(store, head.commit_key, &manifest_object) == OTSARDB_STORE_OK);
    otsardb_commit_manifest manifest;
    assert(otsardb_commit_manifest_decode_json((const char *)manifest_object.data,
                                             manifest_object.size,
                                             &manifest));
    assert(manifest.post_apply_checksum[0] != '\0');
    assert(strlen(manifest.post_apply_checksum) == 16);
    otsardb_commit_manifest_free(&manifest);
    otsardb_store_release_object(store, &manifest_object);

    otsardb_local_vfs_set_wal_reader_sync_hook(NULL, NULL);
    otsardb_close(db);

    otsardb_unlink(source_path);
    otsardb_unlink(wal_path);
    otsardb_unlink(shm_path);
    otsardb_unlink(recovered_path);

    /* Recovery must validate the checksum and succeed on a healthy chain. */
    assert(otsardb_recovery_restore_store(store, database_root, recovered_path));
    otsardb *recovered = NULL;
    assert(otsardb_open(recovered_path, &recovered) == OTSARDB_OK);
    char values[128] = {0};
    error_message = NULL;
    assert(otsardb_exec(recovered,
                      "SELECT group_concat(value, ',') "
                      "FROM (SELECT value FROM items ORDER BY id);",
                      collect_text,
                      values,
                      &error_message) == OTSARDB_OK);
    otsardb_free(error_message);
    assert(strcmp(values, "alpha,beta,gamma") == 0);
    otsardb_close(recovered);
    otsardb_unlink(recovered_path);

    /* Tamper with the HEAD manifest's checksum AND recompute the manifest
     * hash consistently (so the hash chain still validates) — recovery must
     * reject via the rolling-checksum check specifically. */
    otsardb_store_object head_object = {0};
    assert(otsardb_store_get(store, head.commit_key, &head_object) == OTSARDB_STORE_OK);
    size_t head_object_size = head_object.size;
    unsigned char *tampered = (unsigned char *)malloc(head_object_size + 1u);
    assert(tampered != NULL);
    memcpy(tampered, head_object.data, head_object_size);
    char *csum_at = strstr((char *)tampered, "\"post_apply_checksum\":\"");
    assert(csum_at != NULL);
    char *hex_start = csum_at + strlen("\"post_apply_checksum\":\"");
    hex_start[0] = (hex_start[0] == '0') ? '1' : '0';
    otsardb_store_release_object(store, &head_object);

    char tampered_sha[OTSARDB_MANIFEST_SHA256_HEX_SIZE + 1];
    assert(otsardb_sha256_hex_data(tampered, head_object_size, tampered_sha));

    otsardb_store_put_options overwrite = {0};
    assert(otsardb_store_put(store,
                           head.commit_key,
                           tampered,
                           head_object_size,
                           &overwrite,
                           NULL) == OTSARDB_STORE_OK);
    free(tampered);

    /* Rewrite HEAD to point at the tampered manifest hash so the chain walk
     * passes and only the checksum check can reject. */
    otsardb_head tampered_head = head;
    snprintf(tampered_head.commit_sha256,
             sizeof(tampered_head.commit_sha256),
             "%s",
             tampered_sha);
    char *head_json = NULL;
    size_t head_size = 0;
    assert(otsardb_head_encode_json(&tampered_head, &head_json, &head_size));
    otsardb_store_put_options head_put = {0};
    assert(otsardb_store_put(store,
                           "csum/orders/HEAD.json",
                           head_json,
                           head_size,
                           &head_put,
                           NULL) == OTSARDB_STORE_OK);
    free(head_json);

    otsardb_unlink(recovered_path);
    assert(!otsardb_recovery_restore_store(store, database_root, recovered_path));
    otsardb_unlink(recovered_path);

    otsardb_test_memory_store_destroy(store);

    puts("rolling checksum pipeline and tamper rejection passed");
    return 0;
}
