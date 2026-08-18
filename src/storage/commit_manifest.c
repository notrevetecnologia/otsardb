#include "commit_manifest.h"
#include "sha256.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int extract_u64(const char *json, const char *name, uint64_t *out) {
    char pattern[80];
    if (snprintf(pattern, sizeof(pattern), "\"%s\":", name) >= (int)sizeof(pattern)) return 0;
    const char *p = strstr(json, pattern);
    if (!p) return 0;
    p += strlen(pattern);
    while (*p == ' ' || *p == '\t') ++p;
    char *end = NULL;
    /* (audit MAN-2): reject out-of-range values instead of
     * silently accepting the wrapped result. strtoull returns ULLONG_MAX
     * and sets errno to ERANGE for values >= 2^64 on every CRT. Negative
     * strings are ALSO out of range for unsigned long long: glibc sets
     * ERANGE, but MSVC returns ULLONG_MAX with errno unchanged, so reject
     * a leading '-' explicitly (valid manifest fields are non-negative). */
    if (*p == '-') return 0;
    errno = 0;
    unsigned long long value = strtoull(p, &end, 10);
    if (end == p || errno == ERANGE) return 0;
    *out = (uint64_t)value;
    return 1;
}

static int frame_offsets_valid(const uint64_t *offsets, uint32_t count, uint64_t segment_size);

static int extract_string(const char *json,
                          const char *name,
                          char *out,
                          size_t out_size,
                          int allow_empty) {
    char pattern[80];
    if (snprintf(pattern, sizeof(pattern), "\"%s\":\"", name) >= (int)sizeof(pattern)) return 0;
    const char *p = strstr(json, pattern);
    if (!p) {
        /* Optional field absent. */
        if (allow_empty) {
            out[0] = '\0';
            return 1;
        }
        return 0;
    }
    p += strlen(pattern);
    const char *end = strchr(p, '"');
    if (!end) return 0;
    size_t len = (size_t)(end - p);
    if ((!allow_empty && len == 0) || len >= out_size) return 0;
    memcpy(out, p, len);
    out[len] = '\0';
    return 1;
}

static int manifest_parent_is_valid(const otsardb_commit_manifest *manifest) {
    if (manifest->parent_generation == 0) {
        if (manifest->generation != 1 || manifest->parent_commit_id[0] != '\0') return 0;
        if (manifest->format_version >= 2 && manifest->parent_manifest_sha256[0] != '\0') return 0;
        return 1;
    }

    if (manifest->parent_generation + 1u != manifest->generation ||
        manifest->parent_commit_id[0] == '\0') {
        return 0;
    }
    if (manifest->format_version >= 2 && strlen(manifest->parent_manifest_sha256) != 64) {
        return 0;
    }
    return 1;
}

static int pages_list_valid(const uint32_t *pages, uint32_t count,
                            uint64_t database_size_pages);

static int manifest_segments_valid(const otsardb_commit_manifest *manifest) {
    if (manifest->segment_count == 0 || manifest->segment_count > OTSARDB_MANIFEST_MAX_SEGMENTS) {
        return 0;
    }
    for (uint32_t i = 0; i < manifest->segment_count; ++i) {
        const otsardb_manifest_segment_ref *ref = &manifest->segments[i];
        if (ref->key[0] == '\0' || strlen(ref->sha256) != OTSARDB_MANIFEST_SHA256_HEX_SIZE) {
            return 0;
        }
        if (manifest->format_version >= 4) {
            if (ref->page_count > 0 &&
                (ref->pages == NULL ||
                 !pages_list_valid(ref->pages, ref->page_count,
                                   manifest->database_size_pages))) {
                return 0;
            }
            if (ref->frame_offsets != NULL && ref->page_count == 0) return 0;
            if (ref->frame_offsets != NULL &&
                !frame_offsets_valid(ref->frame_offsets, ref->page_count, ref->size)) {
                return 0;
            }
        }
    }
    return 1;
}

static int format_allowed(uint32_t format_version) {
    return format_version >= 1 && format_version <= OTSARDB_COMMIT_MANIFEST_VERSION;
}

static void free_segment_arrays(otsardb_manifest_segment_ref *ref) {
    free(ref->pages);
    ref->pages = NULL;
    free(ref->frame_offsets);
    ref->frame_offsets = NULL;
}

void otsardb_commit_manifest_free(otsardb_commit_manifest *manifest) {
    if (!manifest) return;
    for (uint32_t i = 0; i < manifest->segment_count; ++i) {
        free_segment_arrays(&manifest->segments[i]);
    }
    manifest->segment_count = 0;
}

/* Parse a comma-separated unsigned integer list inside brackets. *pp must
 * point just after the '['; on success *pp points just after the ']'. The
 * result array is allocated (NULL when the list is empty). */
static int parse_u64_list(const char **pp, uint64_t **out_values, size_t *out_count) {
    const char *p = *pp;
    size_t count = 0;
    size_t capacity = 0;
    uint64_t *values = NULL;
    for (;;) {
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == ']') {
            ++p;
            break;
        }
        char *end = NULL;
        /* (audit MAN-2): reject wrapped out-of-range values
         * (>= 2^64 via errno == ERANGE; negative strings via a leading '-',
         * which MSVC's strtoull accepts as ULLONG_MAX without ERANGE). */
        if (*p == '-') {
            free(values);
            return 0;
        }
        errno = 0;
        unsigned long long value = strtoull(p, &end, 10);
        if (end == p || errno == ERANGE) {
            free(values);
            return 0;
        }
        if (count == capacity) {
            /* (audit F06): the list is capped. A manifest
             * whose array exceeds the cap is REFUSED (fail-closed) instead
             * of growing without bound. Applies to every list the decoder
             * parses through this helper (pages[] and frame_offsets[]), so a
             * single oversized array cannot force a multi-million-entry
             * allocation from a crafted manifest. */
            if (count >= OTSARDB_MANIFEST_MAX_LIST_ENTRIES) {
                free(values);
                return 0;
            }
            size_t next_capacity = capacity ? capacity * 2u : 8u;
            if (next_capacity > OTSARDB_MANIFEST_MAX_LIST_ENTRIES) {
                next_capacity = OTSARDB_MANIFEST_MAX_LIST_ENTRIES;
            }
            if (next_capacity > (SIZE_MAX / sizeof(*values))) {
                free(values);
                return 0;
            }
            uint64_t *next = (uint64_t *)realloc(values, next_capacity * sizeof(*values));
            if (!next) {
                free(values);
                return 0;
            }
            values = next;
            capacity = next_capacity;
        }
        values[count++] = (uint64_t)value;
        p = end;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == ']') {
            ++p;
            break;
        }
        if (*p != ',') {
            free(values);
            return 0;
        }
        ++p;
    }
    *pp = p;
    *out_values = values;
    *out_count = count;
    return 1;
}

static int parse_u32_list(const char **pp, uint32_t **out_values, size_t *out_count) {
    uint64_t *u64s = NULL;
    size_t count = 0;
    if (!parse_u64_list(pp, &u64s, &count)) return 0;
    uint32_t *u32s = NULL;
    if (count > 0) {
        u32s = (uint32_t *)malloc(count * sizeof(uint32_t));
        if (!u32s) {
            free(u64s);
            return 0;
        }
        for (size_t i = 0; i < count; ++i) {
            if (u64s[i] > UINT32_MAX) {
                free(u32s);
                free(u64s);
                return 0;
            }
            u32s[i] = (uint32_t)u64s[i];
        }
    }
    free(u64s);
    *out_values = u32s;
    *out_count = count;
    return 1;
}

/* v4 page sets are sorted, deduped, 1-based page numbers. 
 * (audit F07): every page number is ALSO bounded by database_size_pages — a
 * crafted manifest page set beyond the declared database size would let lazy
 * hydration fseek/fwrite past the logical end of the image (sparse disk DoS
 * at lazy_hydration.c:2121-2134). Decode must refuse it fail-closed, and the
 * encoder must never emit it (encode/decode round-trip symmetry). */
static int pages_list_valid(const uint32_t *pages, uint32_t count,
                            uint64_t database_size_pages) {
    for (uint32_t i = 0; i < count; ++i) {
        if (pages[i] == 0) return 0;
        if ((uint64_t)pages[i] > database_size_pages) return 0;
        if (i > 0 && pages[i] <= pages[i - 1]) return 0;
    }
    return 1;
}

/* v4 frame offsets are byte offsets of each block within the segment object:
 * the encoder emits them strictly increasing (blocks are contiguous and
 * sorted by page) and every block lies strictly inside the object (the last
 * block ends exactly at the object size). Decode must reject what encode can
 * never produce — a non-monotonic or out-of-range array would otherwise be
 * accepted and only fail later at hydration time (MAN-1). */
static int frame_offsets_valid(const uint64_t *offsets, uint32_t count,
                               uint64_t segment_size) {
    for (uint32_t i = 0; i < count; ++i) {
        if (offsets[i] >= segment_size) return 0;
        if (i > 0 && offsets[i] <= offsets[i - 1]) return 0;
    }
    return 1;
}

/* Parse the "segments": [{"key":"...","sha256":"...","size":N, ...}, ...] array.
 * The encoder emits exactly this shape; the parser is strict about markers.
 * For v4+ each entry continues with "pages":[...] (required, may be empty)
 * and optional "frame_offsets":[...]. v1-v3 entries end after size. */
static int extract_segments(const char *json,
                            uint32_t format_version,
                            otsardb_commit_manifest *out_manifest) {
    static const char ARRAY_MARKER[] = "\"segments\":[";
    const char *p = strstr(json, ARRAY_MARKER);
    if (!p) return 0;
    p += sizeof(ARRAY_MARKER) - 1;

    uint32_t count = 0;
    while (count < OTSARDB_MANIFEST_MAX_SEGMENTS) {
        while (*p == ' ' || *p == '\t') ++p;
        if (*p != '{') goto fail;
        ++p;

        static const char KEY_MARKER[] = "\"key\":\"";
        if (strncmp(p, KEY_MARKER, sizeof(KEY_MARKER) - 1) != 0) goto fail;
        p += sizeof(KEY_MARKER) - 1;
        const char *key_end = strchr(p, '"');
        if (!key_end) goto fail;
        size_t key_len = (size_t)(key_end - p);
        if (key_len == 0 || key_len >= sizeof(out_manifest->segments[count].key)) goto fail;
        memcpy(out_manifest->segments[count].key, p, key_len);
        out_manifest->segments[count].key[key_len] = '\0';
        p = key_end + 1;

        static const char SHA_MARKER[] = ",\"sha256\":\"";
        if (strncmp(p, SHA_MARKER, sizeof(SHA_MARKER) - 1) != 0) goto fail;
        p += sizeof(SHA_MARKER) - 1;
        const char *sha_end = strchr(p, '"');
        if (!sha_end) goto fail;
        size_t sha_len = (size_t)(sha_end - p);
        if (sha_len != OTSARDB_MANIFEST_SHA256_HEX_SIZE) goto fail;
        memcpy(out_manifest->segments[count].sha256, p, sha_len);
        out_manifest->segments[count].sha256[sha_len] = '\0';
        p = sha_end + 1;

        static const char SIZE_MARKER[] = ",\"size\":";
        if (strncmp(p, SIZE_MARKER, sizeof(SIZE_MARKER) - 1) != 0) goto fail;
        p += sizeof(SIZE_MARKER) - 1;
        while (*p == ' ' || *p == '\t') ++p;
        char *size_end = NULL;
        /* (audit MAN-2): reject wrapped out-of-range sizes
         * (>= 2^64 and negative strings; MSVC's strtoull accepts '-N' as
         * ULLONG_MAX without ERANGE, so reject the sign explicitly). */
        if (*p == '-') goto fail;
        errno = 0;
        unsigned long long size_value = strtoull(p, &size_end, 10);
        if (size_end == p || errno == ERANGE) goto fail;
        out_manifest->segments[count].size = (uint64_t)size_value;
        p = size_end;

        if (format_version >= 4) {
            static const char PAGES_MARKER[] = ",\"pages\":[";
            if (strncmp(p, PAGES_MARKER, sizeof(PAGES_MARKER) - 1) != 0) goto fail;
            p += sizeof(PAGES_MARKER) - 1;
            size_t page_count = 0;
            if (!parse_u32_list(&p, &out_manifest->segments[count].pages, &page_count) ||
                page_count > UINT32_MAX) {
                goto fail;
            }
            out_manifest->segments[count].page_count = (uint32_t)page_count;

            static const char OFFSETS_MARKER[] = ",\"frame_offsets\":[";
            if (strncmp(p, OFFSETS_MARKER, sizeof(OFFSETS_MARKER) - 1) == 0) {
                p += sizeof(OFFSETS_MARKER) - 1;
                size_t offset_count = 0;
                if (!parse_u64_list(&p,
                                    &out_manifest->segments[count].frame_offsets,
                                    &offset_count)) {
                    goto fail;
                }
                /* Offsets are emitted paired with pages. */
                if (offset_count != page_count) goto fail;
                if (!frame_offsets_valid(out_manifest->segments[count].frame_offsets,
                                         (uint32_t)offset_count,
                                         out_manifest->segments[count].size)) {
                    goto fail;
                }
            }

            if (!pages_list_valid(out_manifest->segments[count].pages,
                                  out_manifest->segments[count].page_count,
                                  out_manifest->database_size_pages)) {
                goto fail;
            }
        }

        while (*p == ' ' || *p == '\t') ++p;
        if (*p != '}') goto fail;
        ++p;
        ++count;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == ']') break;
        if (*p != ',') goto fail;
        ++p;
    }
    if (count == 0 || count > OTSARDB_MANIFEST_MAX_SEGMENTS || *p != ']') goto fail;

    out_manifest->segment_count = count;
    return 1;

fail:
    /* Free arrays already parsed for segments [0..count] (count includes the
     * partially parsed current segment; its fields were zeroed by the decode
     * memset). */
    for (uint32_t i = 0; i <= count && i < OTSARDB_MANIFEST_MAX_SEGMENTS; ++i) {
        free_segment_arrays(&out_manifest->segments[i]);
    }
    return 0;
}

/* Encoded size of "[v1,v2,...]" including brackets (2 for an empty list). */
static int u32_array_json_size(const uint32_t *values, uint32_t count) {
    size_t total = 2u; /* '[' ']' */
    for (uint32_t i = 0; i < count; ++i) {
        char tmp[16];
        int digits = snprintf(tmp, sizeof(tmp), "%u", values[i]);
        if (digits <= 0 || (size_t)digits >= sizeof(tmp)) return -1;
        total += (size_t)digits;
        if (i > 0) ++total; /* ',' */
    }
    return total > (size_t)INT_MAX ? -1 : (int)total;
}

static int u64_array_json_size(const uint64_t *values, uint32_t count) {
    size_t total = 2u; /* '[' ']' */
    for (uint32_t i = 0; i < count; ++i) {
        char tmp[32];
        int digits = snprintf(tmp, sizeof(tmp), "%llu", (unsigned long long)values[i]);
        if (digits <= 0 || (size_t)digits >= sizeof(tmp)) return -1;
        total += (size_t)digits;
        if (i > 0) ++total; /* ',' */
    }
    return total > (size_t)INT_MAX ? -1 : (int)total;
}

/* Write "[v1,v2,...]" into buf (size >= encoded size). Returns bytes written
 * or -1 on truncation. */
static int write_u32_array(char *buf, size_t size, const uint32_t *values, uint32_t count) {
    size_t pos = 0;
    int n = snprintf(buf + pos, size - pos, "[");
    if (n <= 0 || (size_t)n >= size - pos) return -1;
    pos += (size_t)n;
    for (uint32_t i = 0; i < count; ++i) {
        n = snprintf(buf + pos, size - pos, "%s%u", i > 0 ? "," : "", values[i]);
        if (n <= 0 || (size_t)n >= size - pos) return -1;
        pos += (size_t)n;
    }
    n = snprintf(buf + pos, size - pos, "]");
    if (n <= 0 || (size_t)n >= size - pos) return -1;
    pos += (size_t)n;
    return (int)pos;
}

static int write_u64_array(char *buf, size_t size, const uint64_t *values, uint32_t count) {
    size_t pos = 0;
    int n = snprintf(buf + pos, size - pos, "[");
    if (n <= 0 || (size_t)n >= size - pos) return -1;
    pos += (size_t)n;
    for (uint32_t i = 0; i < count; ++i) {
        n = snprintf(buf + pos, size - pos, "%s%llu", i > 0 ? "," : "",
                     (unsigned long long)values[i]);
        if (n <= 0 || (size_t)n >= size - pos) return -1;
        pos += (size_t)n;
    }
    n = snprintf(buf + pos, size - pos, "]");
    if (n <= 0 || (size_t)n >= size - pos) return -1;
    pos += (size_t)n;
    return (int)pos;
}

/* Encoded size of one segments[] entry: "{"key":...,"sha256":...,"size":N}"
 * for v1-v3, extended with "pages":[...] (always emitted for v4, empty when
 * no page set) and optional "frame_offsets":[...] (v4, only when the page
 * set is non-empty and offsets are recorded). */
static int segment_literal_size(const otsardb_manifest_segment_ref *ref, int with_pages) {
    int literal = snprintf(
        NULL, 0,
        "{\"key\":\"%s\",\"sha256\":\"%s\",\"size\":%llu",
        ref->key,
        ref->sha256,
        (unsigned long long)ref->size);
    if (literal < 0) return -1;
    size_t total = (size_t)literal;
    if (with_pages) {
        int pages_size = u32_array_json_size(ref->pages, ref->page_count);
        if (pages_size < 0) return -1;
        total += (sizeof(",\"pages\":") - 1u) + (size_t)pages_size;
        if (ref->page_count > 0 && ref->frame_offsets) {
            int offsets_size = u64_array_json_size(ref->frame_offsets, ref->page_count);
            if (offsets_size < 0) return -1;
            total += (sizeof(",\"frame_offsets\":") - 1u) + (size_t)offsets_size;
        }
    }
    total += 1u; /* '}' */
    return total > (size_t)INT_MAX ? -1 : (int)total;
}

static int segment_literal_write(char *buf,
                                 size_t size,
                                 const otsardb_manifest_segment_ref *ref,
                                 int with_pages) {
    int n = snprintf(buf, size, "{\"key\":\"%s\",\"sha256\":\"%s\",\"size\":%llu",
                     ref->key, ref->sha256, (unsigned long long)ref->size);
    if (n <= 0 || (size_t)n >= size) return -1;
    size_t pos = (size_t)n;
    if (with_pages) {
        n = snprintf(buf + pos, size - pos, ",\"pages\":");
        if (n <= 0 || (size_t)n >= size - pos) return -1;
        pos += (size_t)n;
        n = write_u32_array(buf + pos, size - pos, ref->pages, ref->page_count);
        if (n < 0) return -1;
        pos += (size_t)n;
        if (ref->page_count > 0 && ref->frame_offsets) {
            n = snprintf(buf + pos, size - pos, ",\"frame_offsets\":");
            if (n <= 0 || (size_t)n >= size - pos) return -1;
            pos += (size_t)n;
            n = write_u64_array(buf + pos, size - pos, ref->frame_offsets, ref->page_count);
            if (n < 0) return -1;
            pos += (size_t)n;
        }
    }
    n = snprintf(buf + pos, size - pos, "}");
    if (n <= 0 || (size_t)n >= size - pos) return -1;
    pos += (size_t)n;
    return (int)pos;
}

int otsardb_commit_manifest_encode_json(const otsardb_commit_manifest *manifest,
                                      char **out_json,
                                      size_t *out_size,
                                      char out_sha256[OTSARDB_MANIFEST_SHA256_HEX_SIZE + 1]) {
    if (!manifest || !out_json || !out_size || !out_sha256 ||
        !format_allowed(manifest->format_version) ||
        manifest->generation == 0 ||
        manifest->database_id[0] == '\0' || manifest->commit_id[0] == '\0' ||
        manifest->page_size == 0 || !manifest_parent_is_valid(manifest) ||
        !manifest_segments_valid(manifest)) {
        return 0;
    }

    int needed;
    int exact_size = 0;
    if (manifest->format_version >= 3) {
        /* Compute the exact required size with snprintf(NULL, 0, ...):
         * header + one literal per segment + closing bracket/newline. */
        const char *checksum_field =
            manifest->post_apply_checksum[0] != '\0' ? ",\"post_apply_checksum\":\"%s\"" : "";
        int header_len = snprintf(
            NULL, 0,
            "{\"otsardb_format\":%u,\"database_id\":\"%s\",\"generation\":%llu,"
            "\"commit_id\":\"%s\",\"parent_generation\":%llu,"
            "\"parent_commit_id\":\"%s\",\"parent_manifest_sha256\":\"%s\","
            "\"page_size\":%u,\"database_size_pages\":%llu,\"segments\":[",
            manifest->format_version,
            manifest->database_id,
            (unsigned long long)manifest->generation,
            manifest->commit_id,
            (unsigned long long)manifest->parent_generation,
            manifest->parent_commit_id,
            manifest->parent_manifest_sha256,
            manifest->page_size,
            (unsigned long long)manifest->database_size_pages);
        if (header_len < 0) return 0;

        /* post_apply_checksum is appended after the segments array. */
        int checksum_len = manifest->post_apply_checksum[0] != '\0'
                               ? snprintf(NULL, 0, checksum_field, manifest->post_apply_checksum)
                               : 0;
        if (checksum_len < 0) return 0;

        size_t segments_bytes = 0;
        for (uint32_t i = 0; i < manifest->segment_count; ++i) {
            int literal = segment_literal_size(&manifest->segments[i],
                                               manifest->format_version >= 4);
            if (literal < 0) return 0;
            segments_bytes += (size_t)literal;
            if (i > 0) ++segments_bytes; /* ',' between entries */
        }
        size_t total = (size_t)header_len + segments_bytes + (size_t)checksum_len + 3u; /* "]\}\n" */
        if (total > (size_t)INT_MAX) return 0;
        needed = (int)total;
    } else {
        const otsardb_manifest_segment_ref *ref = &manifest->segments[0];
        if (manifest->format_version == 1) {
            needed = snprintf(NULL, 0,
                              "{\"otsardb_format\":%u,\"database_id\":\"%s\",\"generation\":%llu,"
                              "\"commit_id\":\"%s\",\"parent_generation\":%llu,"
                              "\"parent_commit_id\":\"%s\",\"segment_key\":\"%s\","
                              "\"segment_sha256\":\"%s\",\"segment_size\":%llu,"
                              "\"page_size\":%u,\"database_size_pages\":%llu}\n",
                              manifest->format_version,
                              manifest->database_id,
                              (unsigned long long)manifest->generation,
                              manifest->commit_id,
                              (unsigned long long)manifest->parent_generation,
                              manifest->parent_commit_id,
                              ref->key,
                              ref->sha256,
                              (unsigned long long)ref->size,
                              manifest->page_size,
                              (unsigned long long)manifest->database_size_pages);
        } else {
            needed = snprintf(NULL, 0,
                              "{\"otsardb_format\":%u,\"database_id\":\"%s\",\"generation\":%llu,"
                              "\"commit_id\":\"%s\",\"parent_generation\":%llu,"
                              "\"parent_commit_id\":\"%s\",\"parent_manifest_sha256\":\"%s\","
                              "\"segment_key\":\"%s\",\"segment_sha256\":\"%s\","
                              "\"segment_size\":%llu,\"page_size\":%u,"
                              "\"database_size_pages\":%llu}\n",
                              manifest->format_version,
                              manifest->database_id,
                              (unsigned long long)manifest->generation,
                              manifest->commit_id,
                              (unsigned long long)manifest->parent_generation,
                              manifest->parent_commit_id,
                              manifest->parent_manifest_sha256,
                              ref->key,
                              ref->sha256,
                              (unsigned long long)ref->size,
                              manifest->page_size,
                              (unsigned long long)manifest->database_size_pages);
        }
        exact_size = 1;
    }
    if (needed < 0) return 0;

    char *json = (char *)malloc((size_t)needed + 1u);
    if (!json) return 0;

    int written;
    if (manifest->format_version >= 3) {
        char *cursor = json;
        int n = snprintf(cursor,
                         (size_t)needed + 1u,
                         "{\"otsardb_format\":%u,\"database_id\":\"%s\",\"generation\":%llu,"
                         "\"commit_id\":\"%s\",\"parent_generation\":%llu,"
                         "\"parent_commit_id\":\"%s\",\"parent_manifest_sha256\":\"%s\","
                         "\"page_size\":%u,\"database_size_pages\":%llu,\"segments\":[",
                         manifest->format_version,
                         manifest->database_id,
                         (unsigned long long)manifest->generation,
                         manifest->commit_id,
                         (unsigned long long)manifest->parent_generation,
                         manifest->parent_commit_id,
                         manifest->parent_manifest_sha256,
                         manifest->page_size,
                         (unsigned long long)manifest->database_size_pages);
        if (n <= 0 || (size_t)n >= (size_t)needed + 1u) {
            free(json);
            return 0;
        }
        cursor += n;
        int remaining = needed + 1 - n;
        for (uint32_t i = 0; i < manifest->segment_count; ++i) {
            if (i > 0) {
                n = snprintf(cursor, (size_t)remaining, ",");
                if (n <= 0 || n >= remaining) {
                    free(json);
                    return 0;
                }
                cursor += n;
                remaining -= n;
            }
            n = segment_literal_write(cursor,
                                      (size_t)remaining,
                                      &manifest->segments[i],
                                      manifest->format_version >= 4);
            if (n <= 0 || n >= remaining) {
                free(json);
                return 0;
            }
            cursor += n;
            remaining -= n;
        }
        if (manifest->post_apply_checksum[0] != '\0') {
            n = snprintf(cursor,
                         (size_t)remaining,
                         "],\"post_apply_checksum\":\"%s\"}\n",
                         manifest->post_apply_checksum);
        } else {
            n = snprintf(cursor, (size_t)remaining, "]}\n");
        }
        if (n <= 0 || n >= remaining) {
            free(json);
            return 0;
        }
        cursor += n;
        written = (int)(cursor - json);
    } else {
        const otsardb_manifest_segment_ref *ref = &manifest->segments[0];
        if (manifest->format_version == 1) {
            written = snprintf(json,
                               (size_t)needed + 1u,
                               "{\"otsardb_format\":%u,\"database_id\":\"%s\",\"generation\":%llu,"
                               "\"commit_id\":\"%s\",\"parent_generation\":%llu,"
                               "\"parent_commit_id\":\"%s\",\"segment_key\":\"%s\","
                               "\"segment_sha256\":\"%s\",\"segment_size\":%llu,"
                               "\"page_size\":%u,\"database_size_pages\":%llu}\n",
                               manifest->format_version,
                               manifest->database_id,
                               (unsigned long long)manifest->generation,
                               manifest->commit_id,
                               (unsigned long long)manifest->parent_generation,
                               manifest->parent_commit_id,
                               ref->key,
                               ref->sha256,
                               (unsigned long long)ref->size,
                               manifest->page_size,
                               (unsigned long long)manifest->database_size_pages);
        } else {
            written = snprintf(json,
                               (size_t)needed + 1u,
                               "{\"otsardb_format\":%u,\"database_id\":\"%s\",\"generation\":%llu,"
                               "\"commit_id\":\"%s\",\"parent_generation\":%llu,"
                               "\"parent_commit_id\":\"%s\",\"parent_manifest_sha256\":\"%s\","
                               "\"segment_key\":\"%s\",\"segment_sha256\":\"%s\","
                               "\"segment_size\":%llu,\"page_size\":%u,"
                               "\"database_size_pages\":%llu}\n",
                               manifest->format_version,
                               manifest->database_id,
                               (unsigned long long)manifest->generation,
                               manifest->commit_id,
                               (unsigned long long)manifest->parent_generation,
                               manifest->parent_commit_id,
                               manifest->parent_manifest_sha256,
                               ref->key,
                               ref->sha256,
                               (unsigned long long)ref->size,
                               manifest->page_size,
                               (unsigned long long)manifest->database_size_pages);
        }
    }

    if (exact_size ? (written != needed) : (written <= 0 || written > needed)) {
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

int otsardb_commit_manifest_decode_json(const char *json,
                                      size_t size,
                                      otsardb_commit_manifest *out_manifest) {
    if (!json || size == 0 || !out_manifest) return 0;
    /* (audit MAN-3): size + 1u below would wrap for
     * size == SIZE_MAX, producing a tiny allocation followed by a SIZE_MAX
     * memcpy (heap overflow). Fail closed before any allocation. */
    if (size == SIZE_MAX) return 0;
    char *copy = (char *)malloc(size + 1u);
    if (!copy) return 0;
    memcpy(copy, json, size);
    copy[size] = '\0';

    memset(out_manifest, 0, sizeof(*out_manifest));
    uint64_t format = 0;
    uint64_t page_size = 0;
    int ok = extract_u64(copy, "otsardb_format", &format) &&
             extract_u64(copy, "generation", &out_manifest->generation) &&
             extract_u64(copy, "parent_generation", &out_manifest->parent_generation) &&
             extract_u64(copy, "page_size", &page_size) &&
             extract_u64(copy, "database_size_pages", &out_manifest->database_size_pages) &&
             extract_string(copy,
                            "database_id",
                            out_manifest->database_id,
                            sizeof(out_manifest->database_id),
                            0) &&
             extract_string(copy,
                            "commit_id",
                            out_manifest->commit_id,
                            sizeof(out_manifest->commit_id),
                            0) &&
             extract_string(copy,
                            "parent_commit_id",
                            out_manifest->parent_commit_id,
                            sizeof(out_manifest->parent_commit_id),
                            1);

    if (ok && format >= 3) {
        ok = extract_string(copy,
                            "parent_manifest_sha256",
                            out_manifest->parent_manifest_sha256,
                            sizeof(out_manifest->parent_manifest_sha256),
                            1) &&
             extract_segments(copy, (uint32_t)format, out_manifest) &&
             extract_string(copy,
                            "post_apply_checksum",
                            out_manifest->post_apply_checksum,
                            sizeof(out_manifest->post_apply_checksum),
                            1);
    } else if (ok && format == 2) {
        ok = extract_string(copy,
                            "parent_manifest_sha256",
                            out_manifest->parent_manifest_sha256,
                            sizeof(out_manifest->parent_manifest_sha256),
                            1);
    }
    free(copy);

    if (!ok || !format_allowed((uint32_t)format) ||
        page_size == 0 || page_size > UINT32_MAX ||
        out_manifest->generation == 0) {
        otsardb_commit_manifest_free(out_manifest);
        return 0;
    }

    if (format == 1 || format == 2) {
        /* Legacy flat single-segment manifests map to segments[0]. */
        char *copy2 = (char *)malloc(size + 1u);
        if (!copy2) return 0;
        memcpy(copy2, json, size);
        copy2[size] = '\0';
        int flat_ok = extract_string(copy2,
                                     "segment_key",
                                     out_manifest->segments[0].key,
                                     sizeof(out_manifest->segments[0].key),
                                     0) &&
                      extract_string(copy2,
                                     "segment_sha256",
                                     out_manifest->segments[0].sha256,
                                     sizeof(out_manifest->segments[0].sha256),
                                     0) &&
                      extract_u64(copy2, "segment_size", &out_manifest->segments[0].size);
        free(copy2);
        if (!flat_ok) {
            otsardb_commit_manifest_free(out_manifest);
            return 0;
        }
        out_manifest->segment_count = 1;
    }

    if (out_manifest->segment_count == 0 ||
        out_manifest->segment_count > OTSARDB_MANIFEST_MAX_SEGMENTS) {
        otsardb_commit_manifest_free(out_manifest);
        return 0;
    }
    for (uint32_t i = 0; i < out_manifest->segment_count; ++i) {
        if (out_manifest->segments[i].key[0] == '\0' ||
            strlen(out_manifest->segments[i].sha256) != OTSARDB_MANIFEST_SHA256_HEX_SIZE) {
            otsardb_commit_manifest_free(out_manifest);
            return 0;
        }
    }

    if (out_manifest->post_apply_checksum[0] != '\0') {
        size_t csum_len = strlen(out_manifest->post_apply_checksum);
        if (csum_len != 16) {
            otsardb_commit_manifest_free(out_manifest);
            return 0;
        }
        for (size_t i = 0; i < csum_len; ++i) {
            char c = out_manifest->post_apply_checksum[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                otsardb_commit_manifest_free(out_manifest);
                return 0;
            }
        }
    }

    out_manifest->format_version = (uint32_t)format;
    out_manifest->page_size = (uint32_t)page_size;
    if (!manifest_parent_is_valid(out_manifest)) {
        otsardb_commit_manifest_free(out_manifest);
        return 0;
    }
    return 1;
}
