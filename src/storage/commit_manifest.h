#ifndef OTSARDB_COMMIT_MANIFEST_H
#define OTSARDB_COMMIT_MANIFEST_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OTSARDB_MANIFEST_ID_MAX 63
#define OTSARDB_MANIFEST_KEY_MAX 1023
#define OTSARDB_MANIFEST_SHA256_HEX_SIZE 64
#define OTSARDB_COMMIT_MANIFEST_VERSION 4u

/* One commit is encoded as one or more immutable content-addressed segments.
 * Manifest v1/v2 stored a single flat segment reference; v3 stores an ordered
 * array so a very large transaction can be split into bounded chunks without
 * changing commit semantics. Recovery applies all segments oldest-first.
 *
 * In v3, each segment key is a SUFFIX relative to the database root (the
 * encoder emits "segments/sha256/XX/<sha>.otsseg" without the root prefix);
 * v1/v2 flat segment_key remains the absolute key for backward compatibility.
 * The relative form keeps the manifest small (bounded key length) while
 * supporting arbitrarily deep database roots. */
#define OTSARDB_MANIFEST_MAX_SEGMENTS 128u
#define OTSARDB_MANIFEST_SEGMENT_KEY_MAX 128u

/* (audit F06): hard cap on the number of entries any single
 * list-typed array (pages[] / frame_offsets[]) may decode into. parse_u64_list
 * grows geometrically with no count bound of its own (only the linear JSON
 * size bounds it, and that only when the object came from a capped store
 * GET). 1 000 000 entries is far above every legitimate manifest (a segment
 * holds at most one frame per database page and a real database is bounded
 * by the store GET cap and the recovery budget) and bounds the decoder's
 * per-array allocation; exceeding it FAILS CLOSED. */
#define OTSARDB_MANIFEST_MAX_LIST_ENTRIES 1000000u

typedef struct otsardb_manifest_segment_ref {
    char key[OTSARDB_MANIFEST_SEGMENT_KEY_MAX + 1];
    char sha256[OTSARDB_MANIFEST_SHA256_HEX_SIZE + 1];
    uint64_t size;
    /* v4+ page set: sorted, deduped page numbers touched by this segment,
     * owned by the struct after a successful decode (freed by
     * otsardb_commit_manifest_free). page_count 0 + pages NULL means "unknown
     * page set" (legacy v1-v3, or a v4 manifest published without pages —
     * conflict detection must treat it as overlapping everything). */
    uint32_t page_count;
    uint32_t *pages;
    /* v4+ optional: byte offsets of each page's frame within the segment for
     * lazy range reads. When present, count must equal page_count. NULL for
     * v1-v3 and for v4 manifests that did not record offsets. */
    uint64_t *frame_offsets;
} otsardb_manifest_segment_ref;

typedef struct otsardb_commit_manifest {
    uint32_t format_version;
    uint64_t generation;
    uint64_t parent_generation;
    uint32_t page_size;
    uint64_t database_size_pages;
    char database_id[OTSARDB_MANIFEST_ID_MAX + 1];
    char commit_id[OTSARDB_MANIFEST_ID_MAX + 1];
    char parent_commit_id[OTSARDB_MANIFEST_ID_MAX + 1];
    /* Empty only for the genesis commit. Manifest v1 is decoded with this
     * field empty for backward compatibility; all newly published v2/v3
     * child manifests must carry the exact SHA-256 of their parent manifest. */
    char parent_manifest_sha256[OTSARDB_MANIFEST_SHA256_HEX_SIZE + 1];
    uint32_t segment_count;
    otsardb_manifest_segment_ref segments[OTSARDB_MANIFEST_MAX_SEGMENTS];
    /* Optional rolling database checksum: XOR over CRC-64-ISO of every page
     * (page_no || page) of the database state AFTER this commit was applied.
     * Empty when the chain predates checksums (v1/v2 or older v3
     * manifests). */
    char post_apply_checksum[17];
} otsardb_commit_manifest;

int otsardb_commit_manifest_encode_json(const otsardb_commit_manifest *manifest,
                                      char **out_json,
                                      size_t *out_size,
                                      char out_sha256[OTSARDB_MANIFEST_SHA256_HEX_SIZE + 1]);

int otsardb_commit_manifest_decode_json(const char *json,
                                      size_t size,
                                      otsardb_commit_manifest *out_manifest);

/* Release the arrays owned by a decoded manifest (pages / frame_offsets).
 * Safe to call on a zero-initialised manifest and on a manifest whose decode
 * failed; after this call the pointer fields are NULL. Never free a manifest
 * returned by otsardb_publish_commit: its pages pointers are borrowed from the
 * caller's request, not owned. */
void otsardb_commit_manifest_free(otsardb_commit_manifest *manifest);

#ifdef __cplusplus
}
#endif

#endif
