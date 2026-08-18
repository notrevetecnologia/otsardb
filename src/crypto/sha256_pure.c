#include "sha256_pure.h"

#include <string.h>

/* Pure-C SHA-256 (FIPS 180-4). No OpenSSL dependency — the emscripten/WASM
 * build cannot use EVP. The output is bit-exact with the OpenSSL-backed
 * otsardb_sha256 (verified against the NIST FIPS 180-4 vectors and a
 * cross-check over random lengths against the EVP path — project audit trail). */

static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
    0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
    0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
    0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
    0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
    0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
    0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
    0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static uint32_t rotr(uint32_t x, unsigned n) {
    return (x >> n) | (x << (32u - n));
}

static uint32_t load_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void store_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

void otsardb_sha256_pure_init(otsardb_sha256_pure_ctx *ctx) {
    ctx->h[0] = 0x6a09e667u;
    ctx->h[1] = 0xbb67ae85u;
    ctx->h[2] = 0x3c6ef372u;
    ctx->h[3] = 0xa54ff53au;
    ctx->h[4] = 0x510e527fu;
    ctx->h[5] = 0x9b05688cu;
    ctx->h[6] = 0x1f83d9abu;
    ctx->h[7] = 0x5be0cd19u;
    ctx->total_bits = 0;
    ctx->block_used = 0;
}

static void compress(otsardb_sha256_pure_ctx *ctx, const uint8_t block[64]) {
    uint32_t w[64];
    for (unsigned i = 0; i < 16; ++i) w[i] = load_be32(block + i * 4u);
    for (unsigned i = 16; i < 64; ++i) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = ctx->h[0], b = ctx->h[1], c = ctx->h[2], d = ctx->h[3];
    uint32_t e = ctx->h[4], f = ctx->h[5], g = ctx->h[6], h = ctx->h[7];
    for (unsigned i = 0; i < 64; ++i) {
        uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + ch + K[i] + w[i];
        uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    ctx->h[0] += a; ctx->h[1] += b; ctx->h[2] += c; ctx->h[3] += d;
    ctx->h[4] += e; ctx->h[5] += f; ctx->h[6] += g; ctx->h[7] += h;
}

void otsardb_sha256_pure_update(otsardb_sha256_pure_ctx *ctx,
                                const void *data, size_t size) {
    const uint8_t *p = (const uint8_t *)data;
    ctx->total_bits += (uint64_t)size * 8u;
    while (size > 0) {
        size_t take = 64u - ctx->block_used;
        if (take > size) take = size;
        memcpy(ctx->block + ctx->block_used, p, take);
        ctx->block_used += take;
        p += take;
        size -= take;
        if (ctx->block_used == 64u) {
            compress(ctx, ctx->block);
            ctx->block_used = 0;
        }
    }
}

void otsardb_sha256_pure_final(otsardb_sha256_pure_ctx *ctx,
                               uint8_t out[OTSARDB_SHA256_PURE_SIZE]) {
    /* Padding: 0x80, zeros, then the 64-bit big-endian bit length.
     * The pad/zero bytes must NOT go through update() (which would add to
     * total_bits); write them directly into the block buffer and only
     * compress full blocks. */
    uint64_t bitlen = ctx->total_bits;

    ctx->block[ctx->block_used++] = 0x80;
    if (ctx->block_used > 56u) {
        while (ctx->block_used < 64u) ctx->block[ctx->block_used++] = 0;
        compress(ctx, ctx->block);
        ctx->block_used = 0;
    }
    while (ctx->block_used < 56u) ctx->block[ctx->block_used++] = 0;

    uint8_t lenb[8];
    for (int i = 7; i >= 0; --i) {
        lenb[i] = (uint8_t)(bitlen & 0xffu);
        bitlen >>= 8;
    }
    memcpy(ctx->block + 56, lenb, 8);
    compress(ctx, ctx->block);

    for (unsigned i = 0; i < 8; ++i) store_be32(out + i * 4u, ctx->h[i]);
}

int otsardb_sha256_pure_digest(const void *data, size_t size,
                               uint8_t out[OTSARDB_SHA256_PURE_SIZE]) {
    if ((!data && size != 0) || !out) return 0;
    otsardb_sha256_pure_ctx ctx;
    otsardb_sha256_pure_init(&ctx);
    otsardb_sha256_pure_update(&ctx, data, size);
    otsardb_sha256_pure_final(&ctx, out);
    return 1;
}
