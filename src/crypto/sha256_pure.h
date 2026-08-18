#ifndef OTSARDB_SHA256_PURE_H
#define OTSARDB_SHA256_PURE_H

#include <stddef.h>
#include <stdint.h>

#define OTSARDB_SHA256_PURE_SIZE 32u

/* Pure-C SHA-256 (FIPS 180-4), no OpenSSL dependency. Used for the
 * WebAssembly/emscripten build where OpenSSL EVP is unavailable. Byte- and
 * bit-exact with the EVP output (verified against NIST vectors and a
 * cross-check against the OpenSSL-backed path — project audit trail). */

typedef struct otsardb_sha256_pure_ctx {
    uint32_t h[8];
    uint64_t total_bits;
    uint8_t block[64];
    size_t block_used;
} otsardb_sha256_pure_ctx;

void otsardb_sha256_pure_init(otsardb_sha256_pure_ctx *ctx);
void otsardb_sha256_pure_update(otsardb_sha256_pure_ctx *ctx,
                                const void *data, size_t size);
void otsardb_sha256_pure_final(otsardb_sha256_pure_ctx *ctx,
                               uint8_t out[OTSARDB_SHA256_PURE_SIZE]);

/* One-shot convenience: digests [data, data+size) into out[0..31]. */
int otsardb_sha256_pure_digest(const void *data, size_t size,
                               uint8_t out[OTSARDB_SHA256_PURE_SIZE]);

#endif /* OTSARDB_SHA256_PURE_H */
