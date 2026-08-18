#ifndef OTSARDB_SHA256_H
#define OTSARDB_SHA256_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OTSARDB_SHA256_SIZE 32
#define OTSARDB_SHA256_HEX_SIZE 64

int otsardb_sha256(const void *data, size_t size, unsigned char out[OTSARDB_SHA256_SIZE]);
void otsardb_sha256_hex(const unsigned char digest[OTSARDB_SHA256_SIZE], char out[OTSARDB_SHA256_HEX_SIZE + 1]);
int otsardb_sha256_hex_data(const void *data, size_t size, char out[OTSARDB_SHA256_HEX_SIZE + 1]);

#ifdef __cplusplus
}
#endif

#endif
