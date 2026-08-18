#include "memory_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct entry {
    char *key;
    unsigned char *data;
    size_t size;
    unsigned long long version;
    int64_t last_modified;
} entry;

typedef struct memory_store {
    entry *entries;
    size_t count;
    size_t capacity;
    unsigned long long next_version;
    otsardb_store_capabilities supported_capabilities;
} memory_store;

static char *dup_string(const char *value) {
    if (!value) return NULL;
    size_t len = strlen(value) + 1;
    char *copy = (char *)malloc(len);
    if (copy) memcpy(copy, value, len);
    return copy;
}

static entry *find_entry(memory_store *memory, const char *key) {
    for (size_t i = 0; i < memory->count; ++i) {
        if (strcmp(memory->entries[i].key, key) == 0) return &memory->entries[i];
    }
    return NULL;
}

static char *make_etag(unsigned long long version) {
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "v%llu", version);
    return dup_string(buffer);
}

static otsardb_store_result copy_object(entry *found,
                                      size_t offset,
                                      size_t length,
                                      int range,
                                      otsardb_store_object *out) {
    if (!found || !out) return OTSARDB_STORE_ERROR;
    size_t start = range ? offset : 0;
    size_t count = range ? length : found->size;
    if (start > found->size) return OTSARDB_STORE_ERROR;
    if (count > found->size - start) count = found->size - start;

    memset(out, 0, sizeof(*out));
    out->data = (unsigned char *)malloc(count ? count : 1);
    if (!out->data) return OTSARDB_STORE_ERROR;
    if (count) memcpy(out->data, found->data + start, count);
    out->size = count;
    out->etag = make_etag(found->version);
    if (!out->etag) {
        free(out->data);
        memset(out, 0, sizeof(*out));
        return OTSARDB_STORE_ERROR;
    }
    out->last_modified_secs = found->last_modified;
    return OTSARDB_STORE_OK;
}

static otsardb_store_result memory_get(otsardb_object_store *store, const char *key, otsardb_store_object *out) {
    memory_store *memory = (memory_store *)store->context;
    entry *found = find_entry(memory, key);
    if (!found) return OTSARDB_STORE_NOT_FOUND;
    return copy_object(found, 0, 0, 0, out);
}

static otsardb_store_result memory_get_range(otsardb_object_store *store,
                                           const char *key,
                                           size_t offset,
                                           size_t length,
                                           otsardb_store_object *out) {
    memory_store *memory = (memory_store *)store->context;
    if (!(memory->supported_capabilities & OTSARDB_STORE_CAP_RANGE_GET)) {
        return OTSARDB_STORE_UNSUPPORTED;
    }
    entry *found = find_entry(memory, key);
    if (!found) return OTSARDB_STORE_NOT_FOUND;
    return copy_object(found, offset, length, 1, out);
}

static otsardb_store_result memory_put(otsardb_object_store *store,
                                     const char *key,
                                     const void *data,
                                     size_t size,
                                     const otsardb_store_put_options *options,
                                     char **out_etag) {
    memory_store *memory = (memory_store *)store->context;
    entry *found = find_entry(memory, key);

    if (options && options->if_none_match) {
        if (!(memory->supported_capabilities & OTSARDB_STORE_CAP_CONDITIONAL_CREATE)) {
            return OTSARDB_STORE_UNSUPPORTED;
        }
        if (found) return OTSARDB_STORE_PRECONDITION_FAILED;
    }
    if (options && options->if_match_etag) {
        if (!(memory->supported_capabilities & OTSARDB_STORE_CAP_CONDITIONAL_UPDATE)) {
            return OTSARDB_STORE_UNSUPPORTED;
        }
        if (!found) return OTSARDB_STORE_PRECONDITION_FAILED;
        char *current = make_etag(found->version);
        if (!current) return OTSARDB_STORE_ERROR;
        int match = strcmp(current, options->if_match_etag) == 0;
        free(current);
        if (!match) return OTSARDB_STORE_PRECONDITION_FAILED;
    }

    if (!found) {
        if (memory->count == memory->capacity) {
            size_t next_capacity = memory->capacity ? memory->capacity * 2 : 8;
            entry *next = (entry *)realloc(memory->entries, next_capacity * sizeof(*next));
            if (!next) return OTSARDB_STORE_ERROR;
            memory->entries = next;
            memory->capacity = next_capacity;
        }
        found = &memory->entries[memory->count++];
        memset(found, 0, sizeof(*found));
        found->key = dup_string(key);
        if (!found->key) return OTSARDB_STORE_ERROR;
    }

    unsigned char *copy = (unsigned char *)malloc(size ? size : 1);
    if (!copy) return OTSARDB_STORE_ERROR;
    if (size) memcpy(copy, data, size);
    free(found->data);
    found->data = copy;
    found->size = size;
    found->version = ++memory->next_version;
    found->last_modified = (int64_t)time(NULL);

    if (out_etag) {
        *out_etag = make_etag(found->version);
        if (!*out_etag) return OTSARDB_STORE_ERROR;
    }
    return OTSARDB_STORE_OK;
}

static otsardb_store_result memory_put_stream(otsardb_object_store *store,
                                            const char *key,
                                            otsardb_store_source_read source_read,
                                            void *source_ctx,
                                            uint64_t expected_size,
                                            const otsardb_store_put_options *options,
                                            char **out_etag) {
    if (!store || !key || key[0] == '\0' || !source_read ||
        expected_size > (uint64_t)SIZE_MAX) {
        return OTSARDB_STORE_ERROR;
    }
    /* In-memory model of a streamed put: pull exactly `expected_size` bytes
     * (mirrors the S3 adapter's known-size contract; a short source is an
     * error), then store them like a plain put. Used by the multipart
     * publish-path tests. */
    size_t want = (size_t)expected_size;
    unsigned char *buffer = (unsigned char *)malloc(want ? want : 1u);
    if (!buffer) return OTSARDB_STORE_ERROR;
    size_t total = 0;
    while (total < want) {
        long n = source_read(source_ctx, buffer + total, want - total);
        if (n < 0) {
            free(buffer);
            return OTSARDB_STORE_ERROR;
        }
        if (n == 0) break;
        total += (size_t)n;
    }
    if (total != want) {
        free(buffer);
        return OTSARDB_STORE_ERROR;
    }
    otsardb_store_result rc = memory_put(store, key, buffer, total, options, out_etag);
    free(buffer);
    return rc;
}

static void memory_release(otsardb_object_store *store, otsardb_store_object *object) {
    (void)store;
    if (!object) return;
    free(object->data);
    free(object->etag);
    memset(object, 0, sizeof(*object));
}

static otsardb_store_result memory_delete(otsardb_object_store *store, const char *key) {
    memory_store *memory = (memory_store *)store->context;
    entry *found = find_entry(memory, key);
    if (!found) return OTSARDB_STORE_NOT_FOUND;
    free(found->key);
    free(found->data);
    size_t idx = (size_t)(found - memory->entries);
    if (idx < memory->count - 1) {
        memmove(&memory->entries[idx], &memory->entries[idx + 1],
                (memory->count - idx - 1) * sizeof(*memory->entries));
    }
    --memory->count;
    return OTSARDB_STORE_OK;
}

static otsardb_store_result memory_list(otsardb_object_store *store,
                                      const char *prefix,
                                      otsardb_store_list_callback callback,
                                      void *ctx) {
    memory_store *memory = (memory_store *)store->context;
    size_t prefix_len = strlen(prefix ? prefix : "");
    for (size_t i = 0; i < memory->count; ++i) {
        const char *key = memory->entries[i].key;
        if (strncmp(key, prefix, prefix_len) != 0) continue;
        if (callback(key + prefix_len, ctx) != 0) return OTSARDB_STORE_OK;
    }
    return OTSARDB_STORE_OK;
}

static const otsardb_object_store_ops MEMORY_OPS = {
    .get = memory_get,
    .get_range = memory_get_range,
    .put = memory_put,
    .put_stream = memory_put_stream,
    .release_object = memory_release,
    .delete_object = memory_delete,
    .list = memory_list,
};

otsardb_object_store *otsardb_test_memory_store_create_with_capabilities(otsardb_store_capabilities capabilities) {
    otsardb_object_store *store = (otsardb_object_store *)calloc(1, sizeof(*store));
    memory_store *memory = (memory_store *)calloc(1, sizeof(*memory));
    if (!store || !memory) {
        free(store);
        free(memory);
        return NULL;
    }
    memory->supported_capabilities = capabilities;
    store->ops = &MEMORY_OPS;
    store->context = memory;
    store->capabilities = capabilities;
    return store;
}

otsardb_object_store *otsardb_test_memory_store_create(void) {
    return otsardb_test_memory_store_create_with_capabilities(
        OTSARDB_STORE_CAP_ETAG |
        OTSARDB_STORE_CAP_RANGE_GET |
        OTSARDB_STORE_CAP_READ_AFTER_WRITE |
        OTSARDB_STORE_CAP_CONDITIONAL_CREATE |
        OTSARDB_STORE_CAP_CONDITIONAL_UPDATE);
}

void otsardb_test_memory_store_destroy(otsardb_object_store *store) {
    if (!store) return;
    memory_store *memory = (memory_store *)store->context;
    if (memory) {
        for (size_t i = 0; i < memory->count; ++i) {
            free(memory->entries[i].key);
            free(memory->entries[i].data);
        }
        free(memory->entries);
        free(memory);
    }
    free(store);
}

void otsardb_test_memory_store_age(otsardb_object_store *store,
                                 const char *key,
                                 int64_t seconds) {
    if (!store || !key) return;
    memory_store *memory = (memory_store *)store->context;
    entry *found = find_entry(memory, key);
    if (!found) return;
    if (seconds < 0) seconds = 0;
    found->last_modified -= seconds;
    if (found->last_modified < 0) found->last_modified = 0;
}
