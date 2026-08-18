#ifndef OTSARDB_SQLITE_WAL_H
#define OTSARDB_SQLITE_WAL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* On-disk WAL framing constants (SQLite format). */
#define OTSARDB_SQLITE_WAL_HEADER_SIZE 32u
#define OTSARDB_SQLITE_WAL_FRAME_HEADER_SIZE 24u

/* Return non-zero after exactly size bytes were read, zero on failure. */
typedef int (*otsardb_sqlite_wal_read_at)(void *ctx,
                                        uint64_t offset,
                                        void *buffer,
                                        size_t size);

/* Reference to one validated WAL frame. The commit callback receives an array
 * of these plus a read_at so it can re-read page images from the already
 * fsynced WAL file while encoding, keeping memory bounded by chunk size
 * instead of transaction size. Frames in this array were already validated
 * (salts + cumulative checksum) during the scan. */
typedef struct otsardb_sqlite_wal_frame_ref {
    uint32_t page_no;
    uint64_t frame_offset;
} otsardb_sqlite_wal_frame_ref;

typedef int (*otsardb_sqlite_wal_commit_callback)(
    uint32_t page_size,
    uint64_t database_size_pages,
    const otsardb_sqlite_wal_frame_ref *frames,
    uint32_t frame_count,
    otsardb_sqlite_wal_read_at read_at,
    void *read_ctx,
    void *ctx);

typedef struct otsardb_sqlite_wal_header {
    uint32_t page_size;
    uint32_t salt1;
    uint32_t salt2;
    uint32_t checksum0;
    uint32_t checksum1;
    int checksum_big_endian;
} otsardb_sqlite_wal_header;

typedef struct otsardb_sqlite_wal_cursor {
    otsardb_sqlite_wal_header header;
    uint32_t frame_count;
    uint32_t checksum0;
    uint32_t checksum1;
} otsardb_sqlite_wal_cursor;

typedef struct otsardb_sqlite_wal_scan_result {
    uint32_t page_size;
    uint32_t salt1;
    uint32_t salt2;
    uint32_t valid_frames;
    uint32_t committed_frames;
    uint32_t commit_count;
    uint64_t bytes_read;
} otsardb_sqlite_wal_scan_result;

int otsardb_sqlite_wal_read_header(otsardb_sqlite_wal_read_at read_at,
                                 void *read_ctx,
                                 uint64_t wal_size,
                                 otsardb_sqlite_wal_header *out_header);

void otsardb_sqlite_wal_cursor_reset(otsardb_sqlite_wal_cursor *cursor,
                                   const otsardb_sqlite_wal_header *header);

/*
 * Incrementally scan a WAL from cursor->frame_count.
 *
 * The scan validates salts and cumulative checksums frame by frame and only
 * keeps frame references (page number + WAL offset) in memory; page images
 * are re-read by the commit callback through read_at while encoding. The
 * cursor is advanced only through successfully processed commit frames. An
 * uncommitted tail is deliberately left outside the cursor so it is reread on
 * the next sync. commit_limit == 0 means scan all complete frames available;
 * otherwise stop after that many commits. This is used to rebuild a cursor on
 * process restart from the commit index encoded in remote HEAD.
 */
int otsardb_sqlite_wal_scan_reader(otsardb_sqlite_wal_read_at read_at,
                                 void *read_ctx,
                                 uint64_t wal_size,
                                 otsardb_sqlite_wal_cursor *cursor,
                                 uint32_t commit_limit,
                                 otsardb_sqlite_wal_commit_callback callback,
                                 void *callback_ctx,
                                 otsardb_sqlite_wal_scan_result *out_result);

/* Backward-compatible full-image scanner used by tests and tooling. */
int otsardb_sqlite_wal_scan(const void *wal_data,
                          size_t wal_size,
                          otsardb_sqlite_wal_commit_callback callback,
                          void *ctx,
                          otsardb_sqlite_wal_scan_result *out_result);

#ifdef __cplusplus
}
#endif

#endif
