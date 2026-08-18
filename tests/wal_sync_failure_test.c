#include "otsardb.h"
#include "local_vfs.h"
#include "sqlite_wal.h"
#include "sqlite3.h"

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

typedef struct sync_failure_context {
    int armed;
    int fired;
    uint32_t baseline_commits;
} sync_failure_context;

static int fail_new_commit(const char *wal_name,
                           const void *wal_data,
                           size_t wal_size,
                           void *ctx_ptr) {
    (void)wal_name;
    sync_failure_context *ctx = (sync_failure_context *)ctx_ptr;
    otsardb_sqlite_wal_scan_result scan;
    if (!otsardb_sqlite_wal_scan(wal_data, wal_size, NULL, NULL, &scan)) {
        return SQLITE_IOERR_FSYNC;
    }

    if (!ctx->armed) {
        if (scan.commit_count > ctx->baseline_commits) {
            ctx->baseline_commits = scan.commit_count;
        }
        return SQLITE_OK;
    }

    if (!ctx->fired && scan.commit_count > ctx->baseline_commits) {
        ctx->fired = 1;
        return SQLITE_IOERR_FSYNC;
    }
    return SQLITE_OK;
}

static int collect_count(void *ctx, int argc, char **argv, char **columns) {
    (void)columns;
    if (argc != 1 || !argv || !argv[0]) return 1;
    *(int *)ctx = atoi(argv[0]);
    return 0;
}

int main(void) {
    const char *db_path = "otsardb-wal-sync-failure.db";
    const char *wal_path = "otsardb-wal-sync-failure.db-wal";
    const char *shm_path = "otsardb-wal-sync-failure.db-shm";
    otsardb_unlink(db_path);
    otsardb_unlink(wal_path);
    otsardb_unlink(shm_path);

    assert(otsardb_initialize() == OTSARDB_OK);
    sync_failure_context hook = {0};
    otsardb_local_vfs_set_wal_sync_hook(fail_new_commit, &hook);

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
    assert(hook.baseline_commits > 0);

    hook.armed = 1;
    int rc = otsardb_exec(db,
                        "INSERT INTO items(value) VALUES('must-not-survive');",
                        NULL,
                        NULL,
                        &error_message);
    assert(rc == OTSARDB_ERROR);
    assert(hook.fired == 1);
    otsardb_free(error_message);

    otsardb_local_vfs_set_wal_sync_hook(NULL, NULL);
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

    puts("WAL xSync failure correctly prevented recovered commit");
    return 0;
}
