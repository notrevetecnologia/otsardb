#ifndef OTSARDB_LOCAL_VFS_H
#define OTSARDB_LOCAL_VFS_H

#include "sqlite_wal.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*otsardb_wal_sync_hook)(const char *wal_name,
                                   const void *wal_data,
                                   size_t wal_size,
                                   void *ctx);

typedef int (*otsardb_wal_reader_sync_hook)(const char *wal_name,
                                          uint64_t wal_size,
                                          otsardb_sqlite_wal_read_at read_at,
                                          void *read_ctx,
                                          otsardb_sqlite_wal_read_at db_read_at,
                                          void *db_read_ctx,
                                          void *ctx);

/* Lazy-hydration page hooks , invoked ONLY for the main
 * database file of an instance that installed them:
 * - hydrate: called BEFORE every main-db xRead; must ensure the byte range
 * [offset, offset+amount) is materialised in the underlying file
 * (fetching from the object store on demand). Returns SQLITE_OK or an
 * error code that is surfaced to the pager verbatim (fail closed).
 * - mark: called AFTER a successful main-db xWrite/xTruncate so pages
 * written by SQLite itself are never re-hydrated from older segments.
 * - truncate: called AFTER a successful main-db xTruncate with the new
 * size in bytes. */
typedef int (*otsardb_page_hydrate_hook)(void *ctx,
                                       long long offset,
                                       int amount);
typedef void (*otsardb_page_mark_hook)(void *ctx,
                                     long long offset,
                                     int amount);
typedef void (*otsardb_page_truncate_hook)(void *ctx,
                                         long long new_size);

typedef struct otsardb_local_vfs_instance otsardb_local_vfs_instance;

int otsardb_local_vfs_register(void);
const char *otsardb_local_vfs_name(void);

/*
 * Create a dedicated OtsarDB VFS instance whose pAppData owns the WAL durability
 * callback/context for one database connection. The returned VFS is registered
 * with SQLite under a unique name and must remain alive until all connections
 * using it have closed.
 */
int otsardb_local_vfs_instance_create(otsardb_wal_reader_sync_hook hook,
                                    void *ctx,
                                    otsardb_local_vfs_instance **out_instance);
const char *otsardb_local_vfs_instance_name(const otsardb_local_vfs_instance *instance);
void otsardb_local_vfs_instance_destroy(otsardb_local_vfs_instance *instance);

/*
 * Install the lazy-hydration page hooks on a dedicated instance (optional;
 * an instance without hooks behaves exactly like before). The hooks are
 * invoked only for SQLITE_OPEN_MAIN_DB files.
 */
int otsardb_local_vfs_instance_set_page_hooks(otsardb_local_vfs_instance *instance,
                                            otsardb_page_hydrate_hook hydrate,
                                            otsardb_page_mark_hook mark,
                                            otsardb_page_truncate_hook truncate,
                                            void *ctx);

/*
 * Legacy default-VFS hooks kept for tests/tooling. Production S3-backed OtsarDB
 * should use a dedicated VFS instance so durability state is connection-owned.
 */
void otsardb_local_vfs_set_wal_sync_hook(otsardb_wal_sync_hook hook, void *ctx);
void otsardb_local_vfs_set_wal_reader_sync_hook(otsardb_wal_reader_sync_hook hook,
                                               void *ctx);

#ifdef __cplusplus
}
#endif

#endif
