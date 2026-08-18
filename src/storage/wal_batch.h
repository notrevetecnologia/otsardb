#ifndef OTSARDB_WAL_BATCH_H
#define OTSARDB_WAL_BATCH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OTSARDB_WAL_BATCH_VERSION 1u
#define OTSARDB_WAL_BATCH_MAGIC_SIZE 8u
#define OTSARDB_WAL_BATCH_HEADER_SIZE 32u
#define OTSARDB_WAL_BATCH_FRAME_HEADER_SIZE 8u

/* SQLite page sizes are powers of two in [512, 65536]; a WAL batch record is
 * FRAME_HEADER_SIZE + page_size bytes, so a frame record is never smaller
 * than FRAME_HEADER_SIZE + PAGE_SIZE_MIN. (F03) uses the
 * minimum to reject impossible orig_len values at v3 frame-decode time. */
#define OTSARDB_WAL_BATCH_PAGE_SIZE_MIN 512u
#define OTSARDB_WAL_BATCH_PAGE_SIZE_MAX 65536u

typedef struct otsardb_wal_batch_view {
    uint32_t format_version;
    uint32_t page_size;
    uint64_t database_size_pages;
    uint32_t frame_count;
    const unsigned char *data;
    size_t size;
} otsardb_wal_batch_view;

int otsardb_wal_batch_encode(uint32_t page_size,
                           uint64_t database_size_pages,
                           const uint32_t *page_nos,
                           const unsigned char *pages,
                           uint32_t frame_count,
                           unsigned char **out_data,
                           size_t *out_size);

int otsardb_wal_batch_decode(const void *data,
                           size_t size,
                           otsardb_wal_batch_view *out_batch);

int otsardb_wal_batch_frame(const otsardb_wal_batch_view *batch,
                          uint32_t index,
                          uint32_t *out_page_no,
                          const unsigned char **out_page);

#ifdef __cplusplus
}
#endif

#endif
