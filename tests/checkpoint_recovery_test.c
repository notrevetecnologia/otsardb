#include "otsardb.h"
#include "checkpoint_build.h"
#include "checkpoint_manifest.h"
#include "journal.h"
#include "local_vfs.h"
#include "memory_store.h"
#include "recovery.h"
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

static int collect_count(void *ctx, int argc, char **argv, char **columns) {
    (void)columns;
    if (argc != 1 || !argv || !argv[0]) return 1;
    int *out = (int *)ctx;
    *out = atoi(argv[0]);
    return 0;
}

int main(void) {
    const char *source_path = "otsardb-cptest-source.db";
    const char *wal_path = "otsardb-cptest-source.db-wal";
    const char *shm_path = "otsardb-cptest-source.db-shm";
    const char *ref_path = "otsardb-cptest-ref.db";
    const char *build_path = "otsardb-cptest-build.db";
    const char *final_path = "otsardb-cptest-final.db";
    const char *database_root = "integration/cptest";
    const char *database_id = "cptest-db";

    otsardb_unlink(source_path);
    otsardb_unlink(wal_path);
    otsardb_unlink(shm_path);
    otsardb_unlink(ref_path);
    otsardb_unlink(build_path);
    otsardb_unlink(final_path);
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
                      "PRAGMA wal_autocheckpoint=0;"
                      "CREATE TABLE items(id INTEGER PRIMARY KEY, value TEXT);",
                      NULL,
                      NULL,
                      &error_message) == OTSARDB_OK);
    otsardb_free(error_message);
    error_message = NULL;

    assert(otsardb_exec(db,
                      "INSERT INTO items(id, value) "
                      "WITH RECURSIVE cnt(x) AS (VALUES(1) "
                      "UNION ALL SELECT x+1 FROM cnt WHERE x<200) "
                      "SELECT x, printf('item-%d', x) FROM cnt;",
                      NULL,
                      NULL,
                      &error_message) == OTSARDB_OK);
    otsardb_free(error_message);
    error_message = NULL;

    /* Verify HEAD exists and publisher published commits */
    otsardb_head head;
    char *head_etag = NULL;
    assert(otsardb_journal_read_head(store, database_root, &head, &head_etag) == OTSARDB_JOURNAL_OK);
    free(head_etag);
    assert(head.generation >= 1);
    assert(head.database_id[0] != '\0');

    otsardb_local_vfs_set_wal_reader_sync_hook(NULL, NULL);
    otsardb_close(db);

    /* Full-chain recovery — reference result */
    otsardb_unlink(ref_path);
    assert(otsardb_recovery_restore_store(store, database_root, ref_path));

    otsardb *ref_db = NULL;
    assert(otsardb_open(ref_path, &ref_db) == OTSARDB_OK);
    int count = 0;
    assert(otsardb_exec(ref_db,
                      "SELECT count(*) FROM items;",
                      collect_count,
                      &count,
                      &error_message) == OTSARDB_OK);
    otsardb_free(error_message);
    assert(count == 200);
    otsardb_close(ref_db);

    /* Build checkpoint */
    otsardb_unlink(build_path);
    assert(otsardb_checkpoint_build(store, database_root, build_path) == 1);

    /* Read the CHECKPOINT.json pointer */
    otsardb_checkpoint_pointer ptr;
    char *ptr_etag = NULL;
    assert(otsardb_checkpoint_ptr_read(store, database_root, &ptr, &ptr_etag)
           == OTSARDB_CHECKPOINT_PTR_OK);
    free(ptr_etag);
    assert(ptr.covered_generation == head.generation);
    assert(strlen(ptr.checkpoint_sha256) == OTSARDB_CHECKPOINT_SHA256_HEX_SIZE);

    /* Second recovery — verify no regression with checkpoint present */
    otsardb_unlink(final_path);
    assert(otsardb_recovery_restore_store(store, database_root, final_path));

    otsardb *final_db = NULL;
    assert(otsardb_open(final_path, &final_db) == OTSARDB_OK);
    count = 0;
    assert(otsardb_exec(final_db,
                      "SELECT count(*) FROM items;",
                      collect_count,
                      &count,
                      &error_message) == OTSARDB_OK);
    otsardb_free(error_message);
    assert(count == 200);
    otsardb_close(final_db);

    /* Cleanup */
    otsardb_unlink(source_path);
    otsardb_unlink(wal_path);
    otsardb_unlink(shm_path);
    otsardb_unlink(ref_path);
    otsardb_unlink(build_path);
    otsardb_unlink(final_path);
    otsardb_test_memory_store_destroy(store);

    puts("checkpoint build + recovery passthrough passed");
    return 0;
}
