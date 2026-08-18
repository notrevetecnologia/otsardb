#ifndef OTSARDB_CHECKPOINT_PAGE_H
#define OTSARDB_CHECKPOINT_PAGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OTSARDB_CHECKPOINT_PAGE_FORMAT_VERSION 1u
#define OTSARDB_CHECKPOINT_PAGE_FLAG_LZ4 0x00000001u

typedef struct otsardb_checkpoint_page_view {
    uint32_t format_version;
    uint32_t page_size;
    uint64_t database_size_pages;
    uint32_t page_count;
    const unsigned char *data;
    size_t size;
} otsardb_checkpoint_page_view;

int otsardb_checkpoint_page_encode(uint32_t page_size,
                                 uint64_t database_size_pages,
                                 const uint32_t *page_nos,
                                 const unsigned char *pages,
                                 uint32_t page_count,
                                 unsigned char **out_data,
                                 size_t *out_size);

int otsardb_checkpoint_page_decode(const void *data, size_t size,
                                 otsardb_checkpoint_page_view *out);

int otsardb_checkpoint_page_frame(const otsardb_checkpoint_page_view *view,
                                uint32_t index,
                                uint32_t *out_page_no,
                                int *out_compressed,
                                const unsigned char **out_page_data,
                                size_t *out_page_size);

#ifdef __cplusplus
}
#endif

#endif
