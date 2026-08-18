#include "otsardb.h"
#include "local_vfs.h"
#include "memory_store.h"
#include "object_store.h"
#include "recovery.h"
#include "wal_publisher.h"

#include <assert.h>
#include <stdio.h>
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
    snprintf((char *)ctx, 64, "%s", argv[0]);
    return 0;
}

int main(void) {
    const char *source_path = "otsardb-core-only-source.db";
    const char *wal_path = "otsardb-core-only-source.db-wal";
    const char *shm_path = "otsardb-core-only-source.db-shm";
    const char *recovered_path = "otsardb-core-only-recovered.db";
    const char *root = "core-only/database";

    otsardb_unlink(source_path);
    otsardb_unlink(wal_path);
    otsardb_unlink(shm_path);
    otsardb_unlink(recovered_path);
    assert(otsardb_initialize() == OTSARDB_OK);

    const otsardb_store_capabilities core_caps =
        OTSARDB_STORE_CAP_ETAG |
        OTSARDB_STORE_CAP_RANGE_GET |
        OTSARDB_STORE_CAP_READ_AFTER_WRITE;
    otsardb_object_store *store = otsardb_test_memory_store_create_with_capabilities(core_caps);
    assert(store != NULL);

    otsardb_wal_publisher publisher;
    assert(otsardb_wal_publisher_init(&publisher,
                                    store,
                                    root,
                                    "core-only-db",
                                    OTSARDB_WRITE_STRATEGY_DEFAULT));
    assert(publisher.strategy == OTSARDB_WRITE_STRATEGY_SINGLE_WRITER);
    otsardb_local_vfs_set_wal_reader_sync_hook(otsardb_wal_publisher_sync_reader, &publisher);

    otsardb *db = NULL;
    assert(otsardb_open(source_path, &db) == OTSARDB_OK);
    char *error_message = NULL;
    assert(otsardb_exec(db,
                      "PRAGMA journal_mode=WAL;"
                      "PRAGMA wal_autocheckpoint=0;"
                      "CREATE TABLE t(value TEXT);"
                      "INSERT INTO t(value) VALUES('core-only-ok');",
                      NULL,
                      NULL,
                      &error_message) == OTSARDB_OK);
    otsardb_free(error_message);
    assert(publisher.wal_bytes_read_total > 0);

    otsardb_local_vfs_set_wal_reader_sync_hook(NULL, NULL);
    otsardb_close(db);
    otsardb_unlink(source_path);
    otsardb_unlink(wal_path);
    otsardb_unlink(shm_path);

    assert(otsardb_recovery_restore_store(store, root, recovered_path));
    assert(otsardb_open(recovered_path, &db) == OTSARDB_OK);
    char value[64] = {0};
    error_message = NULL;
    assert(otsardb_exec(db,
                      "SELECT value FROM t;",
                      collect_text,
                      value,
                      &error_message) == OTSARDB_OK);
    otsardb_free(error_message);
    assert(strcmp(value, "core-only-ok") == 0);
    otsardb_close(db);

    otsardb_unlink(recovered_path);
    otsardb_test_memory_store_destroy(store);

    puts("incremental S3 Core SINGLE_WRITER WAL publication and recovery passed");
    return 0;
}
