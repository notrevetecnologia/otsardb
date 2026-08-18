#include "checkpoint_page.h"
#include "lz4.h"

#include <stdlib.h>
#include <string.h>

#define OTSARDB_CP_MAGIC_SIZE 8
#define OTSARDB_CP_HEADER_SIZE 32

static const unsigned char OTSARDB_CP_MAGIC[OTSARDB_CP_MAGIC_SIZE] = {
    'A','R','K','C','P','0','1'
};

static void put_u32_le(unsigned char *p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = (unsigned char)((v >> (i * 8)) & 0xffu);
}

static void put_u64_le(unsigned char *p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = (unsigned char)((v >> (i * 8)) & 0xffu);
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

int otsardb_checkpoint_page_encode(uint32_t page_size,
                                 uint64_t database_size_pages,
                                 const uint32_t *page_nos,
                                 const unsigned char *pages,
                                 uint32_t page_count,
                                 unsigned char **out_data,
                                 size_t *out_size) {
    if (!page_nos || !pages || !out_data || !out_size || page_size == 0 ||
        page_count == 0) return 0;

    for (uint32_t i = 0; i < page_count; ++i) {
        if (page_nos[i] == 0 || page_nos[i] > database_size_pages) return 0;
    }

    size_t per_record = 12u + (size_t)page_size;
    if (per_record > SIZE_MAX / page_count) return 0;
    if (OTSARDB_CP_HEADER_SIZE > SIZE_MAX - (per_record * page_count)) return 0;

    size_t total = OTSARDB_CP_HEADER_SIZE + per_record * page_count;
    unsigned char *encoded = (unsigned char *)malloc(total ? total : 1);
    if (!encoded) return 0;

    unsigned char *p = encoded;
    memcpy(p, OTSARDB_CP_MAGIC, OTSARDB_CP_MAGIC_SIZE); p += OTSARDB_CP_MAGIC_SIZE;
    put_u32_le(p, OTSARDB_CHECKPOINT_PAGE_FORMAT_VERSION); p += 4;
    put_u32_le(p, page_size); p += 4;
    put_u64_le(p, database_size_pages); p += 8;
    put_u32_le(p, page_count); p += 4;
    put_u32_le(p, 0); p += 4;

    for (uint32_t i = 0; i < page_count; ++i) {
        put_u32_le(p, page_nos[i]); p += 4;
        put_u32_le(p, 0); p += 4;
        put_u32_le(p, page_size); p += 4;
        memcpy(p, pages + (size_t)i * page_size, page_size);
        p += page_size;
    }

    *out_data = encoded;
    *out_size = total;
    return 1;
}

int otsardb_checkpoint_page_decode(const void *data, size_t size,
                                 otsardb_checkpoint_page_view *out) {
    if (!data || !out || size < OTSARDB_CP_HEADER_SIZE) return 0;

    const unsigned char *p = (const unsigned char *)data;
    if (memcmp(p, OTSARDB_CP_MAGIC, OTSARDB_CP_MAGIC_SIZE) != 0) return 0;
    p += OTSARDB_CP_MAGIC_SIZE;

    uint32_t format_version = get_u32_le(p); p += 4;
    if (format_version != OTSARDB_CHECKPOINT_PAGE_FORMAT_VERSION) return 0;

    uint32_t page_size = get_u32_le(p); p += 4;
    if (page_size == 0) return 0;

    uint64_t database_size_pages = get_u64_le(p); p += 8;
    uint32_t page_count = get_u32_le(p); p += 4;
    if (page_count == 0) return 0;

    uint32_t reserved = get_u32_le(p); p += 4;
    (void)reserved;

    const unsigned char *record_start = p;
    size_t expected_size = OTSARDB_CP_HEADER_SIZE;
    uint32_t max_page_no = 0;
    for (uint32_t i = 0; i < page_count; ++i) {
        if ((size_t)(p - (const unsigned char *)data) + 12 > size) return 0;
        uint32_t page_no = get_u32_le(p); p += 4;
        uint32_t flags = get_u32_le(p); p += 4;
        uint32_t stored_size = get_u32_le(p); p += 4;
        (void)flags;

        if (page_no > max_page_no) max_page_no = page_no;

        if (stored_size > SIZE_MAX - 12u) return 0;
        if (expected_size > SIZE_MAX - 12u - stored_size) return 0;
        expected_size += 12u + stored_size;

        if ((size_t)(p - (const unsigned char *)data) + stored_size > size) return 0;
        p += stored_size;
    }

    if (size != expected_size) return 0;
    if (max_page_no == 0 || max_page_no > database_size_pages) return 0;

    memset(out, 0, sizeof(*out));
    out->format_version = format_version;
    out->page_size = page_size;
    out->database_size_pages = database_size_pages;
    out->page_count = page_count;
    out->data = record_start;
    out->size = size;
    return 1;
}

int otsardb_checkpoint_page_frame(const otsardb_checkpoint_page_view *view,
                                uint32_t index,
                                uint32_t *out_page_no,
                                int *out_compressed,
                                const unsigned char **out_page_data,
                                size_t *out_page_size) {
    if (!view || !out_page_no || !out_compressed || !out_page_data ||
        !out_page_size) return 0;
    if (index >= view->page_count) return 0;

    const unsigned char *p = view->data;
    for (uint32_t i = 0; i <= index; ++i) {
        uint32_t stored_size = get_u32_le(p + 8);
        if (i == index) {
            *out_page_no = get_u32_le(p);
            uint32_t flags = get_u32_le(p + 4);
            *out_compressed = (flags & OTSARDB_CHECKPOINT_PAGE_FLAG_LZ4) ? 1 : 0;
            *out_page_data = p + 12;
            *out_page_size = stored_size;
            return 1;
        }
        p += 12 + stored_size;
    }
    return 0;
}
