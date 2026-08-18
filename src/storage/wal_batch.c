#include "wal_batch.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const unsigned char OTSARDB_WAL_BATCH_MAGIC[OTSARDB_WAL_BATCH_MAGIC_SIZE] = {
    'O', 'T', 'S', 'W', 'A', 'L', '0', '1'
};

static void put_u32_le(unsigned char *p, uint32_t value) {
    p[0] = (unsigned char)(value & 0xffu);
    p[1] = (unsigned char)((value >> 8) & 0xffu);
    p[2] = (unsigned char)((value >> 16) & 0xffu);
    p[3] = (unsigned char)((value >> 24) & 0xffu);
}

static void put_u64_le(unsigned char *p, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        p[i] = (unsigned char)((value >> (i * 8)) & 0xffu);
    }
}

static uint32_t get_u32_le(const unsigned char *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t get_u64_le(const unsigned char *p) {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= ((uint64_t)p[i]) << (i * 8);
    }
    return value;
}

static int valid_page_size(uint32_t page_size) {
    return page_size >= 512 && page_size <= 65536 &&
           (page_size & (page_size - 1)) == 0;
}

int otsardb_wal_batch_encode(uint32_t page_size,
                           uint64_t database_size_pages,
                           const uint32_t *page_nos,
                           const unsigned char *pages,
                           uint32_t frame_count,
                           unsigned char **out_data,
                           size_t *out_size) {
    if (!valid_page_size(page_size) || database_size_pages == 0 ||
        !page_nos || !pages || frame_count == 0 || !out_data || !out_size) {
        return 0;
    }

    size_t record_size = OTSARDB_WAL_BATCH_FRAME_HEADER_SIZE + (size_t)page_size;
    if ((size_t)frame_count > (SIZE_MAX - OTSARDB_WAL_BATCH_HEADER_SIZE) / record_size) {
        return 0;
    }
    size_t total_size = OTSARDB_WAL_BATCH_HEADER_SIZE + (size_t)frame_count * record_size;

    unsigned char *encoded = (unsigned char *)malloc(total_size);
    if (!encoded) return 0;

    memcpy(encoded, OTSARDB_WAL_BATCH_MAGIC, sizeof(OTSARDB_WAL_BATCH_MAGIC));
    put_u32_le(encoded + 8, OTSARDB_WAL_BATCH_VERSION);
    put_u32_le(encoded + 12, page_size);
    put_u64_le(encoded + 16, database_size_pages);
    put_u32_le(encoded + 24, frame_count);
    put_u32_le(encoded + 28, 0);

    unsigned char *cursor = encoded + OTSARDB_WAL_BATCH_HEADER_SIZE;
    for (uint32_t i = 0; i < frame_count; ++i) {
        if (page_nos[i] == 0) {
            free(encoded);
            return 0;
        }
        put_u32_le(cursor, page_nos[i]);
        put_u32_le(cursor + 4, 0);
        memcpy(cursor + OTSARDB_WAL_BATCH_FRAME_HEADER_SIZE,
               pages + (size_t)i * page_size,
               page_size);
        cursor += record_size;
    }

    *out_data = encoded;
    *out_size = total_size;
    return 1;
}

int otsardb_wal_batch_decode(const void *data,
                           size_t size,
                           otsardb_wal_batch_view *out_batch) {
    if (!data || !out_batch || size < OTSARDB_WAL_BATCH_HEADER_SIZE) return 0;

    const unsigned char *encoded = (const unsigned char *)data;
    if (memcmp(encoded, OTSARDB_WAL_BATCH_MAGIC, sizeof(OTSARDB_WAL_BATCH_MAGIC)) != 0) return 0;

    uint32_t format_version = get_u32_le(encoded + 8);
    uint32_t page_size = get_u32_le(encoded + 12);
    uint64_t database_size_pages = get_u64_le(encoded + 16);
    uint32_t frame_count = get_u32_le(encoded + 24);

    if (format_version != OTSARDB_WAL_BATCH_VERSION || !valid_page_size(page_size) ||
        database_size_pages == 0 || frame_count == 0) {
        return 0;
    }

    size_t record_size = OTSARDB_WAL_BATCH_FRAME_HEADER_SIZE + (size_t)page_size;
    if ((size_t)frame_count > (SIZE_MAX - OTSARDB_WAL_BATCH_HEADER_SIZE) / record_size ||
        OTSARDB_WAL_BATCH_HEADER_SIZE + (size_t)frame_count * record_size != size) {
        return 0;
    }

    memset(out_batch, 0, sizeof(*out_batch));
    out_batch->format_version = format_version;
    out_batch->page_size = page_size;
    out_batch->database_size_pages = database_size_pages;
    out_batch->frame_count = frame_count;
    out_batch->data = encoded;
    out_batch->size = size;
    return 1;
}

int otsardb_wal_batch_frame(const otsardb_wal_batch_view *batch,
                          uint32_t index,
                          uint32_t *out_page_no,
                          const unsigned char **out_page) {
    if (!batch || index >= batch->frame_count || !out_page_no || !out_page) return 0;

    size_t record_size = OTSARDB_WAL_BATCH_FRAME_HEADER_SIZE + (size_t)batch->page_size;
    const unsigned char *record = batch->data + OTSARDB_WAL_BATCH_HEADER_SIZE +
                                  (size_t)index * record_size;
    uint32_t page_no = get_u32_le(record);
    if (page_no == 0) return 0;

    *out_page_no = page_no;
    *out_page = record + OTSARDB_WAL_BATCH_FRAME_HEADER_SIZE;
    return 1;
}
