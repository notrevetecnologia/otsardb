#include "journal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *otsardb_strdup(const char *value) {
    if (!value) return NULL;
    size_t len = strlen(value) + 1;
    char *copy = (char *)malloc(len);
    if (copy) memcpy(copy, value, len);
    return copy;
}

static int build_head_key(const char *root, char *out, size_t out_size) {
    if (!root || !out || out_size == 0) return 0;
    size_t len = strlen(root);
    int needs_slash = len > 0 && root[len - 1] != '/';
    int n = snprintf(out, out_size, "%s%sHEAD.json", root, needs_slash ? "/" : "");
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
    if (len >= out_size) return 0;
    memcpy(out, p, len);
    out[len] = '\0';
    return 1;
}

int otsardb_head_encode_json(const otsardb_head *head, char **out_json, size_t *out_size) {
    if (!head || !out_json || !out_size) return 0;

    int needed = snprintf(NULL, 0,
        "{\"otsardb_format\":%u,\"database_id\":\"%s\",\"generation\":%llu,"
        "\"commit_id\":\"%s\",\"commit_key\":\"%s\",\"commit_sha256\":\"%s\"}\n",
        head->format_version,
        head->database_id,
        (unsigned long long)head->generation,
        head->commit_id,
        head->commit_key,
        head->commit_sha256);
    if (needed < 0) return 0;

    char *json = (char *)malloc((size_t)needed + 1);
    if (!json) return 0;

    int written = snprintf(json, (size_t)needed + 1,
        "{\"otsardb_format\":%u,\"database_id\":\"%s\",\"generation\":%llu,"
        "\"commit_id\":\"%s\",\"commit_key\":\"%s\",\"commit_sha256\":\"%s\"}\n",
        head->format_version,
        head->database_id,
        (unsigned long long)head->generation,
        head->commit_id,
        head->commit_key,
        head->commit_sha256);

    if (written != needed) {
        free(json);
        return 0;
    }

    *out_json = json;
    *out_size = (size_t)written;
    return 1;
}

int otsardb_head_decode_json(const char *json, size_t size, otsardb_head *out_head) {
    if (!json || !out_head || size == 0) return 0;

    char *copy = (char *)malloc(size + 1);
    if (!copy) return 0;
    memcpy(copy, json, size);
    copy[size] = '\0';

    memset(out_head, 0, sizeof(*out_head));
    uint64_t format = 0;
    int ok = extract_u64(copy, "otsardb_format", &format) &&
             extract_u64(copy, "generation", &out_head->generation) &&
             extract_string(copy, "database_id", out_head->database_id, sizeof(out_head->database_id)) &&
             extract_string(copy, "commit_id", out_head->commit_id, sizeof(out_head->commit_id)) &&
             extract_string(copy, "commit_key", out_head->commit_key, sizeof(out_head->commit_key)) &&
             extract_string(copy, "commit_sha256", out_head->commit_sha256, sizeof(out_head->commit_sha256));
    free(copy);

    if (!ok || format == 0 || format > UINT32_MAX) return 0;
    out_head->format_version = (uint32_t)format;
    return 1;
}

otsardb_journal_result otsardb_journal_read_head(otsardb_object_store *store,
                                              const char *database_root,
                                              otsardb_head *out_head,
                                              char **out_etag) {
    if (!store || !database_root || !out_head || !out_etag) return OTSARDB_JOURNAL_INVALID;
    *out_etag = NULL;

    char key[OTSARDB_KEY_MAX + 1];
    if (!build_head_key(database_root, key, sizeof(key))) return OTSARDB_JOURNAL_INVALID;

    otsardb_store_object object = {0};
    otsardb_store_result rc = otsardb_store_get(store, key, &object);
    if (rc == OTSARDB_STORE_NOT_FOUND) return OTSARDB_JOURNAL_NOT_FOUND;
    if (rc == OTSARDB_STORE_UNSUPPORTED) return OTSARDB_JOURNAL_UNSUPPORTED;
    if (rc != OTSARDB_STORE_OK) return OTSARDB_JOURNAL_ERROR;

    int decoded = otsardb_head_decode_json((const char *)object.data, object.size, out_head);
    if (decoded && object.etag && object.etag[0] != '\0') {
        *out_etag = otsardb_strdup(object.etag);
        if (!*out_etag) decoded = 0;
    } else if (decoded) {
        decoded = 0;
    }

    otsardb_store_release_object(store, &object);
    return decoded ? OTSARDB_JOURNAL_OK : OTSARDB_JOURNAL_INVALID;
}

static otsardb_journal_result publish_head_json(otsardb_object_store *store,
                                               const char *database_root,
                                               const otsardb_head *head,
                                               const otsardb_store_put_options *options,
                                               char **out_etag) {
    if (out_etag) *out_etag = NULL;

    char key[OTSARDB_KEY_MAX + 1];
    if (!build_head_key(database_root, key, sizeof(key))) return OTSARDB_JOURNAL_INVALID;

    char *json = NULL;
    size_t json_size = 0;
    if (!otsardb_head_encode_json(head, &json, &json_size)) return OTSARDB_JOURNAL_ERROR;

    char *etag = NULL;
    otsardb_store_result rc = otsardb_store_put(store, key, json, json_size, options, &etag);
    free(json);

    if (rc == OTSARDB_STORE_PRECONDITION_FAILED) {
        free(etag);
        return OTSARDB_JOURNAL_CONFLICT;
    }
    if (rc == OTSARDB_STORE_TIMEOUT) {
        free(etag);
        return OTSARDB_JOURNAL_AMBIGUOUS;
    }
    if (rc == OTSARDB_STORE_UNSUPPORTED) {
        free(etag);
        return OTSARDB_JOURNAL_UNSUPPORTED;
    }
    if (rc != OTSARDB_STORE_OK) {
        free(etag);
        return OTSARDB_JOURNAL_ERROR;
    }

    if (out_etag) *out_etag = etag;
    else free(etag);
    return OTSARDB_JOURNAL_OK;
}

otsardb_journal_result otsardb_journal_publish_head(otsardb_object_store *store,
                                                 const char *database_root,
                                                 const otsardb_head *head,
                                                 const char *expected_etag,
                                                 int create_if_missing,
                                                 char **out_etag) {
    if (!store || !database_root || !head) return OTSARDB_JOURNAL_INVALID;
    if (!create_if_missing && !expected_etag) return OTSARDB_JOURNAL_INVALID;

    otsardb_store_put_options options = {0};
    options.if_none_match = create_if_missing ? 1 : 0;
    options.if_match_etag = create_if_missing ? NULL : expected_etag;
    return publish_head_json(store, database_root, head, &options, out_etag);
}

otsardb_journal_result otsardb_journal_publish_head_unconditional(otsardb_object_store *store,
                                                               const char *database_root,
                                                               const otsardb_head *head,
                                                               char **out_etag) {
    if (!store || !database_root || !head) return OTSARDB_JOURNAL_INVALID;
    otsardb_store_put_options options = {0};
    return publish_head_json(store, database_root, head, &options, out_etag);
}
