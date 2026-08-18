#ifndef OTSARDB_OBJECT_STORE_H
#define OTSARDB_OBJECT_STORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum otsardb_store_result {
    OTSARDB_STORE_OK = 0,
    OTSARDB_STORE_NOT_FOUND = 1,
    OTSARDB_STORE_PRECONDITION_FAILED = 2,
    OTSARDB_STORE_ERROR = 3,
    OTSARDB_STORE_NOT_EMPTY = 6,
    OTSARDB_STORE_TIMEOUT = 4,
    OTSARDB_STORE_UNSUPPORTED = 5
} otsardb_store_result;

typedef uint64_t otsardb_store_capabilities;

#define OTSARDB_STORE_CAP_ETAG               (UINT64_C(1) << 0)
#define OTSARDB_STORE_CAP_RANGE_GET          (UINT64_C(1) << 1)
#define OTSARDB_STORE_CAP_READ_AFTER_WRITE   (UINT64_C(1) << 2)
#define OTSARDB_STORE_CAP_CONDITIONAL_CREATE (UINT64_C(1) << 3)
#define OTSARDB_STORE_CAP_CONDITIONAL_UPDATE (UINT64_C(1) << 4)
#define OTSARDB_STORE_CAP_MULTIPART          (UINT64_C(1) << 5)

typedef struct otsardb_store_object {
    unsigned char *data;
    size_t size;
    char *etag;
    /* Server-side last modification time in unix seconds, when the store
     * exposes it (0 = unknown/not exposed). Used by the M2 writer lease to
     * judge a foreign lease's expiry from the server clock. */
    int64_t last_modified_secs;
} otsardb_store_object;

typedef struct otsardb_store_put_options {
    int if_none_match;
    const char *if_match_etag;
} otsardb_store_put_options;

typedef long (*otsardb_store_source_read)(void *ctx, void *buffer, size_t max_size);

/* List callback: invoked once per object key under the listed prefix (the
 * keys are logical keys, i.e. the global OTSARDB_S3_PREFIX is stripped).
 * Return 0 to continue, non-zero to abort the listing. */
typedef int (*otsardb_store_list_callback)(const char *key, void *ctx);

typedef struct otsardb_object_store otsardb_object_store;

typedef struct otsardb_object_store_ops {
    otsardb_store_result (*get)(otsardb_object_store *store, const char *key, otsardb_store_object *out);
    otsardb_store_result (*get_range)(otsardb_object_store *store,
                                    const char *key,
                                    size_t offset,
                                    size_t length,
                                    otsardb_store_object *out);
    otsardb_store_result (*put)(otsardb_object_store *store,
                              const char *key,
                              const void *data,
                              size_t size,
                              const otsardb_store_put_options *options,
                              char **out_etag);
    otsardb_store_result (*put_stream)(otsardb_object_store *store,
                                     const char *key,
                                     otsardb_store_source_read source_read,
                                     void *source_ctx,
                                     uint64_t expected_size,
                                     const otsardb_store_put_options *options,
                                     char **out_etag);
    void (*release_object)(otsardb_object_store *store, otsardb_store_object *object);
    otsardb_store_result (*delete_object)(otsardb_object_store *store, const char *key);
    /* Optional: list the object keys under `prefix` (logical keys). Stores
     * without listing return OTSARDB_STORE_UNSUPPORTED; consumers fall back. */
    otsardb_store_result (*list)(otsardb_object_store *store,
                               const char *prefix,
                               otsardb_store_list_callback callback,
                               void *ctx);
} otsardb_object_store_ops;

struct otsardb_object_store {
    const otsardb_object_store_ops *ops;
    void *context;
    otsardb_store_capabilities capabilities;
};

static inline int otsardb_store_supports(const otsardb_object_store *store,
                                       otsardb_store_capabilities capability) {
    return store && (store->capabilities & capability) == capability;
}

static inline otsardb_store_result otsardb_store_get(otsardb_object_store *store,
                                                  const char *key,
                                                  otsardb_store_object *out) {
    if (!store || !store->ops || !store->ops->get) return OTSARDB_STORE_UNSUPPORTED;
    return store->ops->get(store, key, out);
}

static inline otsardb_store_result otsardb_store_get_range(otsardb_object_store *store,
                                                        const char *key,
                                                        size_t offset,
                                                        size_t length,
                                                        otsardb_store_object *out) {
    if (!store || !store->ops || !store->ops->get_range) return OTSARDB_STORE_UNSUPPORTED;
    return store->ops->get_range(store, key, offset, length, out);
}

static inline otsardb_store_result otsardb_store_put(otsardb_object_store *store,
                                                  const char *key,
                                                  const void *data,
                                                  size_t size,
                                                  const otsardb_store_put_options *options,
                                                  char **out_etag) {
    if (!store || !store->ops || !store->ops->put) return OTSARDB_STORE_UNSUPPORTED;
    return store->ops->put(store, key, data, size, options, out_etag);
}

static inline otsardb_store_result otsardb_store_put_stream(otsardb_object_store *store,
                                                         const char *key,
                                                         otsardb_store_source_read source_read,
                                                         void *source_ctx,
                                                         uint64_t expected_size,
                                                         const otsardb_store_put_options *options,
                                                         char **out_etag) {
    if (!store || !store->ops || !store->ops->put_stream) return OTSARDB_STORE_UNSUPPORTED;
    return store->ops->put_stream(store, key, source_read, source_ctx,
                                  expected_size, options, out_etag);
}

static inline void otsardb_store_release_object(otsardb_object_store *store,
                                               otsardb_store_object *object) {
    if (!store || !store->ops || !store->ops->release_object) return;
    store->ops->release_object(store, object);
}

static inline otsardb_store_result otsardb_store_delete(otsardb_object_store *store, const char *key) {
    if (!store || !store->ops || !store->ops->delete_object) return OTSARDB_STORE_UNSUPPORTED;
    return store->ops->delete_object(store, key);
}

static inline otsardb_store_result otsardb_store_list(otsardb_object_store *store,
                                                   const char *prefix,
                                                   otsardb_store_list_callback callback,
                                                   void *ctx) {
    if (!store || !store->ops || !store->ops->list || !callback) return OTSARDB_STORE_UNSUPPORTED;
    return store->ops->list(store, prefix, callback, ctx);
}

#ifdef __cplusplus
}
#endif

#endif
