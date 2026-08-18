#include "otsardb.h"
#include "fault_store.h"
#include "journal.h"
#include "local_vfs.h"
#include "memory_store.h"
#include "wal_publisher.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(_WIN32)
#include <io.h>
#define otsardb_unlink _unlink
#else
#include <unistd.h>
#define otsardb_unlink unlink
#endif

static int collect_count(void *ctx, int argc, char **argv, char **columns) {
    (void)columns;
    if (argc != 1 || !argv || !argv[0]) return 1;
    *(int *)ctx = atoi(argv[0]);
    return 0;
}

static uint64_t head_generation(otsardb_object_store *store, const char *root) {
    otsardb_head head;
    char *etag = NULL;
    assert(otsardb_journal_read_head(store, root, &head, &etag) == OTSARDB_JOURNAL_OK);
    free(etag);
    return head.generation;
}

int main(void) {
    const char *db_path = "otsardb-publisher-ambiguous.db";
    const char *wal_path = "otsardb-publisher-ambiguous.db-wal";
    const char *shm_path = "otsardb-publisher-ambiguous.db-shm";
    const char *database_root = "ambiguous/orders";

    otsardb_unlink(db_path);
    otsardb_unlink(wal_path);
    otsardb_unlink(shm_path);
    assert(otsardb_initialize() == OTSARDB_OK);

    otsardb_object_store *inner = otsardb_test_memory_store_create();
    assert(inner != NULL);
    otsardb_object_store *fault = otsardb_test_fault_store_create(inner);
    assert(fault != NULL);

    otsardb_wal_publisher publisher;
    assert(otsardb_wal_publisher_init(&publisher,
                                    fault,
                                    database_root,
                                    "ambiguous-db",
                                    OTSARDB_WRITE_STRATEGY_DEFAULT));
    otsardb_local_vfs_set_wal_reader_sync_hook(otsardb_wal_publisher_sync_reader, &publisher);

    otsardb *db = NULL;
    assert(otsardb_open(db_path, &db) == OTSARDB_OK);
    char *error_message = NULL;
    assert(otsardb_exec(db,
                      "PRAGMA journal_mode=WAL;"
                      "PRAGMA wal_autocheckpoint=0;"
                      "CREATE TABLE items(id INTEGER PRIMARY KEY, value TEXT);"
                      "INSERT INTO items(value) VALUES('one');",
                      NULL,
                      NULL,
                      &error_message) == OTSARDB_OK);
    otsardb_free(error_message);
    error_message = NULL;

    uint64_t baseline_generation = head_generation(inner, database_root);
    assert(baseline_generation == 2);

    otsardb_test_fault_store_fail_put(fault, 3, OTSARDB_STORE_TIMEOUT, 1);
    assert(otsardb_exec(db,
                      "INSERT INTO items(value) VALUES('two');",
                      NULL,
                      NULL,
                      &error_message) == OTSARDB_OK);
    otsardb_free(error_message);
    assert(head_generation(inner, database_root) == baseline_generation + 1u);

    otsardb_test_fault_store_clear(fault);
    otsardb_local_vfs_set_wal_reader_sync_hook(NULL, NULL);
    otsardb_close(db);

    db = NULL;
    assert(otsardb_open(db_path, &db) == OTSARDB_OK);
    int count = -1;
    error_message = NULL;
    assert(otsardb_exec(db,
                      "SELECT count(*) FROM items;",
                      collect_count,
                      &count,
                      &error_message) == OTSARDB_OK);
    otsardb_free(error_message);
    assert(count == 2);
    otsardb_close(db);

    otsardb_unlink(db_path);
    otsardb_unlink(wal_path);
    otsardb_unlink(shm_path);
    otsardb_test_fault_store_destroy(fault);
    otsardb_test_memory_store_destroy(inner);

    puts("incremental ambiguous HEAD timeout resolved as committed SQL transaction");
    return 0;
}
