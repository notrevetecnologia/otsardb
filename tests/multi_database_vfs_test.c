#include "otsardb.h"
#include "otsardb_internal.h"
#include "journal.h"
#include "local_vfs.h"
#include "memory_store.h"
#include "recovery.h"
#include "wal_publisher.h"

#include <assert.h>
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

static void remove_db_files(const char *path) {
    char sidecar[256];
    otsardb_unlink(path);
    snprintf(sidecar, sizeof(sidecar), "%s-wal", path);
    otsardb_unlink(sidecar);
    snprintf(sidecar, sizeof(sidecar), "%s-shm", path);
    otsardb_unlink(sidecar);
}

static otsardb_head read_head(otsardb_object_store *store, const char *root) {
    otsardb_head head;
    char *etag = NULL;
    assert(otsardb_journal_read_head(store, root, &head, &etag) == OTSARDB_JOURNAL_OK);
    free(etag);
    return head;
}

static int collect_text(void *ctx, int argc, char **argv, char **columns) {
    (void)columns;
    if (argc != 1 || !argv || !argv[0]) return 1;
    snprintf((char *)ctx, 128, "%s", argv[0]);
    return 0;
}

static void exec_ok(otsardb *db, const char *sql) {
    char *error_message = NULL;
    assert(otsardb_exec(db, sql, NULL, NULL, &error_message) == OTSARDB_OK);
    otsardb_free(error_message);
}

int main(void) {
    const char *path_a = "otsardb-multi-a.db";
    const char *path_b = "otsardb-multi-b.db";
    const char *restore_a = "otsardb-multi-a-restored.db";
    const char *restore_b = "otsardb-multi-b-restored.db";
    const char *root_a = "multi/database-a";
    const char *root_b = "multi/database-b";

    remove_db_files(path_a);
    remove_db_files(path_b);
    remove_db_files(restore_a);
    remove_db_files(restore_b);

    assert(otsardb_initialize() == OTSARDB_OK);

    otsardb_object_store *store_a = otsardb_test_memory_store_create();
    otsardb_object_store *store_b = otsardb_test_memory_store_create();
    assert(store_a && store_b);

    otsardb_wal_publisher publisher_a;
    otsardb_wal_publisher publisher_b;
    assert(otsardb_wal_publisher_init(&publisher_a,
                                    store_a,
                                    root_a,
                                    "database-a",
                                    OTSARDB_WRITE_STRATEGY_DEFAULT));
    assert(otsardb_wal_publisher_init(&publisher_b,
                                    store_b,
                                    root_b,
                                    "database-b",
                                    OTSARDB_WRITE_STRATEGY_DEFAULT));

    otsardb_local_vfs_instance *vfs_a = NULL;
    otsardb_local_vfs_instance *vfs_b = NULL;
    assert(otsardb_local_vfs_instance_create(otsardb_wal_publisher_sync_reader,
                                           &publisher_a,
                                           &vfs_a) == 0);
    assert(otsardb_local_vfs_instance_create(otsardb_wal_publisher_sync_reader,
                                           &publisher_b,
                                           &vfs_b) == 0);
    assert(strcmp(otsardb_local_vfs_instance_name(vfs_a),
                  otsardb_local_vfs_instance_name(vfs_b)) != 0);

    otsardb *db_a = NULL;
    otsardb *db_b = NULL;
    assert(otsardb_open_with_vfs(path_a, otsardb_local_vfs_instance_name(vfs_a), &db_a) == OTSARDB_OK);
    assert(otsardb_open_with_vfs(path_b, otsardb_local_vfs_instance_name(vfs_b), &db_b) == OTSARDB_OK);

    exec_ok(db_a, "PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0;");
    exec_ok(db_b, "PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0;");

    exec_ok(db_a, "CREATE TABLE items(value TEXT);");
    exec_ok(db_b, "CREATE TABLE items(value TEXT);");
    exec_ok(db_a, "INSERT INTO items VALUES('a1');");
    exec_ok(db_b, "INSERT INTO items VALUES('b1');");

    otsardb_head head_a = read_head(store_a, root_a);
    otsardb_head head_b = read_head(store_b, root_b);
    assert(head_a.generation == 2);
    assert(head_b.generation == 2);
    assert(strcmp(head_a.database_id, "database-a") == 0);
    assert(strcmp(head_b.database_id, "database-b") == 0);

    /* Closing and tearing down A must not disturb B's VFS/publisher. */
    otsardb_close(db_a);
    db_a = NULL;
    otsardb_local_vfs_instance_destroy(vfs_a);
    vfs_a = NULL;

    exec_ok(db_b, "INSERT INTO items VALUES('b2');");
    otsardb_head head_b_after = read_head(store_b, root_b);
    assert(head_b_after.generation == 3);
    assert(read_head(store_a, root_a).generation == 2);

    otsardb_close(db_b);
    db_b = NULL;
    otsardb_local_vfs_instance_destroy(vfs_b);
    vfs_b = NULL;

    remove_db_files(path_a);
    remove_db_files(path_b);

    assert(otsardb_recovery_restore_store(store_a, root_a, restore_a));
    assert(otsardb_recovery_restore_store(store_b, root_b, restore_b));

    assert(otsardb_open(restore_a, &db_a) == OTSARDB_OK);
    assert(otsardb_open(restore_b, &db_b) == OTSARDB_OK);

    char values_a[128] = {0};
    char values_b[128] = {0};
    char *error_message = NULL;
    assert(otsardb_exec(db_a,
                      "SELECT group_concat(value, ',') FROM items;",
                      collect_text,
                      values_a,
                      &error_message) == OTSARDB_OK);
    otsardb_free(error_message);
    error_message = NULL;
    assert(otsardb_exec(db_b,
                      "SELECT group_concat(value, ',') FROM (SELECT value FROM items ORDER BY rowid);",
                      collect_text,
                      values_b,
                      &error_message) == OTSARDB_OK);
    otsardb_free(error_message);

    assert(strcmp(values_a, "a1") == 0);
    assert(strcmp(values_b, "b1,b2") == 0);

    otsardb_close(db_a);
    otsardb_close(db_b);
    remove_db_files(restore_a);
    remove_db_files(restore_b);
    otsardb_test_memory_store_destroy(store_a);
    otsardb_test_memory_store_destroy(store_b);

    puts("per-database VFS durability isolation and independent recovery passed");
    return 0;
}
