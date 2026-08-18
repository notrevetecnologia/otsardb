#include "otsardb.h"
#include "otsardb_internal.h"
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
#include <windows.h>
#else
#include <pthread.h>
#endif

#define THREAD_COUNT 8u
#define ROWS_PER_THREAD 80u

#if defined(_WIN32)
typedef HANDLE otsardb_thread_t;
static int otsardb_thread_start(otsardb_thread_t *thread, void *(*func)(void *), void *arg) {
    DWORD id;
    *thread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)func, arg, 0, &id);
    return *thread != NULL;
}
static int otsardb_thread_join(otsardb_thread_t thread) {
    DWORD rc = WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    return rc == WAIT_OBJECT_0;
}
#else
typedef pthread_t otsardb_thread_t;
static int otsardb_thread_start(otsardb_thread_t *thread, void *(*func)(void *), void *arg) {
    return pthread_create(thread, NULL, func, arg) == 0;
}
static int otsardb_thread_join(otsardb_thread_t thread) {
    return pthread_join(thread, NULL) == 0;
}
#endif

#if defined(_WIN32)
#define otsardb_unlink _unlink
#include <io.h>
#else
#define otsardb_unlink unlink
#include <unistd.h>
#endif

typedef struct thread_ctx {
    unsigned int index;
    int ok;
} thread_ctx;

static int collect_text(void *ctx, int argc, char **argv, char **columns) {
    (void)columns;
    if (argc != 1 || !argv || !argv[0]) return 1;
    snprintf((char *)ctx, 64, "%s", argv[0]);
    return 0;
}

static void *thread_work(void *arg) {
    thread_ctx *ctx = (thread_ctx *)arg;
    ctx->ok = 0;

    char root[256];
    char db_id[64];
    char db_path[256];
    char restored_path[256];
    snprintf(root, sizeof(root), "stress/db-%u", ctx->index);
    snprintf(db_id, sizeof(db_id), "stress-db-%u", ctx->index);
    snprintf(db_path, sizeof(db_path), "otsardb-stress-%u.db", ctx->index);
    snprintf(restored_path, sizeof(restored_path), "otsardb-stress-%u-restored.db", ctx->index);

    otsardb_unlink(db_path);
    otsardb_unlink(restored_path);
    {
        char wal[288];
        snprintf(wal, sizeof(wal), "%s-wal", db_path);
        otsardb_unlink(wal);
        snprintf(wal, sizeof(wal), "%s-shm", db_path);
        otsardb_unlink(wal);
    }

    otsardb_object_store *store = otsardb_test_memory_store_create();
    if (!store) return NULL;

    otsardb_wal_publisher publisher;
    if (!otsardb_wal_publisher_init(&publisher, store, root, db_id,
                                  OTSARDB_WRITE_STRATEGY_DEFAULT)) {
        otsardb_test_memory_store_destroy(store);
        return NULL;
    }

    otsardb_local_vfs_instance *vfs = NULL;
    if (otsardb_local_vfs_instance_create(otsardb_wal_publisher_sync_reader,
                                        &publisher, &vfs) != 0) {
        otsardb_test_memory_store_destroy(store);
        return NULL;
    }

    otsardb *db = NULL;
    if (otsardb_open_with_vfs(db_path,
                            otsardb_local_vfs_instance_name(vfs),
                            &db) != OTSARDB_OK) {
        otsardb_local_vfs_instance_destroy(vfs);
        otsardb_test_memory_store_destroy(store);
        return NULL;
    }

    char *error = NULL;
    int rc = otsardb_exec(db, "PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0;",
                        NULL, NULL, &error);
    otsardb_free(error);
    if (rc != OTSARDB_OK) { otsardb_close(db); otsardb_local_vfs_instance_destroy(vfs); otsardb_test_memory_store_destroy(store); return NULL; }

    rc = otsardb_exec(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, value TEXT);",
                    NULL, NULL, &error);
    otsardb_free(error);
    if (rc != OTSARDB_OK) { otsardb_close(db); otsardb_local_vfs_instance_destroy(vfs); otsardb_test_memory_store_destroy(store); return NULL; }

    char sql[2048];
    snprintf(sql, sizeof(sql),
             "WITH RECURSIVE cnt(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM cnt WHERE x < %u) "
             "INSERT INTO t(value) SELECT 'v' || x FROM cnt;",
             ROWS_PER_THREAD);
    rc = otsardb_exec(db, sql, NULL, NULL, &error);
    otsardb_free(error);
    if (rc != OTSARDB_OK) { otsardb_close(db); otsardb_local_vfs_instance_destroy(vfs); otsardb_test_memory_store_destroy(store); return NULL; }

    otsardb_head head;
    char *etag = NULL;
    int head_ok = otsardb_journal_read_head(store, root, &head, &etag) == OTSARDB_JOURNAL_OK;
    free(etag);
    if (!head_ok || head.generation < 2) { otsardb_close(db); otsardb_local_vfs_instance_destroy(vfs); otsardb_test_memory_store_destroy(store); return NULL; }

    otsardb_close(db);
    otsardb_local_vfs_instance_destroy(vfs);

    otsardb_unlink(restored_path);
    if (!otsardb_recovery_restore_store(store, root, restored_path)) {
        otsardb_test_memory_store_destroy(store);
        return NULL;
    }

    otsardb *recovered = NULL;
    if (otsardb_open(restored_path, &recovered) != OTSARDB_OK) {
        otsardb_test_memory_store_destroy(store);
        return NULL;
    }

    char count_text[64] = {0};
    error = NULL;
    rc = otsardb_exec(recovered, "SELECT count(*) FROM t;",
                    collect_text, count_text, &error);
    otsardb_free(error);
    otsardb_close(recovered);

    otsardb_unlink(db_path);
    otsardb_unlink(restored_path);
    {
        char wal[288];
        snprintf(wal, sizeof(wal), "%s-wal", db_path);
        otsardb_unlink(wal);
        snprintf(wal, sizeof(wal), "%s-shm", db_path);
        otsardb_unlink(wal);
    }

    otsardb_test_memory_store_destroy(store);

    char expected[16];
    snprintf(expected, sizeof(expected), "%u", ROWS_PER_THREAD);
    if (rc == OTSARDB_OK && strcmp(count_text, expected) == 0) {
        ctx->ok = 1;
    }
    return NULL;
}

int main(void) {
    assert(otsardb_initialize() == OTSARDB_OK);

    otsardb_thread_t threads[THREAD_COUNT];
    thread_ctx contexts[THREAD_COUNT];

    for (unsigned int i = 0; i < THREAD_COUNT; ++i) {
        contexts[i].index = i;
        contexts[i].ok = 0;
        assert(otsardb_thread_start(&threads[i], thread_work, &contexts[i]));
    }

    for (unsigned int i = 0; i < THREAD_COUNT; ++i) {
        assert(otsardb_thread_join(threads[i]));
        assert(contexts[i].ok);
    }

    puts("threaded concurrent multi-database isolation and recovery passed");
    return 0;
}
