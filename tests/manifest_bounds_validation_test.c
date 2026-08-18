/* manifest_bounds_validation_test.c — decode-time manifest validation
 * regression tests for , audit findings MAN-2, MAN-3 and F07.
 *
 * F07: v4 page sets are bounded by database_size_pages. A crafted
 * manifest whose pages[] contains a value beyond the declared
 * database size must be REFUSED at decode (previously only
 * sorted/1-based/deduped were checked); the encoder must refuse to
 * emit such a manifest too (round-trip symmetry). Without the bound,
 * lazy hydration's fseek/fwrite at (page_no-1)*page_size can extend
 * the local image past the logical end (sparse disk DoS,
 * lazy_hydration.c:2121-2134).
 * MAN-2: strtoull results >= 2^64 (and negative strings) silently wrap to
 * ULLONG_MAX - k without an errno == ERANGE check. A manifest with
 * "generation":18446744073709551616 must be refused, not decoded
 * with a wrapped generation.
 * MAN-3: decode copies the input with malloc(size + 1); for size ==
 * SIZE_MAX that wraps to a tiny allocation followed by a huge
 * memcpy (heap overflow). The decoder must refuse size == SIZE_MAX
 * before any allocation.
 *
 * Deterministic: direct encode/decode only, no network, no sleeps.
 *
 * Wiring (coordinator — , NOT applied here; CMakeLists.txt is
 * out of scope for this change set):
 * add_executable(otsardb_manifest_bounds_validation_test
 * tests/manifest_bounds_validation_test.c)
 * target_include_directories(otsardb_manifest_bounds_validation_test
 * PRIVATE include src/core src/crypto src/storage tests)
 * target_link_libraries(otsardb_manifest_bounds_validation_test
 * PRIVATE otsardb_core)
 * add_test(NAME otsardb_manifest_bounds_validation
 * COMMAND otsardb_manifest_bounds_validation_test)
 * set_tests_properties(otsardb_manifest_bounds_validation PROPERTIES
 * TIMEOUT 120)
 */
#include "commit_manifest.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *dup64hex(void) {
    char *out = (char *)malloc(65);
    assert(out);
    memset(out, 'a', 64);
    out[64] = '\0';
    return out;
}

/* Build a v4 genesis manifest JSON. pages_json is the raw "pages":[...]
 * literal INCLUDING the key (e.g. "pages":[1,2,3]); an empty string omits
 * the array entirely. database_size_pages is rendered literally (so the test
 * can inject an out-of-range number). */
static char *build_v4_manifest(const char *database_size_pages,
                               const char *segments_suffix) {
    char *sha = dup64hex();
    const char *fmt =
        "{\"otsardb_format\":4,\"database_id\":\"m0145-db\",\"generation\":1,"
        "\"commit_id\":\"m0145-commit\",\"parent_generation\":0,"
        "\"parent_commit_id\":\"\",\"parent_manifest_sha256\":\"\","
        "\"page_size\":4096,\"database_size_pages\":%s,\"segments\":[%s]}\n";
    int needed = snprintf(NULL, 0, fmt, database_size_pages, segments_suffix);
    assert(needed > 0);
    char *json = (char *)malloc((size_t)needed + 1u);
    assert(json);
    int written = snprintf(json, (size_t)needed + 1u, fmt,
                           database_size_pages, segments_suffix);
    assert(written == needed);
    free(sha);
    return json;
}

static char *segment_literal(const char *pages_json) {
    char *sha = dup64hex();
    const char *fmt =
        "{\"key\":\"segments/sha256/aa/aa.otsseg\",\"sha256\":\"%s\","
        "\"size\":1000%s}";
    int needed = snprintf(NULL, 0, fmt, sha, pages_json);
    assert(needed > 0);
    char *out = (char *)malloc((size_t)needed + 1u);
    assert(out);
    int written = snprintf(out, (size_t)needed + 1u, fmt, sha, pages_json);
    assert(written == needed);
    free(sha);
    return out;
}

/* ------------------------------------------------------------------ F07 */

/* A page number strictly beyond database_size_pages must be refused at
 * decode, regardless of how "valid" the rest of the manifest is. */
static void test_f07_decode_refuses_out_of_range_pages(void) {
    char *seg = segment_literal(",\"pages\":[1,2,101]");
    char *json = build_v4_manifest("100", seg);
    size_t size = strlen(json);
    otsardb_commit_manifest manifest;
    assert(otsardb_commit_manifest_decode_json(json, size, &manifest) == 0);
    free(json);
    free(seg);

    /* Boundary: page number == database_size_pages is still valid. */
    seg = segment_literal(",\"pages\":[1,2,100]");
    json = build_v4_manifest("100", seg);
    size = strlen(json);
    assert(otsardb_commit_manifest_decode_json(json, size, &manifest));
    assert(manifest.segments[0].page_count == 3);
    assert(manifest.segments[0].pages[2] == 100);
    otsardb_commit_manifest_free(&manifest);
    free(json);
    free(seg);

    /* A huge but in-range page (database of 2^32 pages) is accepted. */
    seg = segment_literal(",\"pages\":[1,1000]");
    json = build_v4_manifest("4294967296", seg);
    size = strlen(json);
    assert(otsardb_commit_manifest_decode_json(json, size, &manifest));
    assert(manifest.segments[0].page_count == 2);
    otsardb_commit_manifest_free(&manifest);
    free(json);
    free(seg);
}

/* The encoder must refuse to emit a page set beyond the declared database
 * size: otherwise it could publish a manifest that decode now rejects. */
static void test_f07_encode_refuses_out_of_range_pages(void) {
    char *sha = dup64hex();
    otsardb_commit_manifest manifest;
    memset(&manifest, 0, sizeof(manifest));
    manifest.format_version = 4;
    manifest.generation = 1;
    manifest.parent_generation = 0;
    manifest.database_size_pages = 100;
    manifest.page_size = 4096;
    strcpy(manifest.database_id, "m0145-db");
    strcpy(manifest.commit_id, "m0145-commit");
    manifest.segment_count = 1;
    strcpy(manifest.segments[0].key, "segments/sha256/aa/aa.otsseg");
    strcpy(manifest.segments[0].sha256, sha);
    manifest.segments[0].size = 1000;
    static const uint32_t bad_pages[3] = { 1, 2, 101 };
    manifest.segments[0].page_count = 3;
    manifest.segments[0].pages = (uint32_t *)bad_pages;

    char *json = NULL;
    size_t json_size = 0;
    char sha_out[OTSARDB_MANIFEST_SHA256_HEX_SIZE + 1];
    assert(otsardb_commit_manifest_encode_json(&manifest, &json, &json_size,
                                               sha_out) == 0);

    /* In-range pages still encode and round-trip. */
    static const uint32_t good_pages[3] = { 1, 2, 100 };
    manifest.segments[0].pages = (uint32_t *)good_pages;
    assert(otsardb_commit_manifest_encode_json(&manifest, &json, &json_size,
                                               sha_out));
    otsardb_commit_manifest decoded;
    assert(otsardb_commit_manifest_decode_json(json, json_size, &decoded));
    assert(decoded.segments[0].page_count == 3);
    assert(decoded.segments[0].pages[2] == 100);
    otsardb_commit_manifest_free(&decoded);
    free(json);
    free(sha);
}

/* ----------------------------------------------------------------- MAN-2 */

/* strtoull overflow (>= 2^64) and negative strings must be refused, not
 * accepted with a silently wrapped value. The discriminating field is
 * database_size_pages (a u64 stored directly; a wrapped value would decode
 * as ULLONG_MAX - k). */
static void test_man2_strtoull_overflow_refused(void) {
    /* 2^64 exactly -> strtoull returns ULLONG_MAX, errno == ERANGE. */
    char *seg = segment_literal(",\"pages\":[1]");
    char *json = build_v4_manifest("18446744073709551616", seg);
    size_t size = strlen(json);
    otsardb_commit_manifest manifest;
    assert(otsardb_commit_manifest_decode_json(json, size, &manifest) == 0);
    free(json);
    free(seg);

    /* 2^64 + 1 -> ERANGE as well. */
    seg = segment_literal(",\"pages\":[1]");
    json = build_v4_manifest("18446744073709551617", seg);
    size = strlen(json);
    assert(otsardb_commit_manifest_decode_json(json, size, &manifest) == 0);
    free(json);
    free(seg);

    /* Negative string: "-1" -> strtoull sets ERANGE (the negated value is
     * not representable in unsigned long long). */
    seg = segment_literal(",\"pages\":[1]");
    json = build_v4_manifest("-1", seg);
    size = strlen(json);
    assert(otsardb_commit_manifest_decode_json(json, size, &manifest) == 0);
    free(json);
    free(seg);

    /* An out-of-range value in the segment "size" field is refused too. */
    char *seg2 = segment_literal(",\"pages\":[1]");
    free(seg2);
    {
        /* size = 2^64 in a segment literal. */
        char *sha = dup64hex();
        char lit[400];
        snprintf(lit, sizeof(lit),
                 "{\"key\":\"segments/sha256/aa/aa.otsseg\",\"sha256\":\"%s\","
                 "\"size\":18446744073709551616,\"pages\":[1]}",
                 sha);
        free(sha);
        json = build_v4_manifest("100", lit);
        size = strlen(json);
        assert(otsardb_commit_manifest_decode_json(json, size, &manifest) == 0);
        free(json);
    }

    /* ULLONG_MAX (the largest representable u64) is still accepted. */
    seg = segment_literal(",\"pages\":[1]");
    json = build_v4_manifest("18446744073709551615", seg);
    size = strlen(json);
    assert(otsardb_commit_manifest_decode_json(json, size, &manifest));
    assert(manifest.database_size_pages == UINT64_MAX);
    otsardb_commit_manifest_free(&manifest);
    free(json);
    free(seg);
}

/* ----------------------------------------------------------------- MAN-3 */

/* size == SIZE_MAX must be refused before any allocation/copy (malloc(size+1)
 * would wrap to a tiny allocation followed by a SIZE_MAX-byte memcpy). */
static void test_man3_size_max_refused(void) {
    const char *json = "{}";
    otsardb_commit_manifest manifest;
    assert(otsardb_commit_manifest_decode_json(json, SIZE_MAX, &manifest) == 0);
    assert(otsardb_commit_manifest_decode_json(json, SIZE_MAX - 1u, &manifest) == 0);
}

/* ----------------------------------------------------------------- main */

int main(void) {
    test_f07_decode_refuses_out_of_range_pages();
    test_f07_encode_refuses_out_of_range_pages();
    test_man2_strtoull_overflow_refused();
    test_man3_size_max_refused();

    puts("manifest bounds validation regression tests passed (F07, MAN-2, MAN-3)");
    return 0;
}
