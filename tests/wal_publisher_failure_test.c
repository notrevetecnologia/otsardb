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
    const char *db_path = "otsardb-publisher-failure.db";
    const char *wal_path = "otsardb-publisher-failure.db-wal";
    const char *shm_path = "otsardb-publisher-failure.db-shm";
    const char *database_root = "failure/orders";

    /* this test asserts the FAIL-FAST contract on a single
     * injected transient store error, so the bounded retry (default 3
     * attempts) must be disabled for the injection window. */
#if defined(_WIN32)
    _putenv_s("OTSARDB_PUBLISH_RETRIES", "1");
    _putenv_s("OTSARDB_PUBLISH_RETRY_BASE_MS", "0");
#else
    setenv("OTSARDB_PUBLISH_RETRIES", "1", 1);
    setenv("OTSARDB_PUBLISH_RETRY_BASE_MS", "0", 1);
#endif

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
                                    "failure-db",
                                    OTSARDB_WRITE_STRATEGY_DEFAULT));
    otsardb_local_vfs_set_wal_reader_sync_hook(otsardb_wal_publisher_sync_reader, &publisher);

    otsardb *db = NULL;
    assert(otsardb_open(db_path, &db) == OTSARDB_OK);
    char *error_message = NULL;
    assert(otsardb_exec(db,
                      "PRAGMA journal_mode=WAL;"
                      "PRAGMA wal_autocheckpoint=0;"
                      "CREATE TABLE items(id INTEGER PRIMARY KEY, value TEXT);"
                      "INSERT INTO items(value) VALUES('committed');",
                      NULL,
                      NULL,
                      &error_message) == OTSARDB_OK);
    otsardb_free(error_message);
    error_message = NULL;

    uint64_t baseline_generation = head_generation(inner, database_root);
    assert(baseline_generation == 2);

    otsardb_test_fault_store_fail_put(fault, 2, OTSARDB_STORE_ERROR, 0);
    int rc = otsardb_exec(db,
                        "INSERT INTO items(value) VALUES('must-not-commit');",
                        NULL,
                        NULL,
                        &error_message);
    assert(rc == OTSARDB_ERROR);
    otsardb_free(error_message);
    assert(head_generation(inner, database_root) == baseline_generation);

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
    assert(count == 1);
    otsardb_close(db);

    otsardb_unlink(db_path);
    otsardb_unlink(wal_path);
    otsardb_unlink(shm_path);
    otsardb_test_fault_store_destroy(fault);
    otsardb_test_memory_store_destroy(inner);

    puts("incremental remote publication failure blocked SQL commit and HEAD advance");
    return 0;
}
