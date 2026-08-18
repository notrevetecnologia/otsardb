/* manifest_list_cap_test.c — regression test for 
 * 0151, audit finding F06 second half: parse_u64_list in commit_manifest.c
 * grew without a count cap.
 *
 * F06 (SECURITY_AUDIT.md, LOW, defense-in-depth): parse_u64_list (pages[] /
 * frame_offsets[] arrays) grew one entry per parsed number with no count cap
 * of its own — only the linear JSON size bounded it, and that only when the
 * object came from a capped store GET. A crafted manifest could force a
 * multi-million-entry allocation. adds the named constant
 * OTSARDB_MANIFEST_MAX_LIST_ENTRIES (1 000 000) and fails closed when a list
 * exceeds it.
 *
 * This test drives otsardb_commit_manifest_decode_json with v4 manifests
 * whose pages[] / frame_offsets[] sit exactly at the cap (accepted) and one
 * past it (REFUSED), plus a small control.
 *
 * Deterministic, no network, no sleeps.
 *
 * Wiring (coordinator): NOT applied here; CMakeLists.txt is
 * out of scope for this change set):
 * add_executable(otsardb_manifest_list_cap_test
 * tests/manifest_list_cap_test.c)
 * target_include_directories(otsardb_manifest_list_cap_test
 * PRIVATE include src/core src/crypto src/storage tests)
 * target_link_libraries(otsardb_manifest_list_cap_test
 * PRIVATE otsardb_core)
 * add_test(NAME otsardb_manifest_list_cap
 * COMMAND otsardb_manifest_list_cap_test)
 * set_tests_properties(otsardb_manifest_list_cap
 * PROPERTIES TIMEOUT 600)
 */
#include "commit_manifest.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Build a v4 manifest JSON with a single segment whose pages[] holds
 * pages_count entries (1..pages_count, the only valid sorted/deduped 1-based
 * set) and whose frame_offsets[] holds offsets_count entries (0..N-1,
 * strictly increasing, each below the segment size). */
static char *build_v4_manifest_json(size_t pages_count,
                                    size_t offsets_count,
                                    size_t *out_size) {
    size_t cap = (size_t)64 * 1024 * 1024;
    char *buf = (char *)malloc(cap);
    assert(buf);
    size_t pos = 0;
    size_t rem = cap;

    uint64_t db_size = pages_count > 0 ? (uint64_t)pages_count : 1u;
    uint64_t seg_size = offsets_count > 0 ? (uint64_t)offsets_count : 1u;

    int n = snprintf(buf + pos, rem,
                     "{\"otsardb_format\":4,\"database_id\":\"0151-f06-db\","
                     "\"generation\":1,\"commit_id\":\"c1\","
                     "\"parent_generation\":0,\"parent_commit_id\":\"\","
                     "\"parent_manifest_sha256\":\"\",\"page_size\":4096,"
                     "\"database_size_pages\":%llu,"
                     "\"segments\":[{\"key\":\"segments/sha256/aa/aa.otsseg\","
                     "\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
                     "\"size\":%llu,\"pages\":[",
                     (unsigned long long)db_size, (unsigned long long)seg_size);
    assert(n > 0 && (size_t)n < rem);
    pos += (size_t)n;
    rem -= (size_t)n;

    for (size_t i = 1; i <= pages_count; ++i) {
        n = snprintf(buf + pos, rem, "%s%llu", i > 1 ? "," : "",
                     (unsigned long long)i);
        assert(n > 0 && (size_t)n < rem);
        pos += (size_t)n;
        rem -= (size_t)n;
    }

    n = snprintf(buf + pos, rem, "]");
    assert(n > 0 && (size_t)n < rem);
    pos += (size_t)n;
    rem -= (size_t)n;

    if (offsets_count > 0) {
        n = snprintf(buf + pos, rem, ",\"frame_offsets\":[");
        assert(n > 0 && (size_t)n < rem);
        pos += (size_t)n;
        rem -= (size_t)n;

        for (size_t i = 0; i < offsets_count; ++i) {
            n = snprintf(buf + pos, rem, "%s%llu", i > 0 ? "," : "",
                         (unsigned long long)i);
            assert(n > 0 && (size_t)n < rem);
            pos += (size_t)n;
            rem -= (size_t)n;
        }

        n = snprintf(buf + pos, rem, "]");
        assert(n > 0 && (size_t)n < rem);
        pos += (size_t)n;
        rem -= (size_t)n;
    }

    n = snprintf(buf + pos, rem, "}]}\n");
    assert(n > 0 && (size_t)n < rem);
    pos += (size_t)n;

    *out_size = pos;
    return buf;
}

static void decode_ok_size(const char *json,
                           size_t size,
                           uint32_t *out_page_count,
                           size_t *out_offset_count) {
    otsardb_commit_manifest manifest = {0};
    assert(otsardb_commit_manifest_decode_json(json, size, &manifest));
    assert(manifest.format_version == OTSARDB_COMMIT_MANIFEST_VERSION);
    assert(manifest.segment_count == 1);
    if (out_page_count) *out_page_count = manifest.segments[0].page_count;
    if (out_offset_count) *out_offset_count = (size_t)manifest.segments[0].page_count;
    otsardb_commit_manifest_free(&manifest);
}

static void decode_refused(const char *json, size_t size) {
    otsardb_commit_manifest manifest = {0};
    assert(!otsardb_commit_manifest_decode_json(json, size, &manifest));
}

static void test_small_control(void) {
    size_t size = 0;
    char *json = build_v4_manifest_json(3, 3, &size);
    uint32_t pages = 0;
    decode_ok_size(json, size, &pages, NULL);
    assert(pages == 3);
    free(json);
}

/* Exactly at the cap: a list with OTSARDB_MANIFEST_MAX_LIST_ENTRIES entries
 * is a legitimate decode and must be ACCEPTED. */
static void test_at_boundary_accepted(void) {
    const size_t n = (size_t)OTSARDB_MANIFEST_MAX_LIST_ENTRIES;
    size_t size = 0;
    char *json = build_v4_manifest_json(n, n, &size);
    uint32_t pages = 0;
    decode_ok_size(json, size, &pages, NULL);
    assert(pages == OTSARDB_MANIFEST_MAX_LIST_ENTRIES);
    free(json);
}

/* One past the cap on frame_offsets (the u64 list): refused. Both lists sit
 * past the cap with EQUAL counts, so the decode has no page/offset mismatch
 * to trip on — the shared list cap (parse_u64_list, which pages[] runs
 * through parse_u32_list too) is the only reason post-fix decode fails.
 * Pre- this manifest decoded cleanly. */
static void test_offsets_over_cap_refused(void) {
    const size_t n = (size_t)OTSARDB_MANIFEST_MAX_LIST_ENTRIES + 1u;
    size_t size = 0;
    char *json = build_v4_manifest_json(n, n, &size);
    decode_refused(json, size);
    free(json);
}

/* One past the cap on pages (the u32 list, parsed through parse_u64_list),
 * with no frame_offsets marker at all: refused. Pre- this
 * manifest decoded cleanly. */
static void test_pages_over_cap_refused(void) {
    const size_t pages = (size_t)OTSARDB_MANIFEST_MAX_LIST_ENTRIES + 1u;
    size_t size = 0;
    char *json = build_v4_manifest_json(pages, 0, &size);
    decode_refused(json, size);
    free(json);
}

int main(void) {
    test_small_control();
    test_at_boundary_accepted();
    test_offsets_over_cap_refused();
    test_pages_over_cap_refused();

    puts("F06 fix test passed: manifest lists at the OTSARDB_MANIFEST_MAX_"
         "LIST_ENTRIES cap decode, one past it fails closed ");
    return 0;
}
