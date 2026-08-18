#include "fault_store.h"

#include <stdlib.h>

typedef struct fault_context {
    otsardb_object_store *inner;
    size_t put_count;
    size_t fail_put_index;
    otsardb_store_result fail_result;
    int fail_after_delegate;
    size_t fail_puts_count;
    size_t *fail_puts_indexes;
    size_t fail_puts_alloc;
    size_t get_count;
    size_t fail_get_index;
    otsardb_store_result fail_get_result;
} fault_context;

static otsardb_store_result fault_get(otsardb_object_store *store,
                                    const char *key,
                                    otsardb_store_object *out) {
    fault_context *ctx = (fault_context *)store->context;
    size_t index = ++ctx->get_count;
    if (ctx->fail_get_index != 0 && index == ctx->fail_get_index) {
        return ctx->fail_get_result;
    }
    return otsardb_store_get(ctx->inner, key, out);
}

static otsardb_store_result fault_get_range(otsardb_object_store *store,
                                          const char *key,
                                          size_t offset,
                                          size_t length,
                                          otsardb_store_object *out) {
    fault_context *ctx = (fault_context *)store->context;
    return otsardb_store_get_range(ctx->inner, key, offset, length, out);
}

static otsardb_store_result fault_put(otsardb_object_store *store,
                                    const char *key,
                                    const void *data,
                                    size_t size,
                                    const otsardb_store_put_options *options,
                                    char **out_etag) {
    fault_context *ctx = (fault_context *)store->context;
    size_t index = ++ctx->put_count;
    int should_fail = ctx->fail_put_index != 0 && index == ctx->fail_put_index;
    if (!should_fail && ctx->fail_puts_count > 0) {
        for (size_t i = 0; i < ctx->fail_puts_count; ++i) {
            if (ctx->fail_puts_indexes[i] == index) {
                should_fail = 1;
                break;
            }
        }
    }

    if (should_fail && !ctx->fail_after_delegate) return ctx->fail_result;

    otsardb_store_result rc = otsardb_store_put(ctx->inner, key, data, size, options, out_etag);
    if (should_fail && rc == OTSARDB_STORE_OK) return ctx->fail_result;
    return rc;
}

static void fault_release(otsardb_object_store *store, otsardb_store_object *object) {
    fault_context *ctx = (fault_context *)store->context;
    otsardb_store_release_object(ctx->inner, object);
}

static otsardb_store_result fault_list(otsardb_object_store *store,
                                     const char *prefix,
                                     otsardb_store_list_callback callback,
                                     void *ctx) {
    fault_context *fault = (fault_context *)store->context;
    return otsardb_store_list(fault->inner, prefix, callback, ctx);
}

static const otsardb_object_store_ops FAULT_OPS = {
    .get = fault_get,
    .get_range = fault_get_range,
    .put = fault_put,
    .release_object = fault_release,
    .list = fault_list,
};

otsardb_object_store *otsardb_test_fault_store_create(otsardb_object_store *inner) {
    if (!inner) return NULL;
    otsardb_object_store *store = (otsardb_object_store *)calloc(1, sizeof(*store));
    fault_context *ctx = (fault_context *)calloc(1, sizeof(*ctx));
    if (!store || !ctx) {
        free(store);
        free(ctx);
        return NULL;
    }
    ctx->inner = inner;
    store->ops = &FAULT_OPS;
    store->context = ctx;
    store->capabilities = inner->capabilities;
    return store;
}

void otsardb_test_fault_store_destroy(otsardb_object_store *store) {
    if (!store) return;
    fault_context *ctx = (fault_context *)store->context;
    if (ctx) free(ctx->fail_puts_indexes);
    free(store->context);
    free(store);
}

void otsardb_test_fault_store_fail_put(otsardb_object_store *store,
                                     size_t put_index,
                                     otsardb_store_result result,
                                     int after_delegate) {
    fault_context *ctx = (fault_context *)store->context;
    ctx->put_count = 0;
    ctx->fail_puts_count = 0;
    ctx->fail_put_index = put_index;
    ctx->fail_result = result;
    ctx->fail_after_delegate = after_delegate;
}

void otsardb_test_fault_store_fail_puts(otsardb_object_store *store,
                                      const size_t *indexes,
                                      size_t count,
                                      otsardb_store_result result,
                                      int after_delegate) {
    fault_context *ctx = (fault_context *)store->context;
    ctx->put_count = 0;
    ctx->fail_put_index = 0;
    ctx->fail_puts_count = 0;
    if (count > 0) {
        if (count > ctx->fail_puts_alloc) {
            size_t alloc = count > 16 ? count : 16;
            size_t *next = (size_t *)realloc(ctx->fail_puts_indexes,
                                             alloc * sizeof(size_t));
            if (!next) return;
            ctx->fail_puts_indexes = next;
            ctx->fail_puts_alloc = alloc;
        }
        for (size_t i = 0; i < count; ++i) ctx->fail_puts_indexes[i] = indexes[i];
        ctx->fail_puts_count = count;
    }
    ctx->fail_result = result;
    ctx->fail_after_delegate = after_delegate;
}

void otsardb_test_fault_store_clear(otsardb_object_store *store) {
    fault_context *ctx = (fault_context *)store->context;
    ctx->put_count = 0;
    ctx->fail_put_index = 0;
    ctx->fail_puts_count = 0;
    ctx->fail_result = OTSARDB_STORE_ERROR;
    ctx->fail_after_delegate = 0;
    ctx->get_count = 0;
    ctx->fail_get_index = 0;
    ctx->fail_get_result = OTSARDB_STORE_ERROR;
}

void otsardb_test_fault_store_fail_get(otsardb_object_store *store,
                                     size_t get_index,
                                     otsardb_store_result result) {
    fault_context *ctx = (fault_context *)store->context;
    ctx->get_count = 0;
    ctx->fail_get_index = get_index;
    ctx->fail_get_result = result;
}

size_t otsardb_test_fault_store_put_count(otsardb_object_store *store) {
    if (!store) return 0;
    fault_context *ctx = (fault_context *)store->context;
    return ctx ? ctx->put_count : 0;
}
