#ifndef OTSARDB_INTERNAL_H
#define OTSARDB_INTERNAL_H

#include "otsardb.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*otsardb_close_hook)(void *ctx);

/* Internal opening primitive used by storage integrations. vfs_name must name
 * an already registered VFS. */
int otsardb_open_with_vfs(const char *target, const char *vfs_name, otsardb **out_db);

/* Opens a database and transfers close_ctx ownership to the returned OtsarDB
 * handle. close_hook is invoked only after SQLite has successfully closed the
 * connection, allowing storage adapters to destroy VFS/publisher/store state in
 * the correct order. On open failure ownership is NOT transferred. */
int otsardb_open_with_vfs_owned(const char *target,
                              const char *vfs_name,
                              otsardb_close_hook close_hook,
                              void *close_ctx,
                              otsardb **out_db);

/* Internal engine handle access for OtsarDB-owned integrations such as the S3
 * durability authorizer. Never expose this from the installed public header. */
struct sqlite3;
struct sqlite3 *otsardb_internal_sqlite_handle(otsardb *db);

/* Close-hook context (the storage integration session) owned by an OtsarDB
 * handle opened with otsardb_open_with_vfs_owned. NULL when the handle was
 * opened without a close hook (e.g. plain local databases). */
void *otsardb_internal_close_ctx(otsardb *db);

/* (FRESH consistency mode): optional per-statement hook for
 * storage integrations. When installed, otsardb_exec invokes it BEFORE the
 * statement executes, with the connection quiescent (no statement in
 * flight). Returning OTSARDB_OK proceeds; any other return aborts the
 * statement with OTSARDB_ERROR. On failure the hook may set *error_message
 * (sqlite3_mprintf-owned; freed by the caller with otsardb_free) — for
 * example the mapped FRESH fail-closed error. NULL hook = pre-0123
 * behavior (byte-identical). */
typedef int (*otsardb_pre_exec_hook)(void *ctx, char **error_message);
int otsardb_internal_set_pre_exec_hook(otsardb *db, otsardb_pre_exec_hook hook, void *ctx);

/* (E4): optional statement serialization for storage
 * integrations whose image-swap paths (lease takeover, FRESH
 * re-materialisation) run on a background thread or hold their own session
 * lock. When installed, otsardb_exec invokes lock(ctx) before the pre-exec
 * hook and unlock(ctx) after sqlite3_exec — the WHOLE statement runs under
 * the integration's session lock, so a swap can never interleave with an
 * in-flight statement. NULL callbacks = pre-0134 behavior (no lock). */
void otsardb_internal_set_exec_lock(otsardb *db,
                                   void (*lock)(void *ctx),
                                   void (*unlock)(void *ctx),
                                   void *ctx);

#ifdef __cplusplus
}
#endif

#endif
