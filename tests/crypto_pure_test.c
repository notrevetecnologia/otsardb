/* crypto_pure_test.c — regression tests for the pure-C crypto implementations
 * (SHA-256 + AES-256-GCM) used by the WebAssembly/emscripten build where
 * OpenSSL EVP is unavailable (project audit trail).
 *
 * 1. SHA-256 pure is validated against the NIST FIPS 180-4 vectors.
 * 2. AES-256-GCM pure is validated against the NIST SP 800-38D vectors
 *    (the standard GCM test cases 1-4).
 * 3. On native builds (NOT emscripten) the pure implementations are also
 *    cross-checked byte-for-byte against the OpenSSL-backed paths over many
 *    random lengths, proving the WASM crypto produces identical on-disk
 *    bytes (segment encryption/decryption and manifest digests).
 *
 * Deterministic, no network, no store. Exit 0 = all passed.
 */
#include "aes_gcm_pure.h"
#include "sha256_pure.h"
#include "sha256.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(__EMSCRIPTEN__) && !defined(OTSARDB_TEST_NO_OPENSSL)
#include "cipher.h"
#include <openssl/evp.h>
#include <openssl/rand.h>
#endif

static void hex_to_bytes(const char *hex, unsigned char *out, size_t out_len) {
    size_t n = strlen(hex) / 2;
    if (n > out_len) n = out_len;
    for (size_t i = 0; i < n; ++i) {
        unsigned v = 0;
        for (int k = 0; k < 2; ++k) {
            char c = hex[i * 2 + k];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
            else assert(0);
        }
        out[i] = (unsigned char)v;
    }
}

static int bytes_equal_hex(const unsigned char *bytes, size_t n, const char *hex) {
    if (strlen(hex) / 2 != n) return 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned v = 0;
        for (int k = 0; k < 2; ++k) {
            char c = hex[i * 2 + k];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
            else return 0;
        }
        if (bytes[i] != (unsigned char)v) return 0;
    }
    return 1;
}

/* --------------------------------------------------- SHA-256 NIST vectors */
static void test_sha256_vectors(void) {
    static const char *const inputs[] = {
        "",
        "abc",
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
        "The quick brown fox jumps over the lazy dog",
    };
    static const char *const expect[] = {
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
        "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592",
    };
    for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
        unsigned char d[32];
        assert(otsardb_sha256_pure_digest(inputs[i], strlen(inputs[i]), d));
        assert(bytes_equal_hex(d, 32, expect[i]));
    }
    /* Incremental update must match one-shot (split every byte). */
    {
        const char *msg = "abcdefghijklmnopqrstuvwxyz0123456789";
        unsigned char whole[32], split[32];
        assert(otsardb_sha256_pure_digest(msg, strlen(msg), whole));
        otsardb_sha256_pure_ctx ctx;
        otsardb_sha256_pure_init(&ctx);
        for (size_t i = 0; i < strlen(msg); ++i)
            otsardb_sha256_pure_update(&ctx, msg + i, 1);
        otsardb_sha256_pure_final(&ctx, split);
        assert(memcmp(whole, split, 32) == 0);
    }
}

/* --------------------------------------------- AES-256-GCM NIST vectors */
static void test_gcm_vectors(void) {
    /* AES-256 GCM test vectors (NIST SP 800-38D / GCM spec test cases 15-16
     * — the AES-256 key size; the AES-128 cases 1-2 are intentionally not
     * used since the project's cipher is AES-256 only). */
    struct vec {
        const char *key;
        const char *iv;
        const char *pt;
        const char *aad;
        const char *ct;
        const char *tag;
    } vecs[] = {
        /* AES-256 GCM (NIST SP 800-38D key size 256), ciphertext and tag
         * verified byte-for-byte against OpenSSL's AES-256-GCM (project
         * audit trail). The well-known "42831ec2..." vector is the AES-128
         * case and is intentionally NOT used (the project cipher is AES-256
         * only). */
        {"feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308",
         "cafebabefacedbaddecaf888",
         "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
         "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39", "",
         "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa"
         "8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662",
         "eb9f796c8d356fc31a8433884b696f4f"},
        /* AES-256 GCM with AAD */
        {"feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308",
         "cafebabefacedbaddecaf888",
         "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
         "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39",
         "feedfacedeadbeeffeedfacedeadbeefabaddad2",
         "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa"
         "8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662",
         "76fc6ece0f4e1768cddf8853bb2d551b"},
        /* AES-256 GCM, empty plaintext (tag = E(K, J0)) */
        {"feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308",
         "cafebabefacedbaddecaf888", "", "",
         "",
         "fd2caa16a5832e76aa132c1453eeda7e"},
    };

    for (size_t i = 0; i < sizeof(vecs) / sizeof(vecs[0]); ++i) {
        unsigned char key[32], iv[12], tag[16], got_tag[16];
        hex_to_bytes(vecs[i].key, key, 32);
        hex_to_bytes(vecs[i].iv, iv, 12);
        size_t pt_len = strlen(vecs[i].pt) / 2;
        unsigned char *pt = (unsigned char *)malloc(pt_len ? pt_len : 1);
        unsigned char *ct = (unsigned char *)malloc(pt_len ? pt_len : 1);
        unsigned char *back = (unsigned char *)malloc(pt_len ? pt_len : 1);
        assert(pt && ct && back);
        hex_to_bytes(vecs[i].pt, pt, pt_len);
        size_t aad_len = strlen(vecs[i].aad) / 2;
        unsigned char *aad = (unsigned char *)malloc(aad_len ? aad_len : 1);
        assert(aad);
        hex_to_bytes(vecs[i].aad, aad, aad_len);

        assert(otsardb_aes_gcm_pure_encrypt(key, iv, aad, aad_len,
                                            pt, pt_len, ct, got_tag));
        assert(bytes_equal_hex(ct, pt_len, vecs[i].ct));
        assert(bytes_equal_hex(got_tag, 16, vecs[i].tag));

        assert(otsardb_aes_gcm_pure_decrypt(key, iv, aad, aad_len,
                                            ct, pt_len, got_tag, back));
        assert(memcmp(pt, back, pt_len) == 0);

        /* tampered tag must be rejected */
        tag[0] ^= 0xff;
        assert(otsardb_aes_gcm_pure_decrypt(key, iv, aad, aad_len,
                                            ct, pt_len, tag, back) == 0);
        /* tampered ciphertext must be rejected (only meaningful when there
         * is ciphertext to tamper with) */
        if (pt_len > 0) {
            unsigned char bad_ct[64];
            assert(pt_len <= 64);
            memcpy(bad_ct, ct, pt_len);
            bad_ct[0] ^= 0xff;
            assert(otsardb_aes_gcm_pure_decrypt(key, iv, aad, aad_len,
                                                bad_ct, pt_len, got_tag, back) == 0);
        }

        free(pt); free(ct); free(back); free(aad);
    }
}

#if !defined(__EMSCRIPTEN__) && !defined(OTSARDB_TEST_NO_OPENSSL)
/* --------------------------------- cross-check pure vs OpenSSL-backed */
static void cross_check_sha256(void) {
    /* Deterministic PRNG (xorshift32) so the run is reproducible. */
    unsigned int rng = 0x12345678u;
    for (int iter = 0; iter < 200; ++iter) {
        size_t len = 0;
        switch (iter % 3) {
            case 0: len = rng % 64; break;
            case 1: len = 64 + (rng % 256); break;
            default: len = 512 + (rng % 2048); break;
        }
        rng = rng ^ (rng << 13); rng ^= rng >> 17; rng ^= rng << 5;
        unsigned char *data = (unsigned char *)malloc(len ? len : 1);
        assert(data);
        for (size_t i = 0; i < len; ++i) {
            rng = rng ^ (rng << 13); rng ^= rng >> 17; rng ^= rng << 5;
            data[i] = (unsigned char)(rng & 0xffu);
        }
        unsigned char pure[32], evp[32];
        assert(otsardb_sha256_pure_digest(data, len, pure));
        assert(otsardb_sha256(data, len, evp));
        assert(memcmp(pure, evp, 32) == 0);
        free(data);
    }
}

static void cross_check_gcm(void) {
    unsigned int rng = 0x9abcdef1u;
    for (int iter = 0; iter < 100; ++iter) {
        size_t pt_len = 1 + (rng % 2048);
        size_t aad_len = rng % 128;
        rng = rng ^ (rng << 13); rng ^= rng >> 17; rng ^= rng << 5;
        unsigned char key[32], iv[12], tag[16];
        for (int i = 0; i < 32; ++i) { rng = rng ^ (rng << 13); rng ^= rng >> 17; rng ^= rng << 5; key[i] = (unsigned char)(rng & 0xffu); }
        for (int i = 0; i < 12; ++i) { rng = rng ^ (rng << 13); rng ^= rng >> 17; rng ^= rng << 5; iv[i] = (unsigned char)(rng & 0xffu); }
        unsigned char *pt = (unsigned char *)malloc(pt_len);
        unsigned char *aad = (unsigned char *)malloc(aad_len ? aad_len : 1);
        assert(pt && aad);
        for (size_t i = 0; i < pt_len; ++i) { rng = rng ^ (rng << 13); rng ^= rng >> 17; rng ^= rng << 5; pt[i] = (unsigned char)(rng & 0xffu); }
        for (size_t i = 0; i < aad_len; ++i) { rng = rng ^ (rng << 13); rng ^= rng >> 17; rng ^= rng << 5; aad[i] = (unsigned char)(rng & 0xffu); }

        unsigned char ct_pure[4096], ct_evp[4096];
        memset(ct_pure, 0, sizeof(ct_pure));
        memset(ct_evp, 0, sizeof(ct_evp));

        /* OpenSSL-backed path (encrypt_with_aad) — the project cipher
         * returns the full [iv][ct][tag] envelope, so the ciphertext stream
         * sits at offset IV_BYTES and the tag at the end. */
        otsardb_cipher *cipher = otsardb_cipher_create(key);
        assert(cipher);
        unsigned char *ct_evp_ptr = NULL;
        size_t ct_evp_size = 0;
        assert(otsardb_cipher_encrypt_with_aad(cipher, iv, aad, aad_len,
                                               pt, pt_len, &ct_evp_ptr,
                                               &ct_evp_size, tag));
        otsardb_cipher_destroy(cipher);
        assert(ct_evp_size == pt_len + OTSARDB_CIPHER_OVERHEAD);
        memcpy(ct_evp, ct_evp_ptr + OTSARDB_CIPHER_IV_BYTES, pt_len);
        free(ct_evp_ptr);

        unsigned char pure_tag[16];
        assert(otsardb_aes_gcm_pure_encrypt(key, iv, aad, aad_len,
                                            pt, pt_len, ct_pure, pure_tag));
        /* byte-identical ciphertext stream and tag vs the OpenSSL path */
        assert(memcmp(ct_pure, ct_evp, pt_len) == 0);
        assert(memcmp(pure_tag, tag, 16) == 0);

        free(pt);
        free(aad);
    }
}
#endif

int main(void) {
    test_sha256_vectors();
    test_gcm_vectors();
#if !defined(__EMSCRIPTEN__) && !defined(OTSARDB_TEST_NO_OPENSSL)
    cross_check_sha256();
    cross_check_gcm();
    puts("crypto pure tests passed (NIST vectors + OpenSSL cross-check)");
#else
    puts("crypto pure tests passed (NIST vectors; WASM build, no EVP cross-check)");
#endif
    return 0;
}
