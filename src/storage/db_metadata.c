#include "db_metadata.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OTSARDB_METADATA_KEY_MAX 255

static int build_key(const char *root, char *out, size_t out_size) {
    if (!root || !out || out_size == 0) return 0;
    size_t len = strlen(root);
    int needs_slash = len > 0 && root[len - 1] != '/';
    int n = snprintf(out, out_size, "%s%sdb.json", root, needs_slash ? "/" : "");
    return n > 0 && (size_t)n < out_size;
}

static int extract_u64(const char *json, const char *name, uint64_t *out) {
    char pattern[64];
    if (snprintf(pattern, sizeof(pattern), "\"%s\":", name) >= (int)sizeof(pattern)) return 0;
    const char *p = strstr(json, pattern);
    if (!p) return 0;
    p += strlen(pattern);
    while (*p == ' ' || *p == '\t') ++p;
    char *end = NULL;
    unsigned long long value = strtoull(p, &end, 10);
    if (end == p) return 0;
    *out = (uint64_t)value;
    return 1;
}

static int extract_string(const char *json, const char *name, char *out, size_t out_size) {
    char pattern[64];
    if (snprintf(pattern, sizeof(pattern), "\"%s\":\"", name) >= (int)sizeof(pattern)) return 0;
    const char *p = strstr(json, pattern);
    if (!p) return 0;
    p += strlen(pattern);
    const char *end = strchr(p, '"');
    if (!end) return 0;
    size_t len = (size_t)(end - p);
    if (len == 0 || len >= out_size) return 0;
    memcpy(out, p, len);
    out[len] = '\0';
    return 1;
}

int otsardb_db_metadata_encode_json(const otsardb_db_metadata *metadata, char **out_json, size_t *out_size) {
    if (!metadata || !out_json || !out_size || metadata->format_version == 0 ||
        metadata->page_size == 0 || metadata->database_id[0] == '\0') return 0;

    int needed = snprintf(NULL, 0,
                          "{\"otsardb_format\":%u,\"database_id\":\"%s\",\"page_size\":%u}\n",
                          metadata->format_version,
                          metadata->database_id,
                          metadata->page_size);
    if (needed < 0) return 0;

    char *json = (char *)malloc((size_t)needed + 1);
    if (!json) return 0;

    int written = snprintf(json, (size_t)needed + 1,
                           "{\"otsardb_format\":%u,\"database_id\":\"%s\",\"page_size\":%u}\n",
                           metadata->format_version,
                           metadata->database_id,
                           metadata->page_size);
    if (written != needed) {
        free(json);
        return 0;
    }

    *out_json = json;
    *out_size = (size_t)written;
    return 1;
}

int otsardb_db_metadata_decode_json(const char *json, size_t size, otsardb_db_metadata *out_metadata) {
    if (!json || size == 0 || !out_metadata) return 0;

    char *copy = (char *)malloc(size + 1);
    if (!copy) return 0;
    memcpy(copy, json, size);
    copy[size] = '\0';

    uint64_t format = 0;
    uint64_t page_size = 0;
    memset(out_metadata, 0, sizeof(*out_metadata));

    int ok = extract_u64(copy, "otsardb_format", &format) &&
             extract_u64(copy, "page_size", &page_size) &&
             extract_string(copy, "database_id", out_metadata->database_id, sizeof(out_metadata->database_id));
    free(copy);

    if (!ok || format == 0 || format > UINT32_MAX || page_size == 0 || page_size > UINT32_MAX) return 0;
    out_metadata->format_version = (uint32_t)format;
    out_metadata->page_size = (uint32_t)page_size;
    return 1;
}

otsardb_metadata_result otsardb_db_metadata_create(otsardb_object_store *store,
                                                const char *database_root,
                                                const otsardb_db_metadata *metadata) {
    if (!store || !database_root || !metadata) return OTSARDB_METADATA_INVALID;

    char key[OTSARDB_METADATA_KEY_MAX + 1];
    if (!build_key(database_root, key, sizeof(key))) return OTSARDB_METADATA_INVALID;

    char *json = NULL;
    size_t json_size = 0;
    if (!otsardb_db_metadata_encode_json(metadata, &json, &json_size)) return OTSARDB_METADATA_INVALID;

    otsardb_store_put_options options = {0};
    if (otsardb_store_supports(store, OTSARDB_STORE_CAP_CONDITIONAL_CREATE)) {
        options.if_none_match = 1;
    } else {
        otsardb_store_object existing = {0};
        otsardb_store_result read_rc = otsardb_store_get(store, key, &existing);
        if (read_rc == OTSARDB_STORE_OK) {
            otsardb_store_release_object(store, &existing);
            free(json);
            return OTSARDB_METADATA_CONFLICT;
        }
        if (read_rc != OTSARDB_STORE_NOT_FOUND) {
            free(json);
            return OTSARDB_METADATA_ERROR;
        }
    }

    otsardb_store_result rc = otsardb_store_put(store, key, json, json_size, &options, NULL);
    free(json);

    if (rc == OTSARDB_STORE_PRECONDITION_FAILED) return OTSARDB_METADATA_CONFLICT;
    return rc == OTSARDB_STORE_OK ? OTSARDB_METADATA_OK : OTSARDB_METADATA_ERROR;
}

otsardb_metadata_result otsardb_db_metadata_read(otsardb_object_store *store,
                                              const char *database_root,
                                              otsardb_db_metadata *out_metadata) {
    if (!store || !database_root || !out_metadata) return OTSARDB_METADATA_INVALID;

    char key[OTSARDB_METADATA_KEY_MAX + 1];
    if (!build_key(database_root, key, sizeof(key))) return OTSARDB_METADATA_INVALID;

    otsardb_store_object object = {0};
    otsardb_store_result rc = otsardb_store_get(store, key, &object);
    if (rc == OTSARDB_STORE_NOT_FOUND) return OTSARDB_METADATA_NOT_FOUND;
    if (rc != OTSARDB_STORE_OK) return OTSARDB_METADATA_ERROR;

    int ok = otsardb_db_metadata_decode_json((const char *)object.data, object.size, out_metadata);
    otsardb_store_release_object(store, &object);
    return ok ? OTSARDB_METADATA_OK : OTSARDB_METADATA_INVALID;
}
