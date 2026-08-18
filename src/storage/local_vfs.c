#include "local_vfs.h"
#include "sqlite3.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct otsardb_file otsardb_file;

struct otsardb_local_vfs_instance {
    sqlite3_vfs vfs;
    sqlite3_vfs *underlying_vfs;
    char name[64];
    otsardb_wal_sync_hook wal_sync_hook;
    void *wal_sync_ctx;
    otsardb_wal_reader_sync_hook wal_reader_sync_hook;
    void *wal_reader_sync_ctx;
    /* The wrapped file of the main database (opened with SQLITE_OPEN_MAIN_DB),
     * used to read old page values when computing rolling checksums. */
    otsardb_file *main_db_file;
    /* Lazy-hydration page hooks: optional; when installed,
     * main-db reads are preceded by a hydrate call and main-db writes are
     * reported back so the hydrator never re-applies older segment data. */
    otsardb_page_hydrate_hook page_hydrate_hook;
    otsardb_page_mark_hook page_mark_hook;
    otsardb_page_truncate_hook page_truncate_hook;
    void *page_hook_ctx;
};

static otsardb_local_vfs_instance g_default_instance;
static int g_default_registered = 0;

typedef struct otsardb_file {
    sqlite3_file base;
    sqlite3_file *underlying;
    const sqlite3_io_methods *underlying_methods;
    otsardb_local_vfs_instance *instance;
    char *name;
    int flags;
    int wal_dirty;
} otsardb_file;

/* MSVC does not provide C11 max_align_t in its <stddef.h>; derive the
 * strictest alignment from a member union instead. */
typedef union otsardb_max_align {
    long double ld;
    double d;
    void *ptr;
    unsigned long long u64;
} otsardb_max_align;

static size_t align_up(size_t value) {
    const size_t alignment = _Alignof(otsardb_max_align);
    return (value + alignment - 1u) & ~(alignment - 1u);
}

static char *otsardb_strdup(const char *value) {
    if (!value) return 0;
    size_t size = strlen(value) + 1u;
    char *copy = (char *)malloc(size);
    if (copy) memcpy(copy, value, size);
    return copy;
}

static otsardb_file *as_otsardb_file(sqlite3_file *file) {
    return (otsardb_file *)file;
}

static otsardb_local_vfs_instance *instance_from_vfs(sqlite3_vfs *vfs) {
    return vfs ? (otsardb_local_vfs_instance *)vfs->pAppData : 0;
}

static sqlite3_vfs *underlying_vfs(sqlite3_vfs *vfs) {
    otsardb_local_vfs_instance *instance = instance_from_vfs(vfs);
    return instance ? instance->underlying_vfs : 0;
}

static int otsardb_io_close(sqlite3_file *file) {
    otsardb_file *wrapped = as_otsardb_file(file);
    int rc = SQLITE_OK;
    if (wrapped->underlying_methods && wrapped->underlying_methods->xClose) {
        rc = wrapped->underlying_methods->xClose(wrapped->underlying);
    }
    free(wrapped->name);
    wrapped->name = 0;
    wrapped->instance = 0;
    wrapped->base.pMethods = 0;
    return rc;
}

static int otsardb_io_read(sqlite3_file *file, void *buffer, int amount, sqlite3_int64 offset) {
    otsardb_file *wrapped = as_otsardb_file(file);
    /* Lazy hydration: materialise the requested range before serving it.
     * Only the main database file is hydrated; WAL/SHM files are never
     * touched. A hydrate failure is surfaced to the pager as-is (fail
     * closed) so a wrong/zero page can never be served silently. */
    if ((wrapped->flags & SQLITE_OPEN_MAIN_DB) != 0 &&
        wrapped->instance && wrapped->instance->page_hydrate_hook) {
        int rc = wrapped->instance->page_hydrate_hook(wrapped->instance->page_hook_ctx,
                                                      offset,
                                                      amount);
        if (rc != SQLITE_OK) return rc;
    }
    return wrapped->underlying_methods->xRead(wrapped->underlying, buffer, amount, offset);
}

static int otsardb_io_write(sqlite3_file *file,
                          const void *buffer,
                          int amount,
                          sqlite3_int64 offset) {
    otsardb_file *wrapped = as_otsardb_file(file);
    int rc = wrapped->underlying_methods->xWrite(wrapped->underlying, buffer, amount, offset);
    if (rc == SQLITE_OK) {
        if ((wrapped->flags & SQLITE_OPEN_WAL) != 0) {
            wrapped->wal_dirty = 1;
        } else if ((wrapped->flags & SQLITE_OPEN_MAIN_DB) != 0 &&
                   wrapped->instance && wrapped->instance->page_mark_hook) {
            /* Pages written by SQLite itself are real data now: tell the
             * hydrator so it never overwrites them from older segments. */
            wrapped->instance->page_mark_hook(wrapped->instance->page_hook_ctx,
                                              offset,
                                              amount);
        }
    }
    return rc;
}

static int otsardb_io_truncate(sqlite3_file *file, sqlite3_int64 size) {
    otsardb_file *wrapped = as_otsardb_file(file);
    int rc = wrapped->underlying_methods->xTruncate(wrapped->underlying, size);
    if (rc == SQLITE_OK) {
        if ((wrapped->flags & SQLITE_OPEN_WAL) != 0) {
            wrapped->wal_dirty = 1;
        } else if ((wrapped->flags & SQLITE_OPEN_MAIN_DB) != 0 &&
                   wrapped->instance && wrapped->instance->page_truncate_hook) {
            wrapped->instance->page_truncate_hook(wrapped->instance->page_hook_ctx,
                                                  size);
        }
    }
    return rc;
}

static int read_underlying_range(otsardb_file *wrapped,
                                 uint64_t offset,
                                 void *buffer,
                                 size_t size) {
    unsigned char *out = (unsigned char *)buffer;
    size_t done = 0;
    while (done < size) {
        size_t remaining = size - done;
        int chunk = remaining > (size_t)INT_MAX ? INT_MAX : (int)remaining;
        if (offset > (uint64_t)INT64_MAX ||
            done > (size_t)((uint64_t)INT64_MAX - offset)) {
            return SQLITE_IOERR_READ;
        }
        int rc = wrapped->underlying_methods->xRead(wrapped->underlying,
                                                     out + done,
                                                     chunk,
                                                     (sqlite3_int64)(offset + done));
        if (rc != SQLITE_OK) return rc;
        done += (size_t)chunk;
    }
    return SQLITE_OK;
}

static int wal_reader_read_at(void *ctx, uint64_t offset, void *buffer, size_t size) {
    otsardb_file *wrapped = (otsardb_file *)ctx;
    return wrapped && read_underlying_range(wrapped, offset, buffer, size) == SQLITE_OK;
}

static int read_underlying_file(otsardb_file *wrapped, unsigned char **out_data, size_t *out_size) {
    sqlite3_int64 file_size = 0;
    int rc = wrapped->underlying_methods->xFileSize(wrapped->underlying, &file_size);
    if (rc != SQLITE_OK) return rc;
    if (file_size < 0 || (uint64_t)file_size > (uint64_t)SIZE_MAX) return SQLITE_IOERR_FSTAT;

    size_t size = (size_t)file_size;
    unsigned char *data = (unsigned char *)malloc(size ? size : 1u);
    if (!data) return SQLITE_NOMEM;

    rc = read_underlying_range(wrapped, 0, data, size);
    if (rc != SQLITE_OK) {
        free(data);
        return rc;
    }

    *out_data = data;
    *out_size = size;
    return SQLITE_OK;
}

static int otsardb_io_sync(sqlite3_file *file, int flags) {
    otsardb_file *wrapped = as_otsardb_file(file);
    int rc = wrapped->underlying_methods->xSync(wrapped->underlying, flags);
    if (rc != SQLITE_OK) return rc;

    if ((wrapped->flags & SQLITE_OPEN_WAL) == 0 || !wrapped->wal_dirty || !wrapped->instance) {
        return SQLITE_OK;
    }

    otsardb_local_vfs_instance *instance = wrapped->instance;
    if (instance->wal_reader_sync_hook) {
        sqlite3_int64 file_size = 0;
        rc = wrapped->underlying_methods->xFileSize(wrapped->underlying, &file_size);
        if (rc != SQLITE_OK) return rc;
        if (file_size < 0) return SQLITE_IOERR_FSTAT;

        rc = instance->wal_reader_sync_hook(wrapped->name ? wrapped->name : "",
                                            (uint64_t)file_size,
                                            wal_reader_read_at,
                                            wrapped,
                                            instance->main_db_file
                                                ? wal_reader_read_at
                                                : 0,
                                            instance->main_db_file,
                                            instance->wal_reader_sync_ctx);
        if (rc == SQLITE_OK) wrapped->wal_dirty = 0;
        return rc;
    }

    if (instance->wal_sync_hook) {
        unsigned char *wal_data = 0;
        size_t wal_size = 0;
        rc = read_underlying_file(wrapped, &wal_data, &wal_size);
        if (rc != SQLITE_OK) return rc;

        rc = instance->wal_sync_hook(wrapped->name ? wrapped->name : "",
                                     wal_data,
                                     wal_size,
                                     instance->wal_sync_ctx);
        free(wal_data);
        if (rc == SQLITE_OK) wrapped->wal_dirty = 0;
        return rc;
    }

    return SQLITE_OK;
}

static int otsardb_io_file_size(sqlite3_file *file, sqlite3_int64 *size) {
    otsardb_file *wrapped = as_otsardb_file(file);
    return wrapped->underlying_methods->xFileSize(wrapped->underlying, size);
}

static int otsardb_io_lock(sqlite3_file *file, int lock) {
    otsardb_file *wrapped = as_otsardb_file(file);
    return wrapped->underlying_methods->xLock(wrapped->underlying, lock);
}

static int otsardb_io_unlock(sqlite3_file *file, int lock) {
    otsardb_file *wrapped = as_otsardb_file(file);
    return wrapped->underlying_methods->xUnlock(wrapped->underlying, lock);
}

static int otsardb_io_check_reserved_lock(sqlite3_file *file, int *result) {
    otsardb_file *wrapped = as_otsardb_file(file);
    return wrapped->underlying_methods->xCheckReservedLock(wrapped->underlying, result);
}

static int otsardb_io_file_control(sqlite3_file *file, int op, void *arg) {
    otsardb_file *wrapped = as_otsardb_file(file);
    return wrapped->underlying_methods->xFileControl(wrapped->underlying, op, arg);
}

static int otsardb_io_sector_size(sqlite3_file *file) {
    otsardb_file *wrapped = as_otsardb_file(file);
    return wrapped->underlying_methods->xSectorSize(wrapped->underlying);
}

static int otsardb_io_device_characteristics(sqlite3_file *file) {
    otsardb_file *wrapped = as_otsardb_file(file);
    return wrapped->underlying_methods->xDeviceCharacteristics(wrapped->underlying);
}

static int otsardb_io_shm_map(sqlite3_file *file,
                            int page,
                            int page_size,
                            int extend,
                            void volatile **out) {
    otsardb_file *wrapped = as_otsardb_file(file);
    if (wrapped->underlying_methods->iVersion < 2 || !wrapped->underlying_methods->xShmMap) {
        return SQLITE_IOERR_SHMMAP;
    }
    return wrapped->underlying_methods->xShmMap(wrapped->underlying,
                                                page,
                                                page_size,
                                                extend,
                                                out);
}

static int otsardb_io_shm_lock(sqlite3_file *file, int offset, int count, int flags) {
    otsardb_file *wrapped = as_otsardb_file(file);
    if (wrapped->underlying_methods->iVersion < 2 || !wrapped->underlying_methods->xShmLock) {
        return SQLITE_IOERR_SHMLOCK;
    }
    return wrapped->underlying_methods->xShmLock(wrapped->underlying, offset, count, flags);
}

static void otsardb_io_shm_barrier(sqlite3_file *file) {
    otsardb_file *wrapped = as_otsardb_file(file);
    if (wrapped->underlying_methods->iVersion >= 2 && wrapped->underlying_methods->xShmBarrier) {
        wrapped->underlying_methods->xShmBarrier(wrapped->underlying);
    }
}

static int otsardb_io_shm_unmap(sqlite3_file *file, int delete_flag) {
    otsardb_file *wrapped = as_otsardb_file(file);
    if (wrapped->underlying_methods->iVersion < 2 || !wrapped->underlying_methods->xShmUnmap) {
        return SQLITE_OK;
    }
    return wrapped->underlying_methods->xShmUnmap(wrapped->underlying, delete_flag);
}

static int otsardb_io_fetch(sqlite3_file *file,
                          sqlite3_int64 offset,
                          int amount,
                          void **out) {
    otsardb_file *wrapped = as_otsardb_file(file);
    if (wrapped->underlying_methods->iVersion < 3 || !wrapped->underlying_methods->xFetch) {
        *out = 0;
        return SQLITE_OK;
    }
    return wrapped->underlying_methods->xFetch(wrapped->underlying, offset, amount, out);
}

static int otsardb_io_unfetch(sqlite3_file *file, sqlite3_int64 offset, void *ptr) {
    otsardb_file *wrapped = as_otsardb_file(file);
    if (wrapped->underlying_methods->iVersion < 3 || !wrapped->underlying_methods->xUnfetch) {
        return SQLITE_OK;
    }
    return wrapped->underlying_methods->xUnfetch(wrapped->underlying, offset, ptr);
}

static const sqlite3_io_methods g_io_methods = {
    3,
    otsardb_io_close,
    otsardb_io_read,
    otsardb_io_write,
    otsardb_io_truncate,
    otsardb_io_sync,
    otsardb_io_file_size,
    otsardb_io_lock,
    otsardb_io_unlock,
    otsardb_io_check_reserved_lock,
    otsardb_io_file_control,
    otsardb_io_sector_size,
    otsardb_io_device_characteristics,
    otsardb_io_shm_map,
    otsardb_io_shm_lock,
    otsardb_io_shm_barrier,
    otsardb_io_shm_unmap,
    otsardb_io_fetch,
    otsardb_io_unfetch,
};

static int otsardb_vfs_open(sqlite3_vfs *vfs,
                          const char *name,
                          sqlite3_file *file,
                          int flags,
                          int *out_flags) {
    otsardb_local_vfs_instance *instance = instance_from_vfs(vfs);
    sqlite3_vfs *base = underlying_vfs(vfs);
    if (!instance || !base) return SQLITE_CANTOPEN;

    size_t underlying_offset = align_up(sizeof(otsardb_file));
    memset(file, 0, (size_t)vfs->szOsFile);

    otsardb_file *wrapped = (otsardb_file *)file;
    wrapped->instance = instance;
    wrapped->underlying = (sqlite3_file *)((unsigned char *)file + underlying_offset);

    int rc = base->xOpen(base, name, wrapped->underlying, flags, out_flags);
    if (rc != SQLITE_OK) return rc;
    if (!wrapped->underlying->pMethods) return SQLITE_CANTOPEN;

    wrapped->underlying_methods = wrapped->underlying->pMethods;
    wrapped->flags = flags;
    if (name) {
        wrapped->name = otsardb_strdup(name);
        if (!wrapped->name) {
            wrapped->underlying_methods->xClose(wrapped->underlying);
            return SQLITE_NOMEM;
        }
    }
    if ((flags & SQLITE_OPEN_MAIN_DB) != 0) {
        instance->main_db_file = wrapped;
    }
    wrapped->base.pMethods = &g_io_methods;
    return SQLITE_OK;
}

static int otsardb_vfs_delete(sqlite3_vfs *vfs, const char *name, int sync_dir) {
    sqlite3_vfs *base = underlying_vfs(vfs);
    return base ? base->xDelete(base, name, sync_dir) : SQLITE_IOERR_DELETE;
}

static int otsardb_vfs_access(sqlite3_vfs *vfs, const char *name, int flags, int *result) {
    sqlite3_vfs *base = underlying_vfs(vfs);
    return base ? base->xAccess(base, name, flags, result) : SQLITE_IOERR_ACCESS;
}

static int otsardb_vfs_full_pathname(sqlite3_vfs *vfs, const char *name, int size, char *out) {
    sqlite3_vfs *base = underlying_vfs(vfs);
    return base ? base->xFullPathname(base, name, size, out) : SQLITE_CANTOPEN;
}

static void *otsardb_vfs_dlopen(sqlite3_vfs *vfs, const char *name) {
    sqlite3_vfs *base = underlying_vfs(vfs);
    return base && base->xDlOpen ? base->xDlOpen(base, name) : 0;
}

static void otsardb_vfs_dlerror(sqlite3_vfs *vfs, int size, char *out) {
    sqlite3_vfs *base = underlying_vfs(vfs);
    if (base && base->xDlError) base->xDlError(base, size, out);
}

static void (*otsardb_vfs_dlsym(sqlite3_vfs *vfs, void *handle, const char *symbol))(void) {
    sqlite3_vfs *base = underlying_vfs(vfs);
    return base && base->xDlSym ? base->xDlSym(base, handle, symbol) : 0;
}

static void otsardb_vfs_dlclose(sqlite3_vfs *vfs, void *handle) {
    sqlite3_vfs *base = underlying_vfs(vfs);
    if (base && base->xDlClose) base->xDlClose(base, handle);
}

static int otsardb_vfs_randomness(sqlite3_vfs *vfs, int size, char *out) {
    sqlite3_vfs *base = underlying_vfs(vfs);
    return base ? base->xRandomness(base, size, out) : 0;
}

static int otsardb_vfs_sleep(sqlite3_vfs *vfs, int microseconds) {
    sqlite3_vfs *base = underlying_vfs(vfs);
    return base ? base->xSleep(base, microseconds) : microseconds;
}

static int otsardb_vfs_current_time(sqlite3_vfs *vfs, double *value) {
    sqlite3_vfs *base = underlying_vfs(vfs);
    return base ? base->xCurrentTime(base, value) : SQLITE_ERROR;
}

static int otsardb_vfs_get_last_error(sqlite3_vfs *vfs, int a, char *b) {
    sqlite3_vfs *base = underlying_vfs(vfs);
    return base && base->xGetLastError ? base->xGetLastError(base, a, b) : 0;
}

static int otsardb_vfs_current_time_i64(sqlite3_vfs *vfs, sqlite3_int64 *value) {
    sqlite3_vfs *base = underlying_vfs(vfs);
    return base && base->xCurrentTimeInt64 ? base->xCurrentTimeInt64(base, value) : SQLITE_NOTFOUND;
}

static int otsardb_vfs_set_system_call(sqlite3_vfs *vfs, const char *name, sqlite3_syscall_ptr fn) {
    sqlite3_vfs *base = underlying_vfs(vfs);
    return base && base->xSetSystemCall ? base->xSetSystemCall(base, name, fn) : SQLITE_NOTFOUND;
}

static sqlite3_syscall_ptr otsardb_vfs_get_system_call(sqlite3_vfs *vfs, const char *name) {
    sqlite3_vfs *base = underlying_vfs(vfs);
    return base && base->xGetSystemCall ? base->xGetSystemCall(base, name) : 0;
}

static const char *otsardb_vfs_next_system_call(sqlite3_vfs *vfs, const char *name) {
    sqlite3_vfs *base = underlying_vfs(vfs);
    return base && base->xNextSystemCall ? base->xNextSystemCall(base, name) : 0;
}

static int configure_instance(otsardb_local_vfs_instance *instance,
                              sqlite3_vfs *base,
                              const char *name,
                              otsardb_wal_reader_sync_hook hook,
                              void *ctx) {
    if (!instance || !base || !name || name[0] == '\0') return SQLITE_MISUSE;

    size_t underlying_offset = align_up(sizeof(otsardb_file));
    if (underlying_offset > (size_t)INT_MAX ||
        (size_t)base->szOsFile > (size_t)INT_MAX - underlying_offset) {
        return SQLITE_TOOBIG;
    }

    memset(instance, 0, sizeof(*instance));
    instance->underlying_vfs = base;
    instance->wal_reader_sync_hook = hook;
    instance->wal_reader_sync_ctx = ctx;
    snprintf(instance->name, sizeof(instance->name), "%s", name);

    memcpy(&instance->vfs, base, sizeof(sqlite3_vfs));
    instance->vfs.zName = instance->name;
    instance->vfs.pAppData = instance;
    instance->vfs.szOsFile = (int)(underlying_offset + (size_t)base->szOsFile);
    instance->vfs.xOpen = otsardb_vfs_open;
    instance->vfs.xDelete = otsardb_vfs_delete;
    instance->vfs.xAccess = otsardb_vfs_access;
    instance->vfs.xFullPathname = otsardb_vfs_full_pathname;
    instance->vfs.xDlOpen = otsardb_vfs_dlopen;
    instance->vfs.xDlError = otsardb_vfs_dlerror;
    instance->vfs.xDlSym = otsardb_vfs_dlsym;
    instance->vfs.xDlClose = otsardb_vfs_dlclose;
    instance->vfs.xRandomness = otsardb_vfs_randomness;
    instance->vfs.xSleep = otsardb_vfs_sleep;
    instance->vfs.xCurrentTime = otsardb_vfs_current_time;
    instance->vfs.xGetLastError = otsardb_vfs_get_last_error;
    if (instance->vfs.iVersion >= 2) instance->vfs.xCurrentTimeInt64 = otsardb_vfs_current_time_i64;
    if (instance->vfs.iVersion >= 3) {
        instance->vfs.xSetSystemCall = otsardb_vfs_set_system_call;
        instance->vfs.xGetSystemCall = otsardb_vfs_get_system_call;
        instance->vfs.xNextSystemCall = otsardb_vfs_next_system_call;
    }
    return SQLITE_OK;
}

int otsardb_local_vfs_register(void) {
    if (g_default_registered) return SQLITE_OK;
    sqlite3_vfs *base = sqlite3_vfs_find(0);
    if (!base) return SQLITE_ERROR;

    int rc = configure_instance(&g_default_instance, base, "otsardb-local", 0, 0);
    if (rc != SQLITE_OK) return rc;

    rc = sqlite3_vfs_register(&g_default_instance.vfs, 0);
    if (rc == SQLITE_OK) g_default_registered = 1;
    return rc;
}

const char *otsardb_local_vfs_name(void) {
    return "otsardb-local";
}

int otsardb_local_vfs_instance_create(otsardb_wal_reader_sync_hook hook,
                                    void *ctx,
                                    otsardb_local_vfs_instance **out_instance) {
    if (!out_instance || !hook) return SQLITE_MISUSE;
    *out_instance = 0;

    int rc = sqlite3_initialize();
    if (rc != SQLITE_OK) return rc;

    sqlite3_vfs *base = sqlite3_vfs_find(0);
    if (!base) return SQLITE_ERROR;

    otsardb_local_vfs_instance *instance =
        (otsardb_local_vfs_instance *)calloc(1, sizeof(*instance));
    if (!instance) return SQLITE_NOMEM;

    char name[64];
    int unique = 0;
    for (int attempt = 0; attempt < 8 && !unique; ++attempt) {
        unsigned char random_bytes[8];
        sqlite3_randomness((int)sizeof(random_bytes), random_bytes);
        snprintf(name,
                 sizeof(name),
                 "otsardb-db-%02x%02x%02x%02x%02x%02x%02x%02x",
                 random_bytes[0], random_bytes[1], random_bytes[2], random_bytes[3],
                 random_bytes[4], random_bytes[5], random_bytes[6], random_bytes[7]);
        unique = sqlite3_vfs_find(name) == 0;
    }
    if (!unique) {
        free(instance);
        return SQLITE_BUSY;
    }

    rc = configure_instance(instance, base, name, hook, ctx);
    if (rc != SQLITE_OK) {
        free(instance);
        return rc;
    }

    rc = sqlite3_vfs_register(&instance->vfs, 0);
    if (rc != SQLITE_OK) {
        free(instance);
        return rc;
    }

    *out_instance = instance;
    return SQLITE_OK;
}

const char *otsardb_local_vfs_instance_name(const otsardb_local_vfs_instance *instance) {
    return instance ? instance->name : 0;
}

int otsardb_local_vfs_instance_set_page_hooks(otsardb_local_vfs_instance *instance,
                                            otsardb_page_hydrate_hook hydrate,
                                            otsardb_page_mark_hook mark,
                                            otsardb_page_truncate_hook truncate,
                                            void *ctx) {
    if (!instance || (!hydrate && !mark && !truncate)) return SQLITE_MISUSE;
    instance->page_hydrate_hook = hydrate;
    instance->page_mark_hook = mark;
    instance->page_truncate_hook = truncate;
    instance->page_hook_ctx = ctx;
    return SQLITE_OK;
}

void otsardb_local_vfs_instance_destroy(otsardb_local_vfs_instance *instance) {
    if (!instance || instance == &g_default_instance) return;
    sqlite3_vfs_unregister(&instance->vfs);
    free(instance);
}

void otsardb_local_vfs_set_wal_sync_hook(otsardb_wal_sync_hook hook, void *ctx) {
    g_default_instance.wal_sync_hook = hook;
    g_default_instance.wal_sync_ctx = ctx;
}

void otsardb_local_vfs_set_wal_reader_sync_hook(otsardb_wal_reader_sync_hook hook,
                                               void *ctx) {
    g_default_instance.wal_reader_sync_hook = hook;
    g_default_instance.wal_reader_sync_ctx = ctx;
}
