#include "sqlite_wal.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define OTSARDB_SQLITE_WAL_MAGIC_LE 0x377f0682u
#define OTSARDB_SQLITE_WAL_MAGIC_BE 0x377f0683u

static uint32_t read_u32_be(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

static uint32_t read_checksum_word(const unsigned char *p, int big_endian) {
    if (big_endian) return read_u32_be(p);
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void checksum_bytes(const unsigned char *data,
                           size_t size,
                           int big_endian,
                           uint32_t *s0,
                           uint32_t *s1) {
    for (size_t i = 0; i < size; i += 8) {
        uint32_t first = read_checksum_word(data + i, big_endian);
        uint32_t second = read_checksum_word(data + i + 4, big_endian);
        *s0 += first + *s1;
        *s1 += second + *s0;
    }
}

static int valid_page_size(uint32_t page_size) {
    return page_size >= 512 && page_size <= 65536 &&
           (page_size & (page_size - 1)) == 0;
}

int otsardb_sqlite_wal_read_header(otsardb_sqlite_wal_read_at read_at,
                                 void *read_ctx,
                                 uint64_t wal_size,
                                 otsardb_sqlite_wal_header *out_header) {
    if (!read_at || !out_header || wal_size < OTSARDB_SQLITE_WAL_HEADER_SIZE) return 0;

    unsigned char wal[OTSARDB_SQLITE_WAL_HEADER_SIZE];
    if (!read_at(read_ctx, 0, wal, sizeof(wal))) return 0;

    uint32_t magic = read_u32_be(wal);
    if (magic != OTSARDB_SQLITE_WAL_MAGIC_LE && magic != OTSARDB_SQLITE_WAL_MAGIC_BE) return 0;

    int checksum_big_endian = magic == OTSARDB_SQLITE_WAL_MAGIC_BE;
    uint32_t page_size = read_u32_be(wal + 8);
    if (!valid_page_size(page_size)) return 0;

    uint32_t checksum0 = 0;
    uint32_t checksum1 = 0;
    checksum_bytes(wal, 24, checksum_big_endian, &checksum0, &checksum1);
    if (checksum0 != read_u32_be(wal + 24) || checksum1 != read_u32_be(wal + 28)) {
        return 0;
    }

    memset(out_header, 0, sizeof(*out_header));
    out_header->page_size = page_size;
    out_header->salt1 = read_u32_be(wal + 16);
    out_header->salt2 = read_u32_be(wal + 20);
    out_header->checksum0 = checksum0;
    out_header->checksum1 = checksum1;
    out_header->checksum_big_endian = checksum_big_endian;
    return 1;
}

void otsardb_sqlite_wal_cursor_reset(otsardb_sqlite_wal_cursor *cursor,
                                   const otsardb_sqlite_wal_header *header) {
    if (!cursor || !header) return;
    memset(cursor, 0, sizeof(*cursor));
    cursor->header = *header;
    cursor->checksum0 = header->checksum0;
    cursor->checksum1 = header->checksum1;
}

static int ensure_frame_ref_capacity(otsardb_sqlite_wal_frame_ref **refs,
                                     size_t *capacity,
                                     size_t required) {
    if (required <= *capacity) return 1;

    size_t next = *capacity ? *capacity : 8u;
    while (next < required) {
        if (next > SIZE_MAX / 2u) return 0;
        next *= 2u;
    }
    if (next > SIZE_MAX / sizeof(otsardb_sqlite_wal_frame_ref)) return 0;

    otsardb_sqlite_wal_frame_ref *next_refs =
        (otsardb_sqlite_wal_frame_ref *)realloc(*refs,
                                              next * sizeof(otsardb_sqlite_wal_frame_ref));
    if (!next_refs) return 0;
    *refs = next_refs;
    *capacity = next;
    return 1;
}

int otsardb_sqlite_wal_scan_reader(otsardb_sqlite_wal_read_at read_at,
                                 void *read_ctx,
                                 uint64_t wal_size,
                                 otsardb_sqlite_wal_cursor *cursor,
                                 uint32_t commit_limit,
                                 otsardb_sqlite_wal_commit_callback callback,
                                 void *callback_ctx,
                                 otsardb_sqlite_wal_scan_result *out_result) {
    if (out_result) memset(out_result, 0, sizeof(*out_result));
    if (!read_at || !cursor || cursor->header.page_size == 0) return 0;

    const uint32_t page_size = cursor->header.page_size;
    const uint64_t frame_size = OTSARDB_SQLITE_WAL_FRAME_HEADER_SIZE + (uint64_t)page_size;
    if (cursor->frame_count > (UINT64_MAX - OTSARDB_SQLITE_WAL_HEADER_SIZE) / frame_size) return 0;

    uint64_t offset = OTSARDB_SQLITE_WAL_HEADER_SIZE + (uint64_t)cursor->frame_count * frame_size;
    if (offset > wal_size) return 0;

    unsigned char *frame = (unsigned char *)malloc((size_t)frame_size);
    if (!frame) return 0;

    /* Only frame references (page number + WAL offset) are accumulated here.
     * Page images stay in the WAL file and are re-read by the commit callback
     * at encode time, bounding memory by chunk budget, not transaction size. */
    otsardb_sqlite_wal_frame_ref *refs = NULL;
    size_t ref_capacity = 0;
    uint32_t pending_frames = 0;

    uint32_t checksum0 = cursor->checksum0;
    uint32_t checksum1 = cursor->checksum1;
    uint32_t absolute_frame_count = cursor->frame_count;
    uint32_t valid_frames = 0;
    uint32_t committed_frames = cursor->frame_count;
    uint32_t commit_count = 0;
    uint64_t bytes_read = 0;

    while (offset <= wal_size && wal_size - offset >= frame_size) {
        if (!read_at(read_ctx, offset, frame, (size_t)frame_size)) {
            free(frame);
            free(refs);
            return 0;
        }
        bytes_read += frame_size;

        uint32_t page_no = read_u32_be(frame);
        uint32_t database_size_pages = read_u32_be(frame + 4);
        if (page_no == 0 ||
            read_u32_be(frame + 8) != cursor->header.salt1 ||
            read_u32_be(frame + 12) != cursor->header.salt2) {
            break;
        }

        uint32_t next_checksum0 = checksum0;
        uint32_t next_checksum1 = checksum1;
        checksum_bytes(frame, 8, cursor->header.checksum_big_endian, &next_checksum0, &next_checksum1);
        checksum_bytes(frame + OTSARDB_SQLITE_WAL_FRAME_HEADER_SIZE,
                       page_size,
                       cursor->header.checksum_big_endian,
                       &next_checksum0,
                       &next_checksum1);
        if (next_checksum0 != read_u32_be(frame + 16) ||
            next_checksum1 != read_u32_be(frame + 20)) {
            break;
        }

        if (!ensure_frame_ref_capacity(&refs, &ref_capacity, (size_t)pending_frames + 1u)) {
            free(frame);
            free(refs);
            return 0;
        }

        refs[pending_frames].page_no = page_no;
        refs[pending_frames].frame_offset = offset;
        ++pending_frames;
        ++absolute_frame_count;
        ++valid_frames;
        checksum0 = next_checksum0;
        checksum1 = next_checksum1;
        offset += frame_size;

        if (database_size_pages != 0) {
            if (callback && callback(page_size,
                                     database_size_pages,
                                     refs,
                                     pending_frames,
                                     read_at,
                                     read_ctx,
                                     callback_ctx) != 0) {
                free(frame);
                free(refs);
                return 0;
            }

            cursor->frame_count = absolute_frame_count;
            cursor->checksum0 = checksum0;
            cursor->checksum1 = checksum1;
            committed_frames = absolute_frame_count;
            ++commit_count;
            pending_frames = 0;

            if (commit_limit != 0 && commit_count >= commit_limit) break;
        }
    }

    free(frame);
    free(refs);

    if (out_result) {
        out_result->page_size = page_size;
        out_result->salt1 = cursor->header.salt1;
        out_result->salt2 = cursor->header.salt2;
        out_result->valid_frames = valid_frames;
        out_result->committed_frames = committed_frames;
        out_result->commit_count = commit_count;
        out_result->bytes_read = bytes_read;
    }
    return 1;
}

typedef struct memory_reader {
    const unsigned char *data;
    size_t size;
} memory_reader;

static int memory_read_at(void *ctx, uint64_t offset, void *buffer, size_t size) {
    memory_reader *reader = (memory_reader *)ctx;
    if (!reader || !buffer || offset > reader->size || size > reader->size - (size_t)offset) {
        return 0;
    }
    if (size) memcpy(buffer, reader->data + (size_t)offset, size);
    return 1;
}

int otsardb_sqlite_wal_scan(const void *wal_data,
                          size_t wal_size,
                          otsardb_sqlite_wal_commit_callback callback,
                          void *ctx,
                          otsardb_sqlite_wal_scan_result *out_result) {
    if (out_result) memset(out_result, 0, sizeof(*out_result));
    if (!wal_data || wal_size < OTSARDB_SQLITE_WAL_HEADER_SIZE) return 0;

    memory_reader reader = {(const unsigned char *)wal_data, wal_size};
    otsardb_sqlite_wal_header header;
    if (!otsardb_sqlite_wal_read_header(memory_read_at, &reader, wal_size, &header)) return 0;

    otsardb_sqlite_wal_cursor cursor;
    otsardb_sqlite_wal_cursor_reset(&cursor, &header);

    otsardb_sqlite_wal_scan_result scan;
    if (!otsardb_sqlite_wal_scan_reader(memory_read_at,
                                      &reader,
                                      wal_size,
                                      &cursor,
                                      0,
                                      callback,
                                      ctx,
                                      &scan)) {
        return 0;
    }

    scan.bytes_read += OTSARDB_SQLITE_WAL_HEADER_SIZE;
    if (out_result) *out_result = scan;
    return 1;
}
