#include "checkpoint_manifest.h"
#include "sha256.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Every checkpoint page-group object published by this tree is a fixed
 * 32-byte header followed by k >= 1 records of exactly (record header +
 * page_size) bytes (checkpoint_page.c encode; flags always 0). The record
 * header is three u32 fields (page_no, flags, stored_size). The header size
 * matches OTSARDB_CP_HEADER_SIZE in checkpoint_page.c and must stay in sync
 * with it. */
#define OTSARDB_CHECKPOINT_PAGE_HEADER_SIZE 32u
#define OTSARDB_CHECKPOINT_PAGE_RECORD_HEADER_SIZE 12u

/* A page-group object whose size does not match that layout cannot be a
 * valid record set (B6 hardening): reject at
 * encode/decode time instead of writing mis-sized records during apply. */
static int page_group_size_valid(uint64_t size, uint32_t page_size) {
    uint64_t record_size = OTSARDB_CHECKPOINT_PAGE_RECORD_HEADER_SIZE +
                           (uint64_t)page_size;
    if (size < (uint64_t)OTSARDB_CHECKPOINT_PAGE_HEADER_SIZE + record_size) return 0;
    return (size - (uint64_t)OTSARDB_CHECKPOINT_PAGE_HEADER_SIZE) % record_size == 0;
}

/* --- JSON helpers --- */

static int extract_u64(const char *json, const char *name, uint64_t *out) {
    char pattern[80];
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

static int extract_string(const char *json,
                          const char *name,
                          char *out,
                          size_t out_size,
                          int allow_empty) {
    char pattern[80];
    if (snprintf(pattern, sizeof(pattern), "\"%s\":\"", name) >= (int)sizeof(pattern)) return 0;
    const char *p = strstr(json, pattern);
    if (!p) return 0;
    p += strlen(pattern);
    const char *end = strchr(p, '"');
    if (!end) return 0;
    size_t len = (size_t)(end - p);
    if ((!allow_empty && len == 0) || len >= out_size) return 0;
    memcpy(out, p, len);
    out[len] = '\0';
    return 1;
}

static int extract_page_groups(const char *json, otsardb_checkpoint_manifest *out_manifest) {
    static const char ARRAY_MARKER[] = "\"page_groups\":[";
    const char *p = strstr(json, ARRAY_MARKER);
    if (!p) return 0;
    p += sizeof(ARRAY_MARKER) - 1;

    uint32_t count = 0;
    while (count < OTSARDB_CHECKPOINT_MAX_PAGE_GROUPS) {
        while (*p == ' ' || *p == '\t') ++p;
        if (*p != '{') return 0;
        ++p;

        static const char KEY_MARKER[] = "\"key\":\"";
        if (strncmp(p, KEY_MARKER, sizeof(KEY_MARKER) - 1) != 0) return 0;
        p += sizeof(KEY_MARKER) - 1;
        const char *key_end = strchr(p, '"');
        if (!key_end) return 0;
        size_t key_len = (size_t)(key_end - p);
        if (key_len == 0 || key_len >= sizeof(out_manifest->page_groups[count].key)) return 0;
        memcpy(out_manifest->page_groups[count].key, p, key_len);
        out_manifest->page_groups[count].key[key_len] = '\0';
        p = key_end + 1;

        static const char SHA_MARKER[] = ",\"sha256\":\"";
        if (strncmp(p, SHA_MARKER, sizeof(SHA_MARKER) - 1) != 0) return 0;
        p += sizeof(SHA_MARKER) - 1;
        const char *sha_end = strchr(p, '"');
        if (!sha_end) return 0;
        size_t sha_len = (size_t)(sha_end - p);
        if (sha_len != OTSARDB_CHECKPOINT_SHA256_HEX_SIZE) return 0;
        memcpy(out_manifest->page_groups[count].sha256, p, sha_len);
        out_manifest->page_groups[count].sha256[sha_len] = '\0';
        p = sha_end + 1;

        static const char SIZE_MARKER[] = ",\"size\":";
        if (strncmp(p, SIZE_MARKER, sizeof(SIZE_MARKER) - 1) != 0) return 0;
        p += sizeof(SIZE_MARKER) - 1;
        while (*p == ' ' || *p == '\t') ++p;
        char *size_end = NULL;
        unsigned long long size_value = strtoull(p, &size_end, 10);
        if (size_end == p) return 0;
        out_manifest->page_groups[count].size = (uint64_t)size_value;
        p = size_end;

        while (*p == ' ' || *p == '\t') ++p;
        if (*p != '}') return 0;
        ++p;
        ++count;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == ']') break;
        if (*p != ',') return 0;
        ++p;
    }
    if (count == 0 || count > OTSARDB_CHECKPOINT_MAX_PAGE_GROUPS || *p != ']') return 0;

    out_manifest->page_group_count = count;
    return 1;
}

/* --- Manifest encode/decode --- */

int otsardb_checkpoint_manifest_encode(const otsardb_checkpoint_manifest *manifest,
                                     char **out_json,
                                     size_t *out_size,
                                     char out_sha256[OTSARDB_CHECKPOINT_SHA256_HEX_SIZE + 1]) {
    if (!manifest || !out_json || !out_size || !out_sha256 ||
        manifest->format_version != OTSARDB_CHECKPOINT_MANIFEST_FORMAT_VERSION ||
        manifest->database_id[0] == '\0' ||
        manifest->base_commit_id[0] == '\0' ||
        manifest->page_size == 0 ||
        manifest->page_group_count == 0 ||
        manifest->page_group_count > OTSARDB_CHECKPOINT_MAX_PAGE_GROUPS) {
        return 0;
    }

    for (uint32_t i = 0; i < manifest->page_group_count; ++i) {
        const otsardb_checkpoint_page_group_ref *ref = &manifest->page_groups[i];
        if (ref->key[0] == '\0' ||
            strlen(ref->sha256) != OTSARDB_CHECKPOINT_SHA256_HEX_SIZE ||
            !page_group_size_valid(ref->size, manifest->page_size)) {
            return 0;
        }
    }

    int header_len = snprintf(
        NULL, 0,
        "{\"otsardb_checkpoint_format\":%u,\"database_id\":\"%s\","
        "\"covered_generation\":%llu,\"base_commit_id\":\"%s\","
        "\"base_manifest_sha256\":\"%s\",\"page_size\":%u,"
        "\"database_size_pages\":%llu,\"page_groups\":[",
        manifest->format_version,
        manifest->database_id,
        (unsigned long long)manifest->covered_generation,
        manifest->base_commit_id,
        manifest->base_manifest_sha256,
        manifest->page_size,
        (unsigned long long)manifest->database_size_pages);
    if (header_len < 0) return 0;

    size_t groups_bytes = 0;
    for (uint32_t i = 0; i < manifest->page_group_count; ++i) {
        int literal = snprintf(
            NULL, 0,
            "%s{\"key\":\"%s\",\"sha256\":\"%s\",\"size\":%llu}",
            i == 0 ? "" : ",",
            manifest->page_groups[i].key,
            manifest->page_groups[i].sha256,
            (unsigned long long)manifest->page_groups[i].size);
        if (literal < 0) return 0;
        groups_bytes += (size_t)literal;
    }

    size_t total = (size_t)header_len + groups_bytes + 3u;
    if (total > (size_t)INT_MAX) return 0;
    int needed = (int)total;

    char *json = (char *)malloc((size_t)needed + 1u);
    if (!json) return 0;

    char *cursor = json;
    int n = snprintf(cursor,
                     (size_t)needed + 1u,
                     "{\"otsardb_checkpoint_format\":%u,\"database_id\":\"%s\","
                     "\"covered_generation\":%llu,\"base_commit_id\":\"%s\","
                     "\"base_manifest_sha256\":\"%s\",\"page_size\":%u,"
                     "\"database_size_pages\":%llu,\"page_groups\":[",
                     manifest->format_version,
                     manifest->database_id,
                     (unsigned long long)manifest->covered_generation,
                     manifest->base_commit_id,
                     manifest->base_manifest_sha256,
                     manifest->page_size,
                     (unsigned long long)manifest->database_size_pages);
    if (n <= 0 || (size_t)n >= (size_t)needed + 1u) {
        free(json);
        return 0;
    }
    cursor += n;
    int remaining = needed + 1 - n;
    for (uint32_t i = 0; i < manifest->page_group_count; ++i) {
        n = snprintf(cursor,
                     (size_t)remaining,
                     "%s{\"key\":\"%s\",\"sha256\":\"%s\",\"size\":%llu}",
                     i == 0 ? "" : ",",
                     manifest->page_groups[i].key,
                     manifest->page_groups[i].sha256,
                     (unsigned long long)manifest->page_groups[i].size);
        if (n <= 0 || n >= remaining) {
            free(json);
            return 0;
        }
        cursor += n;
        remaining -= n;
    }
    n = snprintf(cursor, (size_t)remaining, "]}\n");
    if (n <= 0 || n >= remaining) {
        free(json);
        return 0;
    }
    cursor += n;

    int written = (int)(cursor - json);
    if (written != needed) {
        free(json);
        return 0;
    }

    if (!otsardb_sha256_hex_data(json, (size_t)written, out_sha256)) {
        free(json);
        return 0;
    }

    *out_json = json;
    *out_size = (size_t)written;
    return 1;
}

int otsardb_checkpoint_manifest_decode(const char *json, size_t size,
                                     otsardb_checkpoint_manifest *out) {
    if (!json || size == 0 || !out) return 0;
    char *copy = (char *)malloc(size + 1u);
    if (!copy) return 0;
    memcpy(copy, json, size);
    copy[size] = '\0';

    memset(out, 0, sizeof(*out));
    uint64_t format = 0;
    uint64_t page_size = 0;
    int ok = extract_u64(copy, "otsardb_checkpoint_format", &format) &&
             extract_u64(copy, "covered_generation", &out->covered_generation) &&
             extract_u64(copy, "page_size", &page_size) &&
             extract_u64(copy, "database_size_pages", &out->database_size_pages) &&
             extract_string(copy,
                            "database_id",
                            out->database_id,
                            sizeof(out->database_id),
                            0) &&
             extract_string(copy,
                            "base_commit_id",
                            out->base_commit_id,
                            sizeof(out->base_commit_id),
                            0) &&
             extract_string(copy,
                            "base_manifest_sha256",
                            out->base_manifest_sha256,
                            sizeof(out->base_manifest_sha256),
                            1) &&
             extract_page_groups(copy, out);
    free(copy);

    if (!ok || format != OTSARDB_CHECKPOINT_MANIFEST_FORMAT_VERSION ||
        page_size == 0 || page_size > UINT32_MAX ||
        out->covered_generation == 0) {
        return 0;
    }
    for (uint32_t i = 0; i < out->page_group_count; ++i) {
        if (!page_group_size_valid(out->page_groups[i].size, (uint32_t)page_size)) {
            return 0;
        }
    }

    out->format_version = (uint32_t)format;
    out->page_size = (uint32_t)page_size;
    return 1;
}

/* --- CHECKPOINT.json pointer helpers --- */

static char *strdup_ptr(const char *value) {
    if (!value) return NULL;
    size_t len = strlen(value) + 1;
    char *copy = (char *)malloc(len);
    if (copy) memcpy(copy, value, len);
    return copy;
}

static int build_checkpoint_key(const char *root, char *out, size_t out_size) {
    if (!root || !out || out_size == 0) return 0;
    size_t len = strlen(root);
    int needs_slash = len > 0 && root[len - 1] != '/';
    int n = snprintf(out, out_size, "%s%sCHECKPOINT.json", root, needs_slash ? "/" : "");
    return n > 0 && (size_t)n < out_size;
}

int otsardb_checkpoint_ptr_encode(const otsardb_checkpoint_pointer *ptr,
                                char **out_json, size_t *out_size) {
    if (!ptr || !out_json || !out_size) return 0;

    int needed = snprintf(NULL, 0,
        "{\"otsardb_checkpoint_format\":1,\"database_id\":\"%s\","
        "\"covered_generation\":%llu,\"checkpoint_sha256\":\"%s\","
        "\"checkpoint_key\":\"%s\"}\n",
        ptr->database_id,
        (unsigned long long)ptr->covered_generation,
        ptr->checkpoint_sha256,
        ptr->checkpoint_key);
    if (needed < 0) return 0;

    char *json = (char *)malloc((size_t)needed + 1);
    if (!json) return 0;

    int written = snprintf(json, (size_t)needed + 1,
        "{\"otsardb_checkpoint_format\":1,\"database_id\":\"%s\","
        "\"covered_generation\":%llu,\"checkpoint_sha256\":\"%s\","
        "\"checkpoint_key\":\"%s\"}\n",
        ptr->database_id,
        (unsigned long long)ptr->covered_generation,
        ptr->checkpoint_sha256,
        ptr->checkpoint_key);

    if (written != needed) {
        free(json);
        return 0;
    }

    *out_json = json;
    *out_size = (size_t)written;
    return 1;
}

int otsardb_checkpoint_ptr_decode(const char *json, size_t size,
                                otsardb_checkpoint_pointer *out) {
    if (!json || !out || size == 0) return 0;

    char *copy = (char *)malloc(size + 1);
    if (!copy) return 0;
    memcpy(copy, json, size);
    copy[size] = '\0';

    memset(out, 0, sizeof(*out));
    uint64_t format = 0;
    int ok = extract_u64(copy, "otsardb_checkpoint_format", &format) &&
             extract_u64(copy, "covered_generation", &out->covered_generation) &&
             extract_string(copy, "database_id", out->database_id, sizeof(out->database_id), 0) &&
             extract_string(copy, "checkpoint_sha256", out->checkpoint_sha256, sizeof(out->checkpoint_sha256), 0) &&
             extract_string(copy, "checkpoint_key", out->checkpoint_key, sizeof(out->checkpoint_key), 0);
    free(copy);

    if (!ok || format == 0 || format > UINT32_MAX) return 0;
    return 1;
}

otsardb_checkpoint_ptr_result otsardb_checkpoint_ptr_read(otsardb_object_store *store,
                                                       const char *database_root,
                                                       otsardb_checkpoint_pointer *out,
                                                       char **out_etag) {
    if (!store || !database_root || !out || !out_etag) return OTSARDB_CHECKPOINT_PTR_ERROR;
    *out_etag = NULL;

    char key[OTSARDB_KEY_MAX + 1];
    if (!build_checkpoint_key(database_root, key, sizeof(key))) return OTSARDB_CHECKPOINT_PTR_ERROR;

    otsardb_store_object object = {0};
    otsardb_store_result rc = otsardb_store_get(store, key, &object);
    if (rc == OTSARDB_STORE_NOT_FOUND) return OTSARDB_CHECKPOINT_PTR_NOT_FOUND;
    if (rc != OTSARDB_STORE_OK) return OTSARDB_CHECKPOINT_PTR_ERROR;

    int decoded = otsardb_checkpoint_ptr_decode((const char *)object.data, object.size, out);
    if (decoded && object.etag && object.etag[0] != '\0') {
        *out_etag = strdup_ptr(object.etag);
        if (!*out_etag) decoded = 0;
    } else if (decoded) {
        decoded = 0;
    }

    otsardb_store_release_object(store, &object);
    return decoded ? OTSARDB_CHECKPOINT_PTR_OK : OTSARDB_CHECKPOINT_PTR_ERROR;
}

otsardb_journal_result otsardb_checkpoint_ptr_publish(otsardb_object_store *store,
                                                   const char *database_root,
                                                   const otsardb_checkpoint_pointer *ptr,
                                                   const char *expected_etag,
                                                   int create_if_missing,
                                                   char **out_etag) {
    if (!store || !database_root || !ptr) return OTSARDB_JOURNAL_INVALID;
    if (!create_if_missing && !expected_etag) return OTSARDB_JOURNAL_INVALID;
    if (out_etag) *out_etag = NULL;

    char key[OTSARDB_KEY_MAX + 1];
    if (!build_checkpoint_key(database_root, key, sizeof(key))) return OTSARDB_JOURNAL_INVALID;

    char *json = NULL;
    size_t json_size = 0;
    if (!otsardb_checkpoint_ptr_encode(ptr, &json, &json_size)) return OTSARDB_JOURNAL_ERROR;

    otsardb_store_put_options options = {0};
    options.if_none_match = create_if_missing ? 1 : 0;
    options.if_match_etag = create_if_missing ? NULL : expected_etag;

    char *etag = NULL;
    otsardb_store_result rc = otsardb_store_put(store, key, json, json_size, &options, &etag);
    free(json);

    if (rc == OTSARDB_STORE_PRECONDITION_FAILED) {
        free(etag);
        return OTSARDB_JOURNAL_CONFLICT;
    }
    if (rc == OTSARDB_STORE_ERROR || rc == OTSARDB_STORE_UNSUPPORTED || rc == OTSARDB_STORE_TIMEOUT) {
        free(etag);
        return (rc == OTSARDB_STORE_TIMEOUT) ? OTSARDB_JOURNAL_AMBIGUOUS
               : (rc == OTSARDB_STORE_UNSUPPORTED) ? OTSARDB_JOURNAL_UNSUPPORTED
               : OTSARDB_JOURNAL_ERROR;
    }

    if (out_etag) *out_etag = etag;
    else free(etag);
    return OTSARDB_JOURNAL_OK;
}
