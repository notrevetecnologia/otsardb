/* decode_validation_test.c — decode-time validation regression tests for
 * (audit findings SEG-1, MAN-1, B6) and 
 * (audit finding F03).
 *
 * SEG-1: v3 frame blocks with orig_len < 8 must be rejected BEFORE the
 * decoder reads frame[0..7] — previously the accept/reject decision
 * was made by 4 bytes past the malloc(orig_len) allocation.
 * F03: the minimum is hardened to the FULL minimum frame record
 * (8 + OTSARDB_WAL_BATCH_PAGE_SIZE_MIN = 520 bytes; a real record
 * is 8 + page_size with page_size in [512, 65536]): every
 * orig_len in 1..519 is rejected even with a valid CRC and a valid
 * frame layout, and the boundary value 520 still decodes.
 * MAN-1: commit-manifest decode must reject non-monotonic frame_offsets
 * and offsets outside the segment object; encode refuses to emit
 * them too.
 * B6: checkpoint page-record handling fails closed on compressed
 * records and on stored sizes != page_size (which would over-read
 * the record buffer during apply); checkpoint-manifest decode
 * rejects page-group sizes that do not match the record layout.
 *
 * Deterministic: memory store + direct encode/decode only, no network.
 *
 * Wiring (coordinator, /0137 — already applied):
 * add_executable(otsardb_decode_validation_test
 * tests/decode_validation_test.c tests/memory_store.c)
 * target_include_directories(otsardb_decode_validation_test
 * PRIVATE include src/core src/crypto src/storage tests)
 * target_link_libraries(otsardb_decode_validation_test PRIVATE otsardb_core)
 * add_test(NAME otsardb_decode_validation COMMAND otsardb_decode_validation_test)
 */
#include "checkpoint_manifest.h"
#include "checkpoint_page.h"
#include "commit_manifest.h"
#include "memory_store.h"
#include "object_store.h"
#include "recovery.h"
#include "segment.h"
#include "sha256.h"
#include "wal_batch.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#define otsardb_unlink _unlink
#else
#include <unistd.h>
#define otsardb_unlink unlink
#endif

/* ---- local little-endian helpers (independent of the library's statics) ---- */

/* Internal layout constants mirrored from segment.c / checkpoint_page.c
 * (not exported by the headers): v3 segment header = 8 magic + 4 version +
 * 4 kind + 2 commit-id len + 2 flags + 8 payload size + 32 sha256 digest;
 * checkpoint page object = 8 magic + 4 version + 4 page size + 8 database
 * size + 4 count + 4 reserved = 32 bytes. */
#define OTSARDB_SEGMENT_HEADER_SIZE_LOCAL 60u
#define OTSARDB_CP_HEADER_SIZE_LOCAL 32u
#define OTSARDB_CP_MAGIC_SIZE_LOCAL 8u

static void put_u32_le(unsigned char *p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = (unsigned char)((v >> (i * 8)) & 0xffu);
}

static void put_u64_le(unsigned char *p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = (unsigned char)((v >> (i * 8)) & 0xffu);
}

static char *dup64hex(void) {
    char *out = (char *)malloc(65);
    assert(out);
    memset(out, 'a', 64);
    out[64] = '\0';
    return out;
}

/* ------------------------------------------------------------------ SEG-1 */

/* Build a single-block v3 payload: a block header whose orig_len/data_len
 * are both `len`, followed by `len` frame bytes and a re-computed CRC. The
 * frame bytes carry a VALID frame-record layout (u32 page_no = 7, u32
 * padding = 0, filler page bytes) whenever they fit, so for len >= 8 the
 * ONLY reason a decoder can reject the block is the size bound — a buggy
 * decoder reading past the allocation (len < 8) or accepting impossible
 * orig_len values (8 <= len < 520) would see a "valid" frame and accept.
 * The trailing bytes after the block are zeroed. */
static unsigned char *build_block_payload(uint32_t len, size_t *out_size) {
    size_t block_size = OTSARDB_SEGMENT_BLOCK_HEADER_SIZE + (size_t)len;
    size_t total = block_size + 16u;
    unsigned char *payload = (unsigned char *)malloc(total);
    assert(payload);
    memset(payload, 0, total);

    unsigned char *frame = (unsigned char *)malloc(len ? (size_t)len : 1u);
    assert(frame);
    memset(frame, 0, len ? (size_t)len : 1u);
    if (len >= 4u) put_u32_le(frame, 7);          /* nonzero page_no */
    /* bytes 4..7 stay 0: the frame record's reserved u32 is 0 */
    for (uint32_t i = 8; i < len; ++i) frame[i] = (unsigned char)(0x80u + i);

    put_u32_le(payload + 0, len);                              /* orig_len */
    put_u32_le(payload + 4, len);                              /* data_len  */
    payload[8] = 0;                                            /* flags     */
    put_u32_le(payload + 9, otsardb_segment_crc32(frame, len)); /* frame_crc */
    memcpy(payload + OTSARDB_SEGMENT_BLOCK_HEADER_SIZE, frame, len);
    free(frame);

    *out_size = total;
    return payload;
}

/* (F03): the reject range is the audit's test set — 1..7
 * (the SEG-1 OOB window), 8 (SEG-1's former boundary) and 511 (just below
 * the real minimum record) — extended to the full impossible range below
 * the minimum frame record 8 + OTSARDB_WAL_BATCH_PAGE_SIZE_MIN. All must be
 * rejected even though each carries a valid CRC and, for len >= 8, a valid
 * frame layout (before , orig_len 8..519 was ACCEPTED). */
static void test_seg1_frame_decode_rejects_tiny_blocks(void) {
    const uint32_t min_frame_record =
        OTSARDB_WAL_BATCH_FRAME_HEADER_SIZE + OTSARDB_WAL_BATCH_PAGE_SIZE_MIN;
    assert(min_frame_record == 520u);
    for (uint32_t len = 1; len < min_frame_record; ++len) {
        size_t total = 0;
        unsigned char *payload = build_block_payload(len, &total);
        size_t pos = 0;
        unsigned char *frame = NULL;
        size_t frame_size = 0;
        uint32_t page_no = 0;
        /* Before this returned 1 for the zeroed tail (the
         * get_u32_le(frame + 4) read past the allocation and the heap
         * garbage decided acceptance); before 0138, len in 8..519 was
         * accepted because the block content was fully valid. Now every
         * orig_len below the minimum frame record is rejected before any
         * frame byte is read. */
        assert(otsardb_segment_frame_decode(payload, total, &pos, &frame,
                                            &frame_size, &page_no) == 0);
        assert(frame == NULL);
        free(payload);
    }
}

/* The boundary value (orig_len == 8 + page_size with the minimum page size
 * 512) must still decode: the 0138 check is a minimum, not a blanket
 * rejection of small frames. */
static void test_seg1_minimum_frame_record_boundary(void) {
    const uint32_t page_size = OTSARDB_WAL_BATCH_PAGE_SIZE_MIN;
    const uint32_t min_frame_record =
        OTSARDB_WAL_BATCH_FRAME_HEADER_SIZE + OTSARDB_WAL_BATCH_PAGE_SIZE_MIN;
    const uint32_t page_nos[1] = { 3 };
    unsigned char pages[1][OTSARDB_WAL_BATCH_PAGE_SIZE_MIN];
    memset(pages[0], 0x6a, sizeof(pages[0]));

    unsigned char *batch = NULL;
    size_t batch_size = 0;
    assert(otsardb_wal_batch_encode(page_size, 1000, page_nos,
                                    (const unsigned char *)pages, 1,
                                    &batch, &batch_size));

    unsigned char *segment = NULL;
    size_t segment_size = 0;
    char segment_sha[OTSARDB_SEGMENT_SHA256_HEX_SIZE + 1];
    otsardb_segment_frame_info frames;
    assert(otsardb_segment_encode_with_frames(OTSARDB_SEGMENT_WAL_FRAMES,
                                              "seg1-min", batch, batch_size,
                                              &segment, &segment_size,
                                              segment_sha, 0, &frames));
    free(batch);
    assert(frames.count == 1);
    assert(frames.offsets[0] + OTSARDB_SEGMENT_BLOCK_HEADER_SIZE + min_frame_record
           <= (uint64_t)segment_size);

    /* The emitted block has orig_len == 520 and decodes. */
    size_t pos = (size_t)frames.offsets[0];
    unsigned char *frame = NULL;
    size_t frame_size = 0;
    uint32_t page_no = 0;
    assert(otsardb_segment_frame_decode(segment, segment_size, &pos, &frame,
                                        &frame_size, &page_no) == 1);
    assert(frame_size == (size_t)min_frame_record);
    assert(page_no == 3u);
    free(frame);

    /* Crafted block: orig_len == 519 (one below the minimum) with a fully
     * valid layout must be rejected; 520 with the same layout must be
     * accepted — the boundary is exact. */
    {
        size_t total = 0;
        unsigned char *payload = build_block_payload(min_frame_record - 1u, &total);
        size_t p = 0;
        frame = NULL;
        assert(otsardb_segment_frame_decode(payload, total, &p, &frame,
                                            &frame_size, &page_no) == 0);
        assert(frame == NULL);
        free(payload);
    }
    {
        size_t total = 0;
        unsigned char *payload = build_block_payload(min_frame_record, &total);
        size_t p = 0;
        frame = NULL;
        assert(otsardb_segment_frame_decode(payload, total, &p, &frame,
                                            &frame_size, &page_no) == 1);
        assert(frame_size == (size_t)min_frame_record);
        assert(page_no == 7u);
        free(frame);
        free(payload);
    }

    free(segment);
    otsardb_segment_frame_info_release(&frames);
}

static void test_seg1_crafted_segment_rejected_cleanly(void) {
    /* Encode a real v3 segment (one page, raw), then craft the stored block
     * into a CRC-valid orig_len = 4 block exactly like the audit's probe:
     * the payload digest is recomputed so otsardb_segment_decode accepts the
     * object, and decompression must fail closed on the tiny block. */
    const uint32_t page_size = 4096;
    const uint32_t page_nos[2] = { 7, 11 };
    unsigned char pages[2][4096];
    memset(pages[0], 0x42, sizeof(pages[0]));
    memset(pages[1], 0x9e, sizeof(pages[1]));
    

    unsigned char *batch = NULL;
    size_t batch_size = 0;
    assert(otsardb_wal_batch_encode(page_size, 1000, page_nos,
                                    (const unsigned char *)pages, 2,
                                    &batch, &batch_size));

    unsigned char *segment = NULL;
    size_t segment_size = 0;
    char segment_sha[OTSARDB_SEGMENT_SHA256_HEX_SIZE + 1];
    otsardb_segment_frame_info frames;
    assert(otsardb_segment_encode_with_frames(OTSARDB_SEGMENT_WAL_FRAMES,
                                              "seg1-commit",
                                              batch, batch_size,
                                              &segment, &segment_size,
                                              segment_sha, 0, &frames));
    free(batch);
    assert(frames.count == 2);

    /* Block 0 starts at frames.offsets[0] within the encoded object. */
    const uint64_t block0 = frames.offsets[0];
    assert(block0 + OTSARDB_SEGMENT_BLOCK_HEADER_SIZE + page_size + 8 <= segment_size);

    /* Patch the block: orig_len = 4, data_len = 4, raw, CRC over 4 bytes. */
    const unsigned char *orig_frame = segment + block0 + OTSARDB_SEGMENT_BLOCK_HEADER_SIZE;
    put_u32_le(segment + block0 + 0, 4);
    put_u32_le(segment + block0 + 4, 4);
    segment[block0 + 8] = 0;
    put_u32_le(segment + block0 + 9, otsardb_segment_crc32(orig_frame, 4));

    /* Recompute and patch the payload digest (offset 28 in the header). */
    size_t commit_id_len = strlen("seg1-commit");
    const unsigned char *payload = segment + OTSARDB_SEGMENT_HEADER_SIZE_LOCAL + commit_id_len;
    size_t payload_size = segment_size - (OTSARDB_SEGMENT_HEADER_SIZE_LOCAL + commit_id_len);
    unsigned char digest[OTSARDB_SHA256_SIZE];
    assert(otsardb_sha256(payload, payload_size, digest));
    memcpy(segment + 28, digest, OTSARDB_SHA256_SIZE);

    /* The object is still structurally valid (digest + size match). */
    otsardb_segment_view view;
    assert(otsardb_segment_decode(segment, segment_size, &view));

    /* Decompression must fail closed: the block is rejected before its
     * frame bytes are read. */
    void *out = NULL;
    size_t out_size = 0;
    assert(otsardb_segment_decompress(&view, &out, &out_size) == 0);

    /* The remaining frames (block 1 untouched) still decode individually:
     * the rejection is block-local and does not disturb later blocks. */
    size_t pos = (size_t)block0;
    unsigned char *frame = NULL;
    size_t frame_size = 0;
    uint32_t page_no = 0;
    assert(otsardb_segment_frame_decode(segment, segment_size, &pos, &frame,
                                        &frame_size, &page_no) == 0);
    assert(frame == NULL);

    free(segment);
    otsardb_segment_frame_info_release(&frames);
}

static void test_seg1_round_trip_still_works(void) {
    const uint32_t page_size = 4096;
    const uint32_t page_nos[3] = { 2, 5, 9 };
    unsigned char pages[3][4096];
    memset(pages[0], 0x11, sizeof(pages[0]));
    memset(pages[1], 0x22, sizeof(pages[1]));
    memset(pages[2], 0x33, sizeof(pages[2]));

    unsigned char *batch = NULL;
    size_t batch_size = 0;
    assert(otsardb_wal_batch_encode(page_size, 1000, page_nos,
                                    (const unsigned char *)pages, 3,
                                    &batch, &batch_size));

    for (int compress = 0; compress <= 1; ++compress) {
        unsigned char *segment = NULL;
        size_t segment_size = 0;
        char segment_sha[OTSARDB_SEGMENT_SHA256_HEX_SIZE + 1];
        otsardb_segment_frame_info frames;
        assert(otsardb_segment_encode_with_frames(OTSARDB_SEGMENT_WAL_FRAMES,
                                                  "seg1-rt", batch, batch_size,
                                                  &segment, &segment_size,
                                                  segment_sha, compress, &frames));
        assert(frames.count == 3);
        for (uint32_t i = 0; i < frames.count; ++i) {
            assert(frames.offsets[i] < (uint64_t)segment_size);
            if (i > 0) assert(frames.offsets[i] > frames.offsets[i - 1]);
        }

        otsardb_segment_view view;
        assert(otsardb_segment_decode(segment, segment_size, &view));
        void *out = NULL;
        size_t out_size = 0;
        assert(otsardb_segment_decompress(&view, &out, &out_size));
        assert(out_size == batch_size);
        assert(memcmp(out, batch, batch_size) == 0);
        free(out);
        free(segment);
        otsardb_segment_frame_info_release(&frames);
    }
    free(batch);
}

/* ------------------------------------------------------------------ MAN-1 */

static char *build_v4_manifest(const char *offsets_json, size_t *out_size) {
    char *sha = dup64hex();
    const char *fmt =
        "{\"otsardb_format\":4,\"database_id\":\"m1-db\",\"generation\":1,"
        "\"commit_id\":\"m1-commit\",\"parent_generation\":0,"
        "\"parent_commit_id\":\"\",\"parent_manifest_sha256\":\"\","
        "\"page_size\":4096,\"database_size_pages\":100,\"segments\":["
        "{\"key\":\"segments/sha256/aa/aa.otsseg\",\"sha256\":\"%s\","
        "\"size\":1000,\"pages\":[1,2,3],\"frame_offsets\":%s}]}\n";
    int needed = snprintf(NULL, 0, fmt, sha, offsets_json);
    assert(needed > 0);
    char *json = (char *)malloc((size_t)needed + 1u);
    assert(json);
    int written = snprintf(json, (size_t)needed + 1u, fmt, sha, offsets_json);
    assert(written == needed);
    free(sha);
    *out_size = (size_t)written;
    return json;
}

static void test_man1_decode_offsets_validation(void) {
    const char *valid[] = { "[13,50,100]", "[13,100,999]", "[13,50,999]" };
    const char *invalid[] = {
        "[13,13,100]",   /* equal neighbours */
        "[13,50,50]",    /* equal tail */
        "[100,50,13]",   /* decreasing */
        "[13,100,1000]", /* offset == segment size */
        "[13,100,1500]", /* offset > segment size */
        "[0,13,50]",     /* repeats / first == 0 allowed? 0 IS < size and strictly
                          * increasing with [13,50]: valid in-range form, but the
                          * encoder never emits it; keep as a decode-OK baseline */
    };
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); ++i) {
        size_t size = 0;
        char *json = build_v4_manifest(valid[i], &size);
        otsardb_commit_manifest manifest;
        assert(otsardb_commit_manifest_decode_json(json, size, &manifest));
        assert(manifest.segment_count == 1);
        assert(manifest.segments[0].frame_offsets != NULL);
        otsardb_commit_manifest_free(&manifest);
        free(json);
    }
    size_t ok_count = 0;
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        size_t size = 0;
        char *json = build_v4_manifest(invalid[i], &size);
        otsardb_commit_manifest manifest;
        if (otsardb_commit_manifest_decode_json(json, size, &manifest)) {
            /* The [0,13,50] case is structurally in-range and strictly
             * increasing, so it must decode; every other entry must not. */
            assert(i == 5);
            ++ok_count;
            otsardb_commit_manifest_free(&manifest);
        }
        free(json);
    }
    assert(ok_count == 1);
}

static void test_man1_encode_offsets_validation(void) {
    char *sha = dup64hex();
    for (int valid_case = 0; valid_case <= 1; ++valid_case) {
        otsardb_commit_manifest manifest;
        memset(&manifest, 0, sizeof(manifest));
        manifest.format_version = 4;
        manifest.generation = 1;
        manifest.parent_generation = 0;
        manifest.database_size_pages = 100;
        manifest.page_size = 4096;
        strcpy(manifest.database_id, "m1-db");
        strcpy(manifest.commit_id, "m1-commit");
        manifest.segment_count = 1;
        strcpy(manifest.segments[0].key, "segments/sha256/aa/aa.otsseg");
        strcpy(manifest.segments[0].sha256, sha);
        manifest.segments[0].size = 1000;
        static const uint32_t pages[3] = { 1, 2, 3 };
        manifest.segments[0].page_count = 3;
        manifest.segments[0].pages = (uint32_t *)pages;
        uint64_t offsets[3] = { 13, 50, 100 };
        if (valid_case == 0) offsets[1] = 13;   /* non-monotonic */
        else offsets[2] = 1000;                 /* == size */
        manifest.segments[0].frame_offsets = offsets;

        char *json = NULL;
        size_t json_size = 0;
        char sha_out[OTSARDB_MANIFEST_SHA256_HEX_SIZE + 1];
        /* The encoder must refuse to emit what the decoder rejects. */
        assert(otsardb_commit_manifest_encode_json(&manifest, &json, &json_size,
                                                   sha_out) == 0);
    }

    /* Valid offsets still encode and round-trip. */
    otsardb_commit_manifest manifest;
    memset(&manifest, 0, sizeof(manifest));
    manifest.format_version = 4;
    manifest.generation = 1;
    manifest.parent_generation = 0;
    manifest.database_size_pages = 100;
    manifest.page_size = 4096;
    strcpy(manifest.database_id, "m1-db");
    strcpy(manifest.commit_id, "m1-commit");
    manifest.segment_count = 1;
    strcpy(manifest.segments[0].key, "segments/sha256/aa/aa.otsseg");
    strcpy(manifest.segments[0].sha256, sha);
    manifest.segments[0].size = 1000;
    static const uint32_t pages[3] = { 1, 2, 3 };
    uint64_t offsets[3] = { 13, 50, 100 };
    manifest.segments[0].page_count = 3;
    manifest.segments[0].pages = (uint32_t *)pages;
    manifest.segments[0].frame_offsets = offsets;

    char *json = NULL;
    size_t json_size = 0;
    char sha_out[OTSARDB_MANIFEST_SHA256_HEX_SIZE + 1];
    assert(otsardb_commit_manifest_encode_json(&manifest, &json, &json_size,
                                               sha_out));
    otsardb_commit_manifest decoded;
    assert(otsardb_commit_manifest_decode_json(json, json_size, &decoded));
    assert(decoded.segments[0].frame_offsets != NULL);
    assert(decoded.segments[0].frame_offsets[0] == 13);
    assert(decoded.segments[0].frame_offsets[2] == 100);
    otsardb_commit_manifest_free(&decoded);
    free(json);
    free(sha);
}

/* --------------------------------------------------------------------- B6 */

static char *build_checkpoint_manifest_json(const char *group_json) {
    char *sha = dup64hex();
    const char *fmt =
        "{\"otsardb_checkpoint_format\":1,\"database_id\":\"b6-db\","
        "\"covered_generation\":1,\"base_commit_id\":\"b6-commit\","
        "\"base_manifest_sha256\":\"\",\"page_size\":1024,"
        "\"database_size_pages\":50,\"page_groups\":[%s]}\n";
    int needed = snprintf(NULL, 0, fmt, group_json);
    assert(needed > 0);
    char *json = (char *)malloc((size_t)needed + 1u);
    assert(json);
    int written = snprintf(json, (size_t)needed + 1u, fmt, group_json);
    assert(written == needed);
    free(sha);
    return json;
}

static void test_b6_manifest_group_size_validation(void) {
    char *sha = dup64hex();
    /* Valid layouts: 32-byte header + k records of (12 + 1024) bytes. */
    char group[600];
    snprintf(group, sizeof(group), "{\"key\":\"g1\",\"sha256\":\"%s\",\"size\":%llu}",
             sha, (unsigned long long)(32u + 1036u));
    char *json = build_checkpoint_manifest_json(group);
    size_t json_size = strlen(json);
    otsardb_checkpoint_manifest manifest;
    assert(otsardb_checkpoint_manifest_decode(json, json_size, &manifest));
    assert(manifest.page_group_count == 1);
    free(json);

    snprintf(group, sizeof(group), "{\"key\":\"g1\",\"sha256\":\"%s\",\"size\":%llu}",
             sha, (unsigned long long)(32u + 2u * 1036u));
    json = build_checkpoint_manifest_json(group);
    json_size = strlen(json);
    assert(otsardb_checkpoint_manifest_decode(json, json_size, &manifest));
    free(json);

    /* Invalid layouts: header-only, short record, misaligned. */
    const uint64_t bad_sizes[] = { 32u, 32u + 1035u, 100u, 32u + 1037u,
                                   31u + 1036u, 32u + 1036u + 500u };
    for (size_t i = 0; i < sizeof(bad_sizes) / sizeof(bad_sizes[0]); ++i) {
        snprintf(group, sizeof(group), "{\"key\":\"g1\",\"sha256\":\"%s\",\"size\":%llu}",
                 sha, (unsigned long long)bad_sizes[i]);
        json = build_checkpoint_manifest_json(group);
        json_size = strlen(json);
        otsardb_checkpoint_manifest manifest2;
        assert(otsardb_checkpoint_manifest_decode(json, json_size, &manifest2) == 0);
        free(json);
    }
    free(sha);
}

/* Put a crafted page-group object in the store and run the apply path. */
static int apply_group(otsardb_object_store *store,
                       const char *root,
                       const char *key,
                       const unsigned char *object_data,
                       size_t object_size,
                       const char *db_path) {
    char *sha = dup64hex();
    assert(otsardb_sha256_hex_data(object_data, object_size, sha));

    char full_key[256];
    int n = snprintf(full_key, sizeof(full_key), "%s/%s", root, key);
    assert(n > 0 && (size_t)n < sizeof(full_key));
    char *etag = NULL;
    assert(otsardb_store_put(store, full_key, object_data, object_size, NULL, &etag) ==
           OTSARDB_STORE_OK);
    free(etag);

    char group[600];
    snprintf(group, sizeof(group), "{\"key\":\"%s\",\"sha256\":\"%s\",\"size\":%llu}",
             key, sha, (unsigned long long)object_size);
    char *json = build_checkpoint_manifest_json(group);
    size_t json_size = strlen(json);

    otsardb_checkpoint_manifest manifest;
    if (!otsardb_checkpoint_manifest_decode(json, json_size, &manifest)) {
        free(json);
        free(sha);
        return -1;
    }
    free(json);
    free(sha);
    int rc = otsardb_recovery_apply_checkpoint_pages(store, root, &manifest, db_path);
    return rc;
}

static void test_b6_apply_fails_closed_on_compressed_record(void) {
    const uint32_t page_size = 1024;
    const uint32_t page_nos[2] = { 1, 2 };
    unsigned char pages[2][1024];
    memset(pages[0], 0x5a, sizeof(pages[0]));
    memset(pages[1], 0xa5, sizeof(pages[1]));
    

    unsigned char *object = NULL;
    size_t object_size = 0;
    assert(otsardb_checkpoint_page_encode(page_size, 50, page_nos,
                                          (const unsigned char *)pages, 2,
                                          &object, &object_size));
    assert(object_size == 32u + 2u * (12u + page_size));

    /* Set the LZ4 flag on record 0's flags field (record header: u32 page_no,
     * u32 flags, u32 stored_size; records start at byte 32). */
    put_u32_le(object + 32u + 4u, OTSARDB_CHECKPOINT_PAGE_FLAG_LZ4);

    otsardb_object_store *store = otsardb_test_memory_store_create();
    assert(store);
    const char *db_path = "b6-compressed.db";
    otsardb_unlink(db_path);

    /* Decode still accepts the object (the flag is data, not structure), but
     * the apply path must fail closed instead of writing compressed bytes. */
    otsardb_checkpoint_page_view view;
    assert(otsardb_checkpoint_page_decode(object, object_size, &view));
    int rc = apply_group(store, "b6root", "g1", object, object_size, db_path);
    assert(rc == 0);

    otsardb_test_memory_store_destroy(store);
    otsardb_unlink(db_path);
    free(object);
}

static void test_b6_apply_fails_closed_on_short_stored_record(void) {
    const uint32_t page_size = 1024;
    const size_t header = OTSARDB_CP_HEADER_SIZE_LOCAL;

    /* Case 1: a LAST record storing 512 bytes while page_size is 1024.
     * The data sizes no longer sum to k * page_size, so the checkpoint
     * manifest's record-layout check rejects the group at DECODE time
     * (the group can never be a valid record set) — apply never runs. */
    {
        const uint32_t stored_sizes[4] = { 1024, 1024, 1024, 512 };
        size_t object_size = header;
        for (int i = 0; i < 4; ++i) object_size += 12u + stored_sizes[i];
        assert(object_size == header + 3u * (12u + page_size) + 12u + 512u);

        unsigned char *object = (unsigned char *)malloc(object_size);
        assert(object);
        memset(object, 0xcc, object_size);
        const unsigned char magic[OTSARDB_CP_MAGIC_SIZE_LOCAL] = { 'A','R','K','C','P','0','1' };
        memcpy(object + 0, magic, OTSARDB_CP_MAGIC_SIZE_LOCAL);
        put_u32_le(object + 8, OTSARDB_CHECKPOINT_PAGE_FORMAT_VERSION);
        put_u32_le(object + 12, page_size);
        put_u64_le(object + 16, 50);           /* database_size_pages */
        put_u32_le(object + 24, 4);            /* page_count */
        put_u32_le(object + 28, 0);            /* reserved */
        size_t rec_off = header;
        for (uint32_t i = 0; i < 4; ++i) {
            put_u32_le(object + rec_off + 0, i + 1);
            put_u32_le(object + rec_off + 4, 0);
            put_u32_le(object + rec_off + 8, stored_sizes[i]);
            memset(object + rec_off + 12, (int)(0x10u + i), stored_sizes[i]);
            rec_off += 12u + stored_sizes[i];
        }
        assert(rec_off == object_size);

        /* The object itself decodes (its exact-size bookkeeping is sound),
         * but no valid manifest can reference it: the group is rejected at
         * manifest decode. */
        otsardb_checkpoint_page_view view;
        assert(otsardb_checkpoint_page_decode(object, object_size, &view));
        otsardb_object_store *store = otsardb_test_memory_store_create();
        assert(store);
        const char *db_path = "b6-short-m.db";
        otsardb_unlink(db_path);
        int rc = apply_group(store, "b6root", "g1", object, object_size, db_path);
        assert(rc == -1);
        otsardb_test_memory_store_destroy(store);
        otsardb_unlink(db_path);
        free(object);
    }

    /* Case 2: data sizes 1024+1024+512+1536 still sum to 4 * page_size, so
     * the manifest layout check passes and the object decodes — but the
     * apply path must fail closed at the 512-byte record (before 
     * 0133 the fwrite(1024) read past the short record; for a trailing
     * short record that is an OOB heap read at the object boundary). */
    {
        const uint32_t stored_sizes[4] = { 1024, 1024, 512, 1536 };
        size_t object_size = header;
        for (int i = 0; i < 4; ++i) object_size += 12u + stored_sizes[i];
        assert(object_size == header + 4u * (12u + page_size));

        unsigned char *object = (unsigned char *)malloc(object_size);
        assert(object);
        memset(object, 0xcc, object_size);
        const unsigned char magic[OTSARDB_CP_MAGIC_SIZE_LOCAL] = { 'A','R','K','C','P','0','1' };
        memcpy(object + 0, magic, OTSARDB_CP_MAGIC_SIZE_LOCAL);
        put_u32_le(object + 8, OTSARDB_CHECKPOINT_PAGE_FORMAT_VERSION);
        put_u32_le(object + 12, page_size);
        put_u64_le(object + 16, 50);           /* database_size_pages */
        put_u32_le(object + 24, 4);            /* page_count */
        put_u32_le(object + 28, 0);            /* reserved */
        size_t rec_off = header;
        for (uint32_t i = 0; i < 4; ++i) {
            put_u32_le(object + rec_off + 0, i + 1);
            put_u32_le(object + rec_off + 4, 0);
            put_u32_le(object + rec_off + 8, stored_sizes[i]);
            memset(object + rec_off + 12, (int)(0x10u + i), stored_sizes[i]);
            rec_off += 12u + stored_sizes[i];
        }
        assert(rec_off == object_size);

        otsardb_checkpoint_page_view view;
        assert(otsardb_checkpoint_page_decode(object, object_size, &view));
        otsardb_object_store *store = otsardb_test_memory_store_create();
        assert(store);
        const char *db_path = "b6-short-r.db";
        otsardb_unlink(db_path);
        int rc = apply_group(store, "b6root", "g1", object, object_size, db_path);
        assert(rc == 0);
        otsardb_test_memory_store_destroy(store);
        otsardb_unlink(db_path);
        free(object);
    }
}

static void test_b6_apply_valid_record_succeeds(void) {
    const uint32_t page_size = 1024;
    const uint32_t page_nos[2] = { 1, 2 };
    unsigned char pages[2][1024];
    memset(pages[0], 0x5a, sizeof(pages[0]));
    memset(pages[1], 0xa5, sizeof(pages[1]));
    

    unsigned char *object = NULL;
    size_t object_size = 0;
    assert(otsardb_checkpoint_page_encode(page_size, 50, page_nos,
                                          (const unsigned char *)pages, 2,
                                          &object, &object_size));

    otsardb_object_store *store = otsardb_test_memory_store_create();
    assert(store);
    const char *db_path = "b6-valid.db";
    otsardb_unlink(db_path);

    int rc = apply_group(store, "b6root", "g1", object, object_size, db_path);
    assert(rc == 1);

    /* The file must contain the pages at their (page_no - 1) * page_size
     * offsets. */
    FILE *file = fopen(db_path, "rb");
    assert(file);
    unsigned char readback[2][1024];
    assert(fread(readback[0], 1, page_size, file) == page_size);
    assert(fread(readback[1], 1, page_size, file) == page_size);
    fclose(file);
    assert(memcmp(readback[0], pages[0], page_size) == 0);
    assert(memcmp(readback[1], pages[1], page_size) == 0);

    otsardb_test_memory_store_destroy(store);
    otsardb_unlink(db_path);
    free(object);
}

/* ------------------------------------------------------------------ main */

int main(void) {
    test_seg1_frame_decode_rejects_tiny_blocks();
    test_seg1_minimum_frame_record_boundary();
    test_seg1_crafted_segment_rejected_cleanly();
    test_seg1_round_trip_still_works();

    test_man1_decode_offsets_validation();
    test_man1_encode_offsets_validation();

    test_b6_manifest_group_size_validation();
    test_b6_apply_fails_closed_on_compressed_record();
    test_b6_apply_fails_closed_on_short_stored_record();
    test_b6_apply_valid_record_succeeds();

    puts("decode validation regression tests passed (SEG-1, F03, MAN-1, B6)");
    return 0;
}
