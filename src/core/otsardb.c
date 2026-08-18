#include "otsardb.h"
#include "otsardb_internal.h"
#include "local_vfs.h"
#include "sqlite3.h"

#include <stdlib.h>
#include <string.h>

#define OTSARDB_VERSION "0.1.0-dev"

struct otsardb {
    sqlite3 *handle;
    otsardb_close_hook close_hook;
    void *close_ctx;
    otsardb_pre_exec_hook pre_exec_hook;
    void *pre_exec_ctx;
    /* (E4): optional statement serialization. The S3 storage
     * integration registers callbacks that hold its session sql_mutex, the
     * same lock the lease-thread takeover, the FRESH pre-exec hook and the
     * forward server use; otsardb_exec then holds it around the WHOLE
     * statement (hook + execute) so an image swap can never rewrite the
     * execution file under an in-flight statement. NULL = pre-0134 behavior
     * (plain local databases never register it). */
    void (*exec_lock)(void *ctx);
    void (*exec_unlock)(void *ctx);
    void *exec_lock_ctx;
};

int otsardb_initialize(void) {
    int rc = sqlite3_initialize();
    if (rc != SQLITE_OK) return OTSARDB_ERROR;
    return otsardb_local_vfs_register() == SQLITE_OK ? OTSARDB_OK : OTSARDB_ERROR;
}

const char *otsardb_version(void) {
    return OTSARDB_VERSION;
}

const char *otsardb_engine_version(void) {
    return sqlite3_libversion();
}

static int is_s3_target(const char *target) {
    return target && (strncmp(target, "s3://", 5) == 0 || strncmp(target, "otsardb+s3://", 11) == 0);
}

int otsardb_open_with_vfs_owned(const char *target,
                              const char *vfs_name,
                              otsardb_close_hook close_hook,
                              void *close_ctx,
                              otsardb **out_db) {
    if (!out_db || !target || !vfs_name || vfs_name[0] == '\0') return OTSARDB_ERROR;
    *out_db = 0;

    if (is_s3_target(target)) return OTSARDB_UNSUPPORTED;
    if (otsardb_initialize() != OTSARDB_OK) return OTSARDB_ERROR;
    if (!sqlite3_vfs_find(vfs_name)) return OTSARDB_ERROR;

    otsardb *db = (otsardb *)calloc(1, sizeof(*db));
    if (!db) return OTSARDB_ERROR;

    int rc = sqlite3_open_v2(
        target,
        &db->handle,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI,
        vfs_name);

    if (rc != SQLITE_OK) {
        if (db->handle) sqlite3_close(db->handle);
        free(db);
        return OTSARDB_ERROR;
    }

    sqlite3_extended_result_codes(db->handle, 1);
    sqlite3_busy_timeout(db->handle, 5000);
    db->close_hook = close_hook;
    db->close_ctx = close_ctx;
    *out_db = db;
    return OTSARDB_OK;
}

int otsardb_open_with_vfs(const char *target, const char *vfs_name, otsardb **out_db) {
    return otsardb_open_with_vfs_owned(target, vfs_name, NULL, NULL, out_db);
}

int otsardb_open(const char *target, otsardb **out_db) {
    return otsardb_open_with_vfs(target, otsardb_local_vfs_name(), out_db);
}

int otsardb_exec(otsardb *db, const char *sql, otsardb_row_callback callback, void *ctx, char **error_message) {
    if (!db || !db->handle || !sql) return OTSARDB_ERROR;
    /* (E4): hold the storage integration's session lock for
     * the whole statement. The swap paths (lease-takeover, FRESH
     * re-materialisation, forward server) take the same lock, so a takeover
     * can never rewrite the execution image while a statement reads it. */
    if (db->exec_lock) db->exec_lock(db->exec_lock_ctx);
    int rc;
    if (db->pre_exec_hook) {
        int hook_rc = db->pre_exec_hook(db->pre_exec_ctx, error_message);
        if (hook_rc != OTSARDB_OK) {
            if (db->exec_unlock) db->exec_unlock(db->exec_lock_ctx);
            return OTSARDB_ERROR;
        }
    }
    rc = sqlite3_exec(db->handle, sql, callback, ctx, error_message) == SQLITE_OK ? OTSARDB_OK : OTSARDB_ERROR;
    if (db->exec_unlock) db->exec_unlock(db->exec_lock_ctx);
    return rc;
}

const char *otsardb_errmsg(otsardb *db) {
    if (!db || !db->handle) return "OtsarDB handle is not open";
    return sqlite3_errmsg(db->handle);
}

void otsardb_free(void *ptr) {
    sqlite3_free(ptr);
}

sqlite3 *otsardb_internal_sqlite_handle(otsardb *db) {
    return db ? db->handle : NULL;
}

void *otsardb_internal_close_ctx(otsardb *db) {
    return db ? db->close_ctx : NULL;
}

int otsardb_internal_set_pre_exec_hook(otsardb *db, otsardb_pre_exec_hook hook, void *ctx) {
    if (!db) return OTSARDB_ERROR;
    db->pre_exec_hook = hook;
    db->pre_exec_ctx = ctx;
    return OTSARDB_OK;
}

void otsardb_internal_set_exec_lock(otsardb *db,
                                   void (*lock)(void *ctx),
                                   void (*unlock)(void *ctx),
                                   void *ctx) {
    if (!db) return;
    db->exec_lock = lock;
    db->exec_unlock = unlock;
    db->exec_lock_ctx = ctx;
}

void otsardb_close(otsardb *db) {
    if (!db) return;

    if (db->handle) {
        int rc = sqlite3_close(db->handle);
        if (rc != SQLITE_OK) {
            /* OtsarDB's current public API does not expose prepared statements,
             * so SQLITE_BUSY is unexpected. Keep the handle and its storage
             * owner alive rather than destroying a VFS still referenced by
             * SQLite. */
            return;
        }
        db->handle = NULL;
    }

    if (db->close_hook) {
        db->close_hook(db->close_ctx);
        db->close_hook = NULL;
        db->close_ctx = NULL;
    }
    free(db);
}
