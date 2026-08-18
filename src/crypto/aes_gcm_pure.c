#include "aes_gcm_pure.h"

#include <string.h>

/* Pure-C AES-256-GCM (FIPS 197 + NIST SP 800-38D). No OpenSSL dependency —
 * used by the emscripten/WASM build. The output is byte-exact with the
 * OpenSSL-backed otsardb_cipher paths (validated against NIST vectors and a
 * native cross-check — project audit trail). */

/* ------------------------------------------------------------ AES-256 --- */

static const uint8_t SBOX[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

static uint8_t xtime(uint8_t x) {
    return (uint8_t)((x << 1) ^ (((x >> 7) & 1u) * 0x1bu));
}

/* AES-256 key schedule -> 60 words (240 bytes) of round keys. */
static void aes256_expand_key(const uint8_t key[32], uint8_t rk[240]) {
    memcpy(rk, key, 32);   /* w[0..7] */
    uint8_t rcon = 1;
    for (unsigned i = 8; i < 60; ++i) {
        uint8_t tmp[4];
        memcpy(tmp, rk + (i - 1) * 4u, 4);
        if (i % 8 == 0) {
            /* RotWord + SubWord + Rcon */
            uint8_t r = tmp[0];
            tmp[0] = (uint8_t)(SBOX[tmp[1]] ^ rcon);
            tmp[1] = SBOX[tmp[2]];
            tmp[2] = SBOX[tmp[3]];
            tmp[3] = SBOX[r];
            rcon = xtime(rcon);
        } else if (i % 8 == 4) {
            /* SubWord only */
            for (int k = 0; k < 4; ++k) tmp[k] = SBOX[tmp[k]];
        }
        for (int k = 0; k < 4; ++k) rk[i * 4u + (uint32_t)k] =
            (uint8_t)(rk[(i - 8) * 4u + (uint32_t)k] ^ tmp[k]);
    }
}

static void aes256_encrypt_block(const uint8_t rk[240], const uint8_t in[16],
                                 uint8_t out[16]) {
    uint8_t s[16];
    memcpy(s, in, 16);
    /* AddRoundKey 0 */
    for (unsigned i = 0; i < 16; ++i) s[i] ^= rk[i];

    for (unsigned round = 1; round <= 13; ++round) {
        /* SubBytes + ShiftRows */
        uint8_t t[16];
        for (unsigned c = 0; c < 4; ++c) {
            t[c * 4u + 0] = SBOX[s[c * 4u + 0]];
            t[c * 4u + 1] = SBOX[s[(c * 4u + 5) % 16u]];
            t[c * 4u + 2] = SBOX[s[(c * 4u + 10) % 16u]];
            t[c * 4u + 3] = SBOX[s[(c * 4u + 15) % 16u]];
        }
        /* MixColumns */
        for (unsigned c = 0; c < 4; ++c) {
            uint8_t a0 = t[c * 4u], a1 = t[c * 4u + 1], a2 = t[c * 4u + 2], a3 = t[c * 4u + 3];
            s[c * 4u + 0] = (uint8_t)(xtime(a0) ^ (xtime(a1) ^ a1) ^ a2 ^ a3);
            s[c * 4u + 1] = (uint8_t)(a0 ^ xtime(a1) ^ (xtime(a2) ^ a2) ^ a3);
            s[c * 4u + 2] = (uint8_t)(a0 ^ a1 ^ xtime(a2) ^ (xtime(a3) ^ a3));
            s[c * 4u + 3] = (uint8_t)((xtime(a0) ^ a0) ^ a1 ^ a2 ^ xtime(a3));
        }
        /* AddRoundKey */
        const uint8_t *rkp = rk + round * 16u;
        for (unsigned i = 0; i < 16; ++i) s[i] ^= rkp[i];
    }

    /* Final round: SubBytes + ShiftRows + AddRoundKey(14) */
    uint8_t t[16];
    for (unsigned c = 0; c < 4; ++c) {
        t[c * 4u + 0] = SBOX[s[c * 4u + 0]];
        t[c * 4u + 1] = SBOX[s[(c * 4u + 5) % 16u]];
        t[c * 4u + 2] = SBOX[s[(c * 4u + 10) % 16u]];
        t[c * 4u + 3] = SBOX[s[(c * 4u + 15) % 16u]];
    }
    const uint8_t *rkp = rk + 14 * 16u;
    for (unsigned i = 0; i < 16; ++i) out[i] = (uint8_t)(t[i] ^ rkp[i]);
}

/* -------------------------------------------------------------- GHASH --- */

static uint64_t load_be64(const uint8_t *p) {
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}

static void store_be64(uint8_t *p, uint64_t v) {
    for (int i = 7; i >= 0; --i) { p[i] = (uint8_t)(v & 0xffu); v >>= 8; }
}

typedef struct {
    uint64_t hi, lo;   /* 128-bit big-endian value */
} u128;

static void gf_mult(const u128 *x, const u128 *y, u128 *out) {
    /* Standard GCM GHASH multiplication over GF(2^128) with R = 0xe1... */
    uint64_t hi = x->hi, lo = x->lo;
    uint64_t zhi = 0, zlo = 0;
    uint64_t yhi = y->hi, ylo = y->lo;
    for (int i = 127; i >= 0; --i) {
        /* if (y & (1<<i)) Z ^= X */
        uint64_t bit = (i >= 64) ? ((yhi >> (uint32_t)(i - 64)) & 1u)
                                 : ((ylo >> (uint32_t)i) & 1u);
        if (bit) { zhi ^= hi; zlo ^= lo; }
        /* X = X >> 1 (with reduction) */
        uint64_t lsb = lo & 1u;
        lo = (lo >> 1) | (hi << 63);
        hi >>= 1;
        if (lsb) {
            /* X ^= R where R = 0xE1000000000000000000000000000000 (big-endian) */
            hi ^= 0xe100000000000000ull;
        }
    }
    out->hi = zhi;
    out->lo = zlo;
}

static void ghash(const uint8_t *h, const uint8_t *aad, size_t aad_size,
                  const uint8_t *ct, size_t ct_size,
                  uint8_t out_tag[16]) {
    u128 y = {0, 0};
    u128 h128;
    h128.hi = load_be64(h);
    h128.lo = load_be64(h + 8);

    /* AAD blocks, zero-padded to a 16-byte boundary */
    size_t aoff = 0;
    while (aoff < aad_size) {
        uint8_t block[16];
        memset(block, 0, 16);
        size_t take = aad_size - aoff;
        if (take > 16) take = 16;
        memcpy(block, aad + aoff, take);
        u128 b; b.hi = load_be64(block); b.lo = load_be64(block + 8);
        y.hi ^= b.hi; y.lo ^= b.lo;
        gf_mult(&y, &h128, &y);
        aoff += take;
    }

    /* Ciphertext blocks, zero-padded */
    size_t coff = 0;
    while (coff < ct_size) {
        uint8_t block[16];
        memset(block, 0, 16);
        size_t take = ct_size - coff;
        if (take > 16) take = 16;
        memcpy(block, ct + coff, take);
        u128 b; b.hi = load_be64(block); b.lo = load_be64(block + 8);
        y.hi ^= b.hi; y.lo ^= b.lo;
        gf_mult(&y, &h128, &y);
        coff += take;
    }

    /* Length block: 64-bit bit-lengths of AAD and ciphertext */
    uint8_t lenb[16];
    store_be64(lenb, (uint64_t)aad_size * 8u);
    store_be64(lenb + 8, (uint64_t)ct_size * 8u);
    u128 l; l.hi = load_be64(lenb); l.lo = load_be64(lenb + 8);
    y.hi ^= l.hi; y.lo ^= l.lo;
    gf_mult(&y, &h128, &y);
    store_be64(out_tag, y.hi);
    store_be64(out_tag + 8, y.lo);
}

static void ctrok(uint8_t *out, const uint8_t *iv, uint32_t counter) {
    /* J0: IV || 0x00000001 */
    memset(out, 0, 16);
    memcpy(out, iv, 12);
    out[12] = (uint8_t)(counter >> 24);
    out[13] = (uint8_t)(counter >> 16);
    out[14] = (uint8_t)(counter >> 8);
    out[15] = (uint8_t)counter;
}

/* ------------------------------------------------------------------- GCM */

int otsardb_aes_gcm_pure_encrypt(const uint8_t key[OTSARDB_AES_GCM_PURE_KEY_BYTES],
                                 const uint8_t iv[OTSARDB_AES_GCM_PURE_IV_BYTES],
                                 const void *aad, size_t aad_size,
                                 const void *plaintext, size_t plaintext_size,
                                 uint8_t *ct,
                                 uint8_t out_tag[OTSARDB_AES_GCM_PURE_TAG_BYTES]) {
    if (!key || !iv || !ct || !out_tag) return 0;
    if (plaintext_size && !plaintext) return 0;

    uint8_t rk[240];
    aes256_expand_key(key, rk);

    /* H = AES_K(0^128) */
    uint8_t h[16];
    uint8_t zero[16];
    memset(zero, 0, 16);
    aes256_encrypt_block(rk, zero, h);

    /* CTR keystream: J0 = IV||1, then increment per block */
    const uint8_t *pt = (const uint8_t *)plaintext;
    size_t off = 0;
    uint32_t counter = 2;
    uint8_t j0[16];
    ctrok(j0, iv, 1);
    while (off < plaintext_size) {
        uint8_t ks[16];
        uint8_t counter_block[16];
        ctrok(counter_block, iv, counter);
        aes256_encrypt_block(rk, counter_block, ks);
        size_t take = plaintext_size - off;
        if (take > 16) take = 16;
        for (size_t i = 0; i < take; ++i) ct[off + i] = (uint8_t)(pt[off + i] ^ ks[i]);
        off += take;
        ++counter;
    }

    ghash(h, (const uint8_t *)aad, aad_size, ct, plaintext_size, out_tag);
    /* tag = GCTR_K(J0) XOR GHASH result */
    uint8_t ej0[16];
    aes256_encrypt_block(rk, j0, ej0);
    for (unsigned i = 0; i < 16; ++i) out_tag[i] ^= ej0[i];

    memset(rk, 0, sizeof(rk));
    memset(h, 0, sizeof(h));
    memset(zero, 0, sizeof(zero));
    memset(j0, 0, sizeof(j0));
    return 1;
}

int otsardb_aes_gcm_pure_decrypt(const uint8_t key[OTSARDB_AES_GCM_PURE_KEY_BYTES],
                                 const uint8_t iv[OTSARDB_AES_GCM_PURE_IV_BYTES],
                                 const void *aad, size_t aad_size,
                                 const void *ct, size_t ct_size,
                                 const uint8_t tag[OTSARDB_AES_GCM_PURE_TAG_BYTES],
                                 uint8_t *plaintext) {
    if (!key || !iv || !ct || !tag || !plaintext) return 0;

    uint8_t rk[240];
    aes256_expand_key(key, rk);

    uint8_t h[16];
    uint8_t zero[16];
    memset(zero, 0, 16);
    aes256_encrypt_block(rk, zero, h);

    /* Compute expected tag first */
    uint8_t expect_tag[16];
    ghash(h, (const uint8_t *)aad, aad_size, (const uint8_t *)ct, ct_size, expect_tag);
    uint8_t j0[16];
    ctrok(j0, iv, 1);
    uint8_t ej0[16];
    aes256_encrypt_block(rk, j0, ej0);
    for (unsigned i = 0; i < 16; ++i) expect_tag[i] ^= ej0[i];

    int tag_ok = 1;
    for (unsigned i = 0; i < 16; ++i) {
        if (expect_tag[i] != tag[i]) tag_ok = 0;
    }

    if (!tag_ok) {
        memset(rk, 0, sizeof(rk));
        memset(h, 0, sizeof(h));
        memset(expect_tag, 0, sizeof(expect_tag));
        memset(j0, 0, sizeof(j0));
        return 0;
    }

    const uint8_t *c = (const uint8_t *)ct;
    size_t off = 0;
    uint32_t counter = 2;
    while (off < ct_size) {
        uint8_t ks[16];
        uint8_t counter_block[16];
        ctrok(counter_block, iv, counter);
        aes256_encrypt_block(rk, counter_block, ks);
        size_t take = ct_size - off;
        if (take > 16) take = 16;
        for (size_t i = 0; i < take; ++i) plaintext[off + i] = (uint8_t)(c[off + i] ^ ks[i]);
        off += take;
        ++counter;
    }

    memset(rk, 0, sizeof(rk));
    memset(h, 0, sizeof(h));
    memset(expect_tag, 0, sizeof(expect_tag));
    memset(j0, 0, sizeof(j0));
    return 1;
}
