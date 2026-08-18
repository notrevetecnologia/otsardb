#ifndef OTSARDB_CHECKSUM_H
#define OTSARDB_CHECKSUM_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Rolling database checksum (CRC-64-ISO): XOR over CRC-64-ISO of each page
 * (page_no || page_bytes). CRC-64-ISO = reflected algorithm with nominal
 * polynomial 0xD800000000000000; effective init 0xFFFFFFFFFFFFFFFF with a
 * final inversion. Check value: crc64_iso("123456789") ==
 * 0xb90956c775a41001. */

#define OTSARDB_CHECKSUM_HEX_SIZE 16

/* XOR-accumulate one page into a rolling checksum. page_no is 1-indexed.
 * Encoding: first 8 bytes = page_no as big-endian u64, then the page. */
uint64_t otsardb_checksum_accumulate(uint64_t current,
                                   uint32_t page_no,
                                   const void *page,
                                   size_t page_size);

/* Compute the full checksum over `count` contiguous pages of page_size
 * bytes each (page_no = index + 1). */
uint64_t otsardb_checksum_compute(const void *pages,
                                size_t page_size,
                                uint32_t count);

/* Compute the full checksum over a database file (page_count pages read
 * sequentially; missing/truncated pages count as zero-filled). */
uint64_t otsardb_checksum_file(FILE *file,
                             uint32_t page_size,
                             uint64_t page_count);

/* Hex encode (16 lowercase hex chars, no NUL required by caller length). */
void otsardb_checksum_hex(uint64_t checksum, char out[OTSARDB_CHECKSUM_HEX_SIZE + 1]);

/* Parse a 16-hex string back to u64; returns 1 on success. */
int otsardb_checksum_parse(const char *hex, uint64_t *out);

#ifdef __cplusplus
}
#endif

#endif
