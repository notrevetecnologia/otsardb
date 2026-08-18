#ifndef OTSARDB_SEGMENT_H
#define OTSARDB_SEGMENT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Format versions:
 * v1 - raw whole payload (no compression).
 * v2 - whole-payload LZ4 when OTSARDB_SEGMENT_FLAG_LZ4 is set.
 * v3 - per-frame blocks: the stored payload of a
 * WAL_FRAMES segment is a copy of the WAL batch header followed by one
 * self-describing block per UNIQUE page (sorted ascending, last-writer
 * frame wins), enabling byte-range frame reads through the manifest's
 * "frame_offsets". Decoders still rebuild the exact batch from the
 * blocks so whole-object reads (recovery, legacy lazy hydration) work
 * unchanged.
 * v1 and v2 segments remain decodable; v3 is emitted only for payloads that
 * parse as a WAL batch. */
#define OTSARDB_SEGMENT_FORMAT_VERSION 3u
#define OTSARDB_SEGMENT_FLAG_LZ4 0x0001u
#define OTSARDB_SEGMENT_COMMIT_ID_MAX 63
#define OTSARDB_SEGMENT_SHA256_HEX_SIZE 64

/* v3 per-frame block layout (all little-endian):
 * u32 orig_len - decompressed size of the frame record (8 + page_size)
 * u32 data_len - stored data size (== orig_len when raw)
 * u8 flags - bit0: data is LZ4-compressed
 * u32 frame_crc - CRC-32 of the DECOMPRESSED frame record
 * data[data_len] - frame record (u32 page_no | u32 0 | page bytes)
 * A frame record is exactly one WAL-batch frame (OTSARDB_WAL_BATCH_FRAME_HEADER
 * bytes of header + page_size page bytes). */
#define OTSARDB_SEGMENT_BLOCK_FLAG_LZ4 0x01u
#define OTSARDB_SEGMENT_BLOCK_HEADER_SIZE 13u
#define OTSARDB_SEGMENT_BATCH_HEADER_COPY_SIZE 32u

typedef enum otsardb_segment_kind {
    OTSARDB_SEGMENT_WAL_FRAMES = 1
} otsardb_segment_kind;

typedef struct otsardb_segment_view {
    uint32_t format_version;
    uint32_t kind;
    char commit_id[OTSARDB_SEGMENT_COMMIT_ID_MAX + 1];
    const unsigned char *payload;
    size_t payload_size;
    int compressed;
    char payload_sha256[OTSARDB_SEGMENT_SHA256_HEX_SIZE + 1];
} otsardb_segment_view;

/* Per-frame block table of a v3 segment, returned by the encoder (
 * 0059). Offsets are byte offsets of each block within the encoded object;
 * blocks are contiguous and sorted by page number, so block i spans
 * [offsets[i], offsets[i+1]) and the last block ends at the object size.
 * All arrays are malloc'd; release with otsardb_segment_frame_info_release. */
typedef struct otsardb_segment_frame_info {
    uint64_t *offsets;
    uint32_t *page_nos;
    uint32_t count;
} otsardb_segment_frame_info;

void otsardb_segment_frame_info_release(otsardb_segment_frame_info *info);

/* v2-compatible encoder (whole-payload LZ4, no frame table). Kept for
 * legacy callers and tests; otsardb_publish_commit uses the _with_frames form. */
int otsardb_segment_encode(uint32_t kind,
                         const char *commit_id,
                         const void *payload,
                         size_t payload_size,
                         unsigned char **out_data,
                         size_t *out_size,
                         char out_object_sha256[OTSARDB_SEGMENT_SHA256_HEX_SIZE + 1],
                         int compress);

/* Encoder with per-frame block table. When the payload is a WAL batch, a v3
 * object is produced and out_frames (when non-NULL) receives one block per
 * unique page (sorted ascending, last-writer frame wins). Any other payload
 * falls back to the v2 layout with out_frames->count == 0. */
int otsardb_segment_encode_with_frames(uint32_t kind,
                                     const char *commit_id,
                                     const void *payload,
                                     size_t payload_size,
                                     unsigned char **out_data,
                                     size_t *out_size,
                                     char out_object_sha256[OTSARDB_SEGMENT_SHA256_HEX_SIZE + 1],
                                     int compress,
                                     otsardb_segment_frame_info *out_frames);

int otsardb_segment_decode(const void *data, size_t size, otsardb_segment_view *out_view);

/* Whole-object decompression. For v1/v2 this is the stored whole payload
 * (LZ4 when compressed); for v3 it rebuilds the exact WAL batch from the
 * block sequence. The output must be freed by the caller. */
int otsardb_segment_decompress(const otsardb_segment_view *view,
                             void **out_data,
                             size_t *out_size);

/* Parse + decode ONE v3 frame block inside a byte range (a whole v3 payload
 * or a slice fetched from the object). *pos is advanced past the block.
 * Returns 1 on success (out_frame malloc'd, out_page_no = frame's page).
 * Blocks whose orig_len is below the minimum possible frame record
 * (8 + OTSARDB_WAL_BATCH_PAGE_SIZE_MIN) are rejected before any read of the
 * decoded frame (SEG-1; hardened to the full record minimum
 * by the record-minimum check, F03). */
int otsardb_segment_frame_decode(const unsigned char *data,
                               size_t size,
                               size_t *pos,
                               unsigned char **out_frame,
                               size_t *out_frame_size,
                               uint32_t *out_page_no);

/* CRC-32 (IEEE, reflected, poly 0xEDB88320) over arbitrary bytes. Used to
 * verify a fetched frame block against its own stored checksum. */
uint32_t otsardb_segment_crc32(const void *data, size_t size);

#ifdef __cplusplus
}
#endif

#endif
