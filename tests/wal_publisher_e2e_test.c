#include "otsardb.h"
#include "commit_manifest.h"
#include "journal.h"
#include "local_vfs.h"
#include "memory_store.h"
#include "recovery.h"
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

static int collect_text(void *ctx, int argc, char **argv, char **columns) {
    (void)columns;
    if (argc != 1 || !argv || !argv[0]) return 1;
    char *output = (char *)ctx;
    snprintf(output, 128, "%s", argv[0]);
    return 0;
}

static otsardb_head read_head(otsardb_object_store *store, const char *root) {
    otsardb_head head;
    char *etag = NULL;
    assert(otsardb_journal_read_head(store, root, &head, &etag) == OTSARDB_JOURNAL_OK);
    free(etag);
    return head;
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

static uint64_t wal_file_size(FILE *file) {
    assert(file != NULL);
    assert(fseek(file, 0, SEEK_END) == 0);
    long size = ftell(file);
    assert(size >= 0);
    return (uint64_t)size;
}

int main(void) {
    const char *source_path = "otsardb-publisher-source.db";
    const char *wal_path = "otsardb-publisher-source.db-wal";
    const char *shm_path = "otsardb-publisher-source.db-shm";
    const char *recovered_path = "otsardb-publisher-recovered.db";
    const char *tampered_recovery_path = "otsardb-publisher-tampered.db";
    const char *database_root = "integration/orders";
    const char *database_id = "orders-db";

    otsardb_unlink(source_path);
    otsardb_unlink(wal_path);
    otsardb_unlink(shm_path);
    otsardb_unlink(recovered_path);
    otsardb_unlink(tampered_recovery_path);
    assert(otsardb_initialize() == OTSARDB_OK);

    otsardb_object_store *store = otsardb_test_memory_store_create();
    assert(store != NULL);

    otsardb_wal_publisher publisher1;
    assert(otsardb_wal_publisher_init(&publisher1,
                                    store,
                                    database_root,
                                    database_id,
                                    OTSARDB_WRITE_STRATEGY_DEFAULT));
    assert(publisher1.strategy == OTSARDB_WRITE_STRATEGY_CAS);
    otsardb_local_vfs_set_wal_reader_sync_hook(otsardb_wal_publisher_sync_reader, &publisher1);

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

    assert(otsardb_exec(db,
                      "INSERT INTO items(value) VALUES('one');",
                      NULL,
                      NULL,
                      &error_message) == OTSARDB_OK);
    otsardb_free(error_message);
    error_message = NULL;

    FILE *wal = fopen(wal_path, "rb");
    assert(wal != NULL);
    uint64_t current_wal_size = wal_file_size(wal);
    assert(publisher1.wal_bytes_read_last_sync > 32);
    assert(publisher1.wal_bytes_read_last_sync < current_wal_size);

    wal_file_reader reader = {wal};
    uint64_t total_before_idle_sync = publisher1.wal_bytes_read_total;
    assert(otsardb_wal_publisher_sync_reader(wal_path,
                                           current_wal_size,
                                           wal_file_read_at,
                                           &reader,
                                           NULL,
                                           NULL,
                                           &publisher1) == 0);
    assert(publisher1.wal_bytes_read_last_sync == 32);
    assert(publisher1.wal_bytes_read_total == total_before_idle_sync + 32);
    fclose(wal);

    otsardb_head after_two = read_head(store, database_root);
    assert(after_two.generation == 2);

    assert(otsardb_exec(db,
                      "BEGIN;"
                      "INSERT INTO items(value) VALUES('rolled-back');"
                      "ROLLBACK;",
                      NULL,
                      NULL,
                      &error_message) == OTSARDB_OK);
    otsardb_free(error_message);
    error_message = NULL;
    otsardb_head after_rollback = read_head(store, database_root);
    assert(after_rollback.generation == after_two.generation);
    assert(strcmp(after_rollback.commit_id, after_two.commit_id) == 0);

    otsardb_wal_publisher publisher2;
    assert(otsardb_wal_publisher_init(&publisher2,
                                    store,
                                    database_root,
                                    database_id,
                                    OTSARDB_WRITE_STRATEGY_DEFAULT));
    otsardb_local_vfs_set_wal_reader_sync_hook(otsardb_wal_publisher_sync_reader, &publisher2);

    assert(otsardb_exec(db,
                      "BEGIN;"
                      "INSERT INTO items(value) VALUES('two');"
                      "INSERT INTO items(value) VALUES('three');"
                      "COMMIT;",
                      NULL,
                      NULL,
                      &error_message) == OTSARDB_OK);
    otsardb_free(error_message);
    error_message = NULL;

    otsardb_head after_restart = read_head(store, database_root);
    assert(after_restart.generation == 3);
    assert(strcmp(after_restart.commit_id, after_two.commit_id) != 0);

    assert(otsardb_exec(db,
                      "PRAGMA wal_checkpoint(TRUNCATE);",
                      NULL,
                      NULL,
                      &error_message) == OTSARDB_OK);
    otsardb_free(error_message);
    error_message = NULL;
    otsardb_head after_checkpoint = read_head(store, database_root);
    assert(after_checkpoint.generation == 3);
    assert(strcmp(after_checkpoint.commit_id, after_restart.commit_id) == 0);

    assert(otsardb_exec(db,
                      "INSERT INTO items(value) VALUES('four');",
                      NULL,
                      NULL,
                      &error_message) == OTSARDB_OK);
    otsardb_free(error_message);

    otsardb_head after_new_wal = read_head(store, database_root);
    assert(after_new_wal.generation == 4);
    assert(strcmp(after_new_wal.commit_id, after_checkpoint.commit_id) != 0);

    otsardb_local_vfs_set_wal_reader_sync_hook(NULL, NULL);
    otsardb_close(db);

    otsardb_unlink(source_path);
    otsardb_unlink(wal_path);
    otsardb_unlink(shm_path);
    otsardb_unlink(recovered_path);

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
    assert(strcmp(values, "one,two,three,four") == 0);
    otsardb_close(recovered);

    /* Hash-chain integrity test: alter a parent manifest by appending harmless
     * whitespace. Its JSON content still decodes to the same fields, but its
     * object hash changes. A v2 child points to the original parent hash, so a
     * second zero-local recovery must reject the modified history. */
    otsardb_store_object latest_manifest_object = {0};
    assert(otsardb_store_get(store,
                           after_new_wal.commit_key,
                           &latest_manifest_object) == OTSARDB_STORE_OK);
    otsardb_commit_manifest latest_manifest;
    assert(otsardb_commit_manifest_decode_json((const char *)latest_manifest_object.data,
                                             latest_manifest_object.size,
                                             &latest_manifest));
    assert(latest_manifest.format_version == OTSARDB_COMMIT_MANIFEST_VERSION);
    assert(latest_manifest.parent_commit_id[0] != '\0');
    assert(strlen(latest_manifest.parent_manifest_sha256) == 64);
    otsardb_commit_manifest_free(&latest_manifest);
    otsardb_store_release_object(store, &latest_manifest_object);

    char parent_manifest_key[OTSARDB_KEY_MAX + 1];
    int key_len = snprintf(parent_manifest_key,
                           sizeof(parent_manifest_key),
                           "%s/commits/%s.json",
                           database_root,
                           latest_manifest.parent_commit_id);
    assert(key_len > 0 && (size_t)key_len < sizeof(parent_manifest_key));

    otsardb_store_object parent_manifest_object = {0};
    assert(otsardb_store_get(store,
                           parent_manifest_key,
                           &parent_manifest_object) == OTSARDB_STORE_OK);
    unsigned char *tampered =
        (unsigned char *)malloc(parent_manifest_object.size + 1u);
    assert(tampered != NULL);
    memcpy(tampered, parent_manifest_object.data, parent_manifest_object.size);
    tampered[parent_manifest_object.size] = ' ';
    size_t tampered_size = parent_manifest_object.size + 1u;
    otsardb_store_release_object(store, &parent_manifest_object);

    otsardb_store_put_options overwrite = {0};
    assert(otsardb_store_put(store,
                           parent_manifest_key,
                           tampered,
                           tampered_size,
                           &overwrite,
                           NULL) == OTSARDB_STORE_OK);
    free(tampered);

    otsardb_unlink(tampered_recovery_path);
    assert(!otsardb_recovery_restore_store(store,
                                         database_root,
                                         tampered_recovery_path));
    otsardb_unlink(tampered_recovery_path);

    otsardb_unlink(recovered_path);
    otsardb_test_memory_store_destroy(store);

    puts("incremental WAL -> hash-linked object store -> zero-local recovery passed");
    return 0;
}
