#include "segment.h"
#include "sha256.h"
#include "wal_batch.h"
#include "lz4.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define OTSARDB_SEGMENT_MAGIC_SIZE 8
#define OTSARDB_SEGMENT_HEADER_SIZE (OTSARDB_SEGMENT_MAGIC_SIZE + 4 + 4 + 2 + 2 + 8 + 32)

#define OTSARDB_SEGMENT_MAX_DECOMPRESSED (1024u * 1024u * 1024u)

static const unsigned char OTSARDB_SEGMENT_MAGIC[OTSARDB_SEGMENT_MAGIC_SIZE] = {
    'A','R','K','S','E','G','0','1'
};

static void put_u16_le(unsigned char *p, uint16_t v) {
    p[0] = (unsigned char)(v & 0xffu);
    p[1] = (unsigned char)((v >> 8) & 0xffu);
}

static void put_u32_le(unsigned char *p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = (unsigned char)((v >> (i * 8)) & 0xffu);
}

static void put_u64_le(unsigned char *p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = (unsigned char)((v >> (i * 8)) & 0xffu);
}

static uint16_t get_u16_le(const unsigned char *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_u32_le(const unsigned char *p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= ((uint32_t)p[i]) << (i * 8);
    return v;
}

static uint64_t get_u64_le(const unsigned char *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= ((uint64_t)p[i]) << (i * 8);
    return v;
}

/* ---------------------------------------------------------------------------
 * CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320). Standard public-domain
 * table-driven implementation.
 * ------------------------------------------------------------------------- */

uint32_t otsardb_segment_crc32(const void *data, size_t size) {
    static uint32_t table[256];
    static int table_built = 0;
    const unsigned char *bytes = (const unsigned char *)data;
    if (!table_built) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        table_built = 1;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) {
        crc = table[(crc ^ bytes[i]) & 0xffu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

/* ---------------------------------------------------------------------------
 * v3 frame block parse + decode (shared by otsardb_segment_decompress and the
 * lazy-hydration slice path, so the block layout decoded is exactly the
 * layout the encoder produced).
 * ------------------------------------------------------------------------- */

int otsardb_segment_frame_decode(const unsigned char *data,
                               size_t size,
                               size_t *pos,
                               unsigned char **out_frame,
                               size_t *out_frame_size,
                               uint32_t *out_page_no) {
    if (!data || !pos || !out_frame || !out_frame_size || !out_page_no) return 0;
    if (*pos > size || size - *pos < OTSARDB_SEGMENT_BLOCK_HEADER_SIZE) return 0;

    uint32_t orig_len = get_u32_le(data + *pos);
    uint32_t data_len = get_u32_le(data + *pos + 4);
    unsigned char flags = data[*pos + 8];
    uint32_t frame_crc = get_u32_le(data + *pos + 9);
    /* (audit finding SEG-1): a frame record is at least the
     * 8-byte header (u32 page_no | u32 0) followed by page bytes, so
     * orig_len < OTSARDB_WAL_BATCH_FRAME_HEADER_SIZE must be rejected HERE,
     * before the decoder reads frame[0..7] past the malloc(orig_len)
     * allocation (the accept/reject decision used to depend on heap
     * garbage).
     *
     * (audit finding F03): fail closed on the FULL minimum
     * frame record, not just the 8-byte header. A frame record is exactly
     * 8 + page_size with page_size in [512, 65536] (SQLite power-of-two
     * constraint, enforced by wal_batch encode/decode and segment v3
     * rebuild), so orig_len in 8..519 can never be a valid record: rejecting
     * it here keeps every caller from ever seeing an undersized frame. */
    if (orig_len < OTSARDB_WAL_BATCH_FRAME_HEADER_SIZE + OTSARDB_WAL_BATCH_PAGE_SIZE_MIN ||
        orig_len > OTSARDB_SEGMENT_MAX_DECOMPRESSED ||
        (flags & ~OTSARDB_SEGMENT_BLOCK_FLAG_LZ4) != 0) {
        return 0;
    }
    if ((size_t)data_len > size - *pos - OTSARDB_SEGMENT_BLOCK_HEADER_SIZE) return 0;

    const unsigned char *stored = data + *pos + OTSARDB_SEGMENT_BLOCK_HEADER_SIZE;
    unsigned char *frame = (unsigned char *)malloc(orig_len);
    if (!frame) return 0;

    int ok = 1;
    if (flags & OTSARDB_SEGMENT_BLOCK_FLAG_LZ4) {
        int rc = LZ4_decompress_safe((const char *)stored,
                                     (char *)frame,
                                     (int)data_len,
                                     (int)orig_len);
        ok = rc == (int)orig_len;
    } else {
        ok = (size_t)data_len == (size_t)orig_len;
        if (ok) memcpy(frame, stored, orig_len);
    }
    if (ok && otsardb_segment_crc32(frame, orig_len) != frame_crc) ok = 0;
    if (ok) {
        uint32_t page_no = get_u32_le(frame);
        /* Frame records carry [u32 page_no][u32 0][page bytes]; the padding
         * is written 0 by every encoder (v3 and the WAL batch encoder). */
        if (page_no == 0 || get_u32_le(frame + 4) != 0) ok = 0;
        else {
            *pos += OTSARDB_SEGMENT_BLOCK_HEADER_SIZE + (size_t)data_len;
            *out_frame = frame;
            *out_frame_size = orig_len;
            *out_page_no = page_no;
            return 1;
        }
    }
    free(frame);
    return 0;
}

/* ---------------------------------------------------------------------------
 * v3 encoder: one block per unique page, sorted ascending, last-writer frame
 * wins, preceded by a copy of the WAL batch header (frame_count patched to
 * the unique page count so the rebuilt batch is exact and self-consistent).
 * ------------------------------------------------------------------------- */

typedef struct frame_page_ref {
    uint32_t page_no;
    uint32_t last_frame_index;
} frame_page_ref;

static int frame_page_ref_cmp(const void *a, const void *b) {
    const frame_page_ref *pa = (const frame_page_ref *)a;
    const frame_page_ref *pb = (const frame_page_ref *)b;
    if (pa->page_no < pb->page_no) return -1;
    if (pa->page_no > pb->page_no) return 1;
    return 0;
}

/* Build the v3 object. On success the caller owns *out_data (the frame info
 * arrays when out_frames is non-NULL). */
static int segment_encode_v3(const char *commit_id,
                             const void *payload,
                             size_t payload_size,
                             unsigned char **out_data,
                             size_t *out_size,
                             char out_object_sha256[OTSARDB_SEGMENT_SHA256_HEX_SIZE + 1],
                             int compress,
                             otsardb_segment_frame_info *out_frames) {
    otsardb_wal_batch_view batch;
    if (!otsardb_wal_batch_decode(payload, payload_size, &batch) ||
        batch.format_version != OTSARDB_WAL_BATCH_VERSION) {
        return 0;
    }
    size_t frame_record_size = OTSARDB_WAL_BATCH_FRAME_HEADER_SIZE + (size_t)batch.page_size;

    /* Unique pages with their last (winning) frame index, batch order. */
    frame_page_ref *refs = (frame_page_ref *)malloc(batch.frame_count * sizeof(*refs));
    if (!refs) return 0;
    uint32_t unique_count = 0;
    for (uint32_t i = 0; i < batch.frame_count; ++i) {
        uint32_t page_no = 0;
        const unsigned char *page = NULL;
        if (!otsardb_wal_batch_frame(&batch, i, &page_no, &page)) {
            free(refs);
            return 0;
        }
        uint32_t found = unique_count;
        for (uint32_t j = 0; j < unique_count; ++j) {
            if (refs[j].page_no == page_no) {
                found = j;
                break;
            }
        }
        if (found == unique_count) {
            refs[unique_count].page_no = page_no;
            ++unique_count;
        }
        refs[found].last_frame_index = i;
    }

    /* Sort by page number (v4 manifest page sets are sorted; the block table
     * must match them so ranges are contiguous). */
    qsort(refs, unique_count, sizeof(*refs), frame_page_ref_cmp);

    uint32_t page_size = batch.page_size;
    size_t orig_len = frame_record_size; /* == 8 + page_size */
    if (orig_len > UINT32_MAX || orig_len > (size_t)INT_MAX) {
        free(refs);
        return 0;
    }

    /* First pass: choose raw vs LZ4 per block and size the payload. */
    uint32_t *data_lens = (uint32_t *)malloc(unique_count * sizeof(*data_lens));
    unsigned char *block_flags = (unsigned char *)malloc(unique_count);
    if (!data_lens || !block_flags) {
        free(data_lens);
        free(block_flags);
        free(refs);
        return 0;
    }
    uint64_t blocks_bytes = 0;
    for (uint32_t i = 0; i < unique_count; ++i) {
        const unsigned char *frame = batch.data + OTSARDB_WAL_BATCH_HEADER_SIZE +
                                     (size_t)refs[i].last_frame_index * frame_record_size;
        (void)frame;
        if (compress && orig_len >= 64) {
            int bound = LZ4_compressBound((int)orig_len);
            if (bound > 0) {
                unsigned char *compressed = (unsigned char *)malloc((size_t)bound);
                if (compressed) {
                    int compressed_len = LZ4_compress_default((const char *)frame,
                                                              (char *)compressed,
                                                              (int)orig_len,
                                                              bound);
                    if (compressed_len > 0 && (size_t)compressed_len < orig_len) {
                        data_lens[i] = (uint32_t)compressed_len;
                        block_flags[i] = OTSARDB_SEGMENT_BLOCK_FLAG_LZ4;
                        free(compressed);
                        blocks_bytes += OTSARDB_SEGMENT_BLOCK_HEADER_SIZE +
                                        (size_t)compressed_len;
                        continue;
                    }
                    free(compressed);
                }
            }
        }
        data_lens[i] = (uint32_t)orig_len;
        block_flags[i] = 0;
        blocks_bytes += OTSARDB_SEGMENT_BLOCK_HEADER_SIZE + orig_len;
    }

    size_t stored_payload_size = OTSARDB_SEGMENT_BATCH_HEADER_COPY_SIZE + (size_t)blocks_bytes;
    size_t commit_id_len = strlen(commit_id);
    if (commit_id_len > OTSARDB_SEGMENT_COMMIT_ID_MAX ||
        OTSARDB_SEGMENT_HEADER_SIZE > SIZE_MAX - commit_id_len ||
        OTSARDB_SEGMENT_HEADER_SIZE + commit_id_len > SIZE_MAX - stored_payload_size) {
        free(data_lens);
        free(block_flags);
        free(refs);
        return 0;
    }
    size_t total = OTSARDB_SEGMENT_HEADER_SIZE + commit_id_len + stored_payload_size;

    unsigned char *encoded = (unsigned char *)malloc(total ? total : 1u);
    if (!encoded) {
        free(data_lens);
        free(block_flags);
        free(refs);
        return 0;
    }

    unsigned char *p = encoded;
    memcpy(p, OTSARDB_SEGMENT_MAGIC, OTSARDB_SEGMENT_MAGIC_SIZE); p += OTSARDB_SEGMENT_MAGIC_SIZE;
    put_u32_le(p, OTSARDB_SEGMENT_FORMAT_VERSION); p += 4;
    put_u32_le(p, OTSARDB_SEGMENT_WAL_FRAMES); p += 4;
    put_u16_le(p, (uint16_t)commit_id_len); p += 2;
    put_u16_le(p, 0); p += 2; /* v3: per-block flags only */
    put_u64_le(p, (uint64_t)stored_payload_size); p += 8;
    unsigned char *digest_slot = p; p += 32;
    memcpy(p, commit_id, commit_id_len); p += commit_id_len;

    /* Batch header copy with the frame count patched to the block count. */
    memcpy(p, batch.data, OTSARDB_WAL_BATCH_HEADER_SIZE);
    put_u32_le(p + 24, unique_count);
    p += OTSARDB_WAL_BATCH_HEADER_SIZE;

    uint64_t *offsets = NULL;
    uint32_t *page_nos = NULL;
    if (out_frames && unique_count > 0) {
        offsets = (uint64_t *)malloc((size_t)unique_count * sizeof(*offsets));
        page_nos = (uint32_t *)malloc((size_t)unique_count * sizeof(*page_nos));
        if (!offsets || !page_nos) {
            free(offsets);
            free(page_nos);
            free(data_lens);
            free(block_flags);
            free(refs);
            free(encoded);
            return 0;
        }
    }

    for (uint32_t i = 0; i < unique_count; ++i) {
        const unsigned char *frame = batch.data + OTSARDB_WAL_BATCH_HEADER_SIZE +
                                     (size_t)refs[i].last_frame_index * frame_record_size;
        size_t block_offset = (size_t)(p - encoded);
        if (offsets) offsets[i] = (uint64_t)block_offset;
        if (page_nos) page_nos[i] = refs[i].page_no;

        put_u32_le(p, (uint32_t)orig_len); p += 4;
        put_u32_le(p, data_lens[i]); p += 4;
        *p++ = block_flags[i];
        put_u32_le(p, otsardb_segment_crc32(frame, orig_len)); p += 4;
        if (block_flags[i] & OTSARDB_SEGMENT_BLOCK_FLAG_LZ4) {
            int bound = LZ4_compressBound((int)orig_len);
            if (bound <= 0 ||
                LZ4_compress_default((const char *)frame,
                                     (char *)p,
                                     (int)orig_len,
                                     bound) != (int)data_lens[i]) {
                free(offsets);
                free(page_nos);
                free(data_lens);
                free(block_flags);
                free(refs);
                free(encoded);
                return 0;
            }
        } else {
            memcpy(p, frame, orig_len);
        }
        p += data_lens[i];
    }
    if ((size_t)(p - encoded) != total) {
        free(offsets);
        free(page_nos);
        free(data_lens);
        free(block_flags);
        free(refs);
        free(encoded);
        return 0;
    }

    unsigned char payload_digest[OTSARDB_SHA256_SIZE];
    if (!otsardb_sha256(encoded + OTSARDB_SEGMENT_HEADER_SIZE + commit_id_len,
                      stored_payload_size,
                      payload_digest)) {
        free(offsets);
        free(page_nos);
        free(data_lens);
        free(block_flags);
        free(refs);
        free(encoded);
        return 0;
    }
    memcpy(digest_slot, payload_digest, OTSARDB_SHA256_SIZE);

    if (!otsardb_sha256_hex_data(encoded, total, out_object_sha256)) {
        free(offsets);
        free(page_nos);
        free(data_lens);
        free(block_flags);
        free(refs);
        free(encoded);
        return 0;
    }

    free(data_lens);
    free(block_flags);
    free(refs);

    if (out_frames) {
        out_frames->offsets = offsets;
        out_frames->page_nos = page_nos;
        out_frames->count = unique_count;
    }

    *out_data = encoded;
    *out_size = total;
    return 1;
}

void otsardb_segment_frame_info_release(otsardb_segment_frame_info *info) {
    if (!info) return;
    free(info->offsets);
    free(info->page_nos);
    memset(info, 0, sizeof(*info));
}

int otsardb_segment_encode_with_frames(uint32_t kind,
                                     const char *commit_id,
                                     const void *payload,
                                     size_t payload_size,
                                     unsigned char **out_data,
                                     size_t *out_size,
                                     char out_object_sha256[OTSARDB_SEGMENT_SHA256_HEX_SIZE + 1],
                                     int compress,
                                     otsardb_segment_frame_info *out_frames) {
    if (out_frames) memset(out_frames, 0, sizeof(*out_frames));
    if (!commit_id || commit_id[0] == '\0' || (!payload && payload_size != 0) ||
        !out_data || !out_size || !out_object_sha256 || kind == 0) {
        return 0;
    }
    if (kind == OTSARDB_SEGMENT_WAL_FRAMES) {
        if (segment_encode_v3(commit_id, payload, payload_size, out_data, out_size,
                              out_object_sha256, compress, out_frames)) {
            return 1;
        }
        /* Failure paths inside segment_encode_v3 may have freed the frame
         * arrays while leaving the pointers set: zero them before release. */
        if (out_frames) {
            out_frames->offsets = NULL;
            out_frames->page_nos = NULL;
            out_frames->count = 0;
            otsardb_segment_frame_info_release(out_frames);
        }
    }
    /* Fallback: v2 layout (whole-payload LZ4), no frame table. */
    return otsardb_segment_encode(kind, commit_id, payload, payload_size,
                                out_data, out_size, out_object_sha256, compress);
}

int otsardb_segment_encode(uint32_t kind,
                         const char *commit_id,
                         const void *payload,
                         size_t payload_size,
                         unsigned char **out_data,
                         size_t *out_size,
                         char out_object_sha256[OTSARDB_SEGMENT_SHA256_HEX_SIZE + 1],
                         int compress) {
    if (!commit_id || commit_id[0] == '\0' || (!payload && payload_size != 0) ||
        !out_data || !out_size || !out_object_sha256 || kind == 0) return 0;

    size_t commit_id_len = strlen(commit_id);
    if (commit_id_len > OTSARDB_SEGMENT_COMMIT_ID_MAX || commit_id_len > UINT16_MAX) return 0;
    if (payload_size > UINT64_MAX) return 0;

    const void *stored_payload = payload;
    size_t stored_size = payload_size;
    uint16_t flags = 0;
    unsigned char *compressed_payload = NULL;

    if (compress && payload_size >= 64 && payload_size <= (size_t)INT_MAX) {
        int bound = LZ4_compressBound((int)payload_size);
        if (bound > 0) {
            size_t cap = (size_t)bound + 4u;
            compressed_payload = (unsigned char *)malloc(cap);
            if (compressed_payload) {
                int compressed_len = LZ4_compress_default((const char *)payload,
                                                          (char *)(compressed_payload + 4u),
                                                          (int)payload_size,
                                                          (int)(cap - 4u));
                if (compressed_len > 0 && (size_t)compressed_len + 4u < payload_size) {
                    put_u32_le(compressed_payload, (uint32_t)payload_size);
                    stored_payload = compressed_payload;
                    stored_size = (size_t)compressed_len + 4u;
                    flags = OTSARDB_SEGMENT_FLAG_LZ4;
                } else {
                    free(compressed_payload);
                    compressed_payload = NULL;
                }
            }
        }
    }

    if (OTSARDB_SEGMENT_HEADER_SIZE > SIZE_MAX - commit_id_len ||
        OTSARDB_SEGMENT_HEADER_SIZE + commit_id_len > SIZE_MAX - stored_size) {
        free(compressed_payload);
        return 0;
    }

    size_t total = OTSARDB_SEGMENT_HEADER_SIZE + commit_id_len + stored_size;
    unsigned char *encoded = (unsigned char *)malloc(total ? total : 1);
    if (!encoded) {
        free(compressed_payload);
        return 0;
    }

    unsigned char payload_digest[OTSARDB_SHA256_SIZE];
    if (!otsardb_sha256(stored_payload, stored_size, payload_digest)) {
        free(compressed_payload);
        free(encoded);
        return 0;
    }

    unsigned char *p = encoded;
    memcpy(p, OTSARDB_SEGMENT_MAGIC, OTSARDB_SEGMENT_MAGIC_SIZE); p += OTSARDB_SEGMENT_MAGIC_SIZE;
    put_u32_le(p, 2u); p += 4; /* v2 */
    put_u32_le(p, kind); p += 4;
    put_u16_le(p, (uint16_t)commit_id_len); p += 2;
    put_u16_le(p, flags); p += 2;
    put_u64_le(p, (uint64_t)stored_size); p += 8;
    memcpy(p, payload_digest, OTSARDB_SHA256_SIZE); p += OTSARDB_SHA256_SIZE;
    memcpy(p, commit_id, commit_id_len); p += commit_id_len;
    if (stored_size) memcpy(p, stored_payload, stored_size);

    free(compressed_payload);

    if (!otsardb_sha256_hex_data(encoded, total, out_object_sha256)) {
        free(encoded);
        return 0;
    }

    *out_data = encoded;
    *out_size = total;
    return 1;
}

int otsardb_segment_decode(const void *data, size_t size, otsardb_segment_view *out_view) {
    if (!data || !out_view || size < OTSARDB_SEGMENT_HEADER_SIZE) return 0;

    const unsigned char *p = (const unsigned char *)data;
    if (memcmp(p, OTSARDB_SEGMENT_MAGIC, OTSARDB_SEGMENT_MAGIC_SIZE) != 0) return 0;
    p += OTSARDB_SEGMENT_MAGIC_SIZE;

    uint32_t format_version = get_u32_le(p); p += 4;
    uint32_t kind = get_u32_le(p); p += 4;
    uint16_t commit_id_len = get_u16_le(p); p += 2;
    uint16_t flags = get_u16_le(p); p += 2;
    uint64_t payload_size_u64 = get_u64_le(p); p += 8;
    const unsigned char *expected_payload_digest = p; p += OTSARDB_SHA256_SIZE;

    if ((format_version != 1u && format_version != 2u &&
         format_version != OTSARDB_SEGMENT_FORMAT_VERSION) ||
        kind == 0 ||
        commit_id_len == 0 || commit_id_len > OTSARDB_SEGMENT_COMMIT_ID_MAX ||
        payload_size_u64 > SIZE_MAX ||
        (flags & ~OTSARDB_SEGMENT_FLAG_LZ4) != 0 ||
        (format_version == 1u && flags != 0) ||
        (format_version >= 3u && flags != 0)) return 0;

    size_t payload_size = (size_t)payload_size_u64;
    size_t expected_size = OTSARDB_SEGMENT_HEADER_SIZE + (size_t)commit_id_len;
    if (expected_size > SIZE_MAX - payload_size) return 0;
    expected_size += payload_size;
    if (size != expected_size) return 0;

    memset(out_view, 0, sizeof(*out_view));
    out_view->format_version = format_version;
    out_view->kind = kind;
    memcpy(out_view->commit_id, p, commit_id_len);
    out_view->commit_id[commit_id_len] = '\0';
    p += commit_id_len;
    out_view->payload = p;
    out_view->payload_size = payload_size;
    /* v3 payloads are always rebuilt from blocks on decompress. */
    out_view->compressed = (format_version >= 3u) || (flags & OTSARDB_SEGMENT_FLAG_LZ4) != 0;

    unsigned char actual_payload_digest[OTSARDB_SHA256_SIZE];
    if (!otsardb_sha256(out_view->payload, payload_size, actual_payload_digest) ||
        memcmp(actual_payload_digest, expected_payload_digest, OTSARDB_SHA256_SIZE) != 0) return 0;

    otsardb_sha256_hex(actual_payload_digest, out_view->payload_sha256);
    return 1;
}

/* Rebuild the exact WAL batch from a v3 block payload. */
static int segment_decompress_v3(const unsigned char *payload,
                                 size_t size,
                                 void **out_data,
                                 size_t *out_size) {
    if (!payload || size < OTSARDB_SEGMENT_BATCH_HEADER_COPY_SIZE ||
        memcmp(payload, "OTSWAL01", 8) != 0) {
        return 0;
    }
    uint32_t page_size = get_u32_le(payload + 12);
    uint32_t frame_count = get_u32_le(payload + 24);
    if (page_size < OTSARDB_WAL_BATCH_PAGE_SIZE_MIN ||
        page_size > OTSARDB_WAL_BATCH_PAGE_SIZE_MAX ||
        (page_size & (page_size - 1u)) != 0 ||
        frame_count == 0) {
        return 0;
    }
    size_t frame_record_size = OTSARDB_WAL_BATCH_FRAME_HEADER_SIZE + (size_t)page_size;
    if ((size_t)frame_count > (OTSARDB_SEGMENT_MAX_DECOMPRESSED - OTSARDB_WAL_BATCH_HEADER_SIZE) /
                                   frame_record_size) {
        return 0;
    }
    size_t expected_out = OTSARDB_WAL_BATCH_HEADER_SIZE + (size_t)frame_count * frame_record_size;

    unsigned char *out = (unsigned char *)malloc(expected_out ? expected_out : 1u);
    if (!out) return 0;
    memcpy(out, payload, OTSARDB_WAL_BATCH_HEADER_SIZE);

    size_t pos = OTSARDB_SEGMENT_BATCH_HEADER_COPY_SIZE;
    size_t out_pos = OTSARDB_WAL_BATCH_HEADER_SIZE;
    int ok = 1;
    for (uint32_t i = 0; i < frame_count && ok; ++i) {
        unsigned char *frame = NULL;
        size_t frame_size = 0;
        uint32_t page_no = 0;
        if (!otsardb_segment_frame_decode(payload, size, &pos, &frame, &frame_size, &page_no) ||
            frame_size != frame_record_size) {
            ok = 0;
            break;
        }
        memcpy(out + out_pos, frame, frame_size);
        out_pos += frame_size;
        free(frame);
    }
    if (ok && (out_pos != expected_out || pos != size)) ok = 0;
    if (ok) {
        otsardb_wal_batch_view batch;
        ok = otsardb_wal_batch_decode(out, expected_out, &batch);
    }
    if (!ok) {
        free(out);
        return 0;
    }

    *out_data = out;
    *out_size = expected_out;
    return 1;
}

int otsardb_segment_decompress(const otsardb_segment_view *view,
                             void **out_data,
                             size_t *out_size) {
    if (!view || !out_data || !out_size) return 0;

    if (view->format_version >= 3u) {
        return segment_decompress_v3(view->payload, view->payload_size, out_data, out_size);
    }
    if (!view->compressed) return 0;

    const unsigned char *stored = view->payload;
    size_t stored_size = view->payload_size;
    if (stored_size < 4) return 0;

    uint32_t original_size = get_u32_le(stored);
    if (original_size == 0 || original_size > OTSARDB_SEGMENT_MAX_DECOMPRESSED) return 0;

    unsigned char *out = (unsigned char *)malloc(original_size);
    if (!out) return 0;

    int rc = LZ4_decompress_safe((const char *)(stored + 4u),
                                 (char *)out,
                                 (int)(stored_size - 4u),
                                 (int)original_size);
    if (rc != (int)original_size) {
        free(out);
        return 0;
    }

    *out_data = out;
    *out_size = original_size;
    return 1;
}
