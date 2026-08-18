#include "cipher.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__EMSCRIPTEN__)
#include "aes_gcm_pure.h"
#include <unistd.h>   /* getentropy() -> browser crypto.getRandomValues */
#else
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#endif

struct otsardb_cipher {
    unsigned char key[OTSARDB_CIPHER_KEY_BYTES];
};

#if defined(_MSC_VER)
static __declspec(thread) const char *g_cipher_error;
static __declspec(thread) const otsardb_cipher *g_thread_cipher;
#elif defined(__GNUC__) || defined(__clang__)
static _Thread_local const char *g_cipher_error;
static _Thread_local const otsardb_cipher *g_thread_cipher;
#else
static const char *g_cipher_error;
static const otsardb_cipher *g_thread_cipher;
#endif

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void set_error(const char *message) {
    g_cipher_error = message;
}

/* Best-effort key material wipe. On OpenSSL builds OPENSSL_cleanse is the
 * compiler-optimization-resistant zeroise; on WASM plain memset (the
 * optimization-resistant guarantee is not available there, but the WASM
 * sandbox does not expose the key to other native processes either). */
#if defined(__EMSCRIPTEN__)
static void wipe(void *p, size_t n) { memset(p, 0, n); }
#else
static void wipe(void *p, size_t n) { OPENSSL_cleanse(p, n); }
#endif

const char *otsardb_cipher_last_error(void) {
    return g_cipher_error ? g_cipher_error : "unknown cipher error";
}

int otsardb_cipher_valid_hex_key(const char *s) {
    if (!s) return 0;
    size_t len = strlen(s);
    if (len != OTSARDB_CIPHER_KEY_HEX_LEN) return 0;
    for (size_t i = 0; i < len; ++i) {
        if (hex_nibble(s[i]) < 0) return 0;
    }
    return 1;
}

otsardb_cipher *otsardb_cipher_create(const unsigned char key[OTSARDB_CIPHER_KEY_BYTES]) {
    if (!key) {
        set_error("null key material");
        return NULL;
    }
    otsardb_cipher *cipher = (otsardb_cipher *)malloc(sizeof(*cipher));
    if (!cipher) {
        set_error("out of memory");
        return NULL;
    }
    memcpy(cipher->key, key, OTSARDB_CIPHER_KEY_BYTES);
    return cipher;
}

otsardb_cipher *otsardb_cipher_create_from_hex(const char *hex) {
    if (!otsardb_cipher_valid_hex_key(hex)) {
        set_error("key must be exactly 64 hex characters (32 bytes)");
        return NULL;
    }
    unsigned char key[OTSARDB_CIPHER_KEY_BYTES];
    for (size_t i = 0; i < OTSARDB_CIPHER_KEY_BYTES; ++i) {
        key[i] = (unsigned char)((hex_nibble(hex[i * 2]) << 4) |
                                 hex_nibble(hex[i * 2 + 1]));
    }
    otsardb_cipher *cipher = otsardb_cipher_create(key);
    wipe(key, sizeof(key));
    return cipher;
}

void otsardb_cipher_destroy(otsardb_cipher *cipher) {
    if (!cipher) return;
    wipe(cipher->key, sizeof(cipher->key));
    free(cipher);
}

void otsardb_cipher_thread_set(const otsardb_cipher *cipher) {
    g_thread_cipher = cipher;
}

const otsardb_cipher *otsardb_cipher_thread_get(void) {
    return g_thread_cipher;
}

int otsardb_cipher_encrypt_stream(const otsardb_cipher *cipher,
                                const unsigned char *plaintext,
                                size_t plaintext_size,
                                unsigned char out_iv[OTSARDB_CIPHER_IV_BYTES],
                                unsigned char **out_ct,
                                size_t *out_ct_size) {
    if (!cipher || !plaintext || !out_iv || !out_ct || !out_ct_size) {
        set_error("invalid encrypt arguments");
        return 0;
    }
    if (plaintext_size > (size_t)INT_MAX) {
        set_error("input too large for EVP (int range)");
        return 0;
    }
#if defined(__EMSCRIPTEN__)
    if (getentropy(out_iv, OTSARDB_CIPHER_IV_BYTES) != 0) {
        set_error("getentropy failed (no entropy)");
        return 0;
    }
#else
    if (RAND_bytes(out_iv, OTSARDB_CIPHER_IV_BYTES) != 1) {
        set_error("RAND_bytes failed (no entropy)");
        return 0;
    }
#endif

    unsigned char *ct = (unsigned char *)malloc(plaintext_size ? plaintext_size : 1u);
    if (!ct) {
        set_error("out of memory");
        return 0;
    }

#if defined(__EMSCRIPTEN__)
    unsigned char tag[OTSARDB_CIPHER_TAG_BYTES];
    if (!otsardb_aes_gcm_pure_encrypt(cipher->key, out_iv, NULL, 0,
                                      plaintext, plaintext_size, ct, tag)) {
        free(ct);
        set_error("AES-256-GCM stream encrypt failed");
        return 0;
    }
#else
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        free(ct);
        set_error("failed to allocate EVP cipher context");
        return 0;
    }

    int out_len = 0;
    int ok = EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1 &&
             EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, OTSARDB_CIPHER_IV_BYTES, NULL) == 1 &&
             EVP_EncryptInit_ex(ctx, NULL, NULL, cipher->key, out_iv) == 1 &&
             EVP_EncryptUpdate(ctx, ct, &out_len,
                               (const unsigned char *)plaintext, (int)plaintext_size) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok || (size_t)out_len != plaintext_size) {
        wipe(ct, plaintext_size ? plaintext_size : 1u);
        free(ct);
        set_error("EVP AES-256-GCM stream encrypt failed");
        return 0;
    }
#endif

    *out_ct = ct;
    *out_ct_size = plaintext_size;
    return 1;
}

int otsardb_cipher_encrypt_with_aad(const otsardb_cipher *cipher,
                                  const unsigned char iv[OTSARDB_CIPHER_IV_BYTES],
                                  const void *aad,
                                  size_t aad_size,
                                  const void *plaintext,
                                  size_t plaintext_size,
                                  unsigned char **out,
                                  size_t *out_size,
                                  unsigned char out_tag[OTSARDB_CIPHER_TAG_BYTES]) {
    if (!cipher || !iv || !plaintext || !out || !out_size) {
        set_error("invalid encrypt arguments");
        return 0;
    }
    if (plaintext_size > (size_t)INT_MAX || aad_size > (size_t)INT_MAX ||
        plaintext_size > (size_t)SIZE_MAX - OTSARDB_CIPHER_OVERHEAD) {
        set_error("input too large for EVP (int range)");
        return 0;
    }

    size_t total = plaintext_size + OTSARDB_CIPHER_OVERHEAD;
    unsigned char *encoded = (unsigned char *)malloc(total ? total : 1u);
    if (!encoded) {
        set_error("out of memory");
        return 0;
    }

#if defined(__EMSCRIPTEN__)
    unsigned char *ciphertext = encoded + OTSARDB_CIPHER_IV_BYTES;
    unsigned char *tag_slot = ciphertext + plaintext_size;
    int ok = otsardb_aes_gcm_pure_encrypt(cipher->key, iv, aad, aad_size,
                                          plaintext, plaintext_size,
                                          ciphertext, tag_slot);
    if (ok) {
        memcpy(encoded, iv, OTSARDB_CIPHER_IV_BYTES);
        if (out_tag) memcpy(out_tag, tag_slot, OTSARDB_CIPHER_TAG_BYTES);
        *out = encoded;
        *out_size = total;
    } else {
        memset(encoded, 0, total);
        free(encoded);
        *out = NULL;
        *out_size = 0;
        set_error("AES-256-GCM encrypt failed");
    }
    return ok;
#else
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        free(encoded);
        set_error("failed to allocate EVP cipher context");
        return 0;
    }

    int ok = 0;
    int out_len = 0;
    int final_len = 0;
    unsigned char *ciphertext = encoded + OTSARDB_CIPHER_IV_BYTES;
    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, OTSARDB_CIPHER_IV_BYTES, NULL) != 1 ||
        EVP_EncryptInit_ex(ctx, NULL, NULL, cipher->key, iv) != 1 ||
        (aad_size > 0 &&
         EVP_EncryptUpdate(ctx, NULL, &out_len, (const unsigned char *)aad, (int)aad_size) != 1) ||
        EVP_EncryptUpdate(ctx, ciphertext, &out_len,
                          (const unsigned char *)plaintext, (int)plaintext_size) != 1) {
        set_error("EVP AES-256-GCM encrypt failed");
        goto done;
    }
    if (EVP_EncryptFinal_ex(ctx, ciphertext + out_len, &final_len) != 1) {
        set_error("EVP AES-256-GCM encrypt final failed");
        goto done;
    }
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, OTSARDB_CIPHER_TAG_BYTES,
                            encoded + OTSARDB_CIPHER_IV_BYTES + plaintext_size) != 1) {
        set_error("EVP AES-256-GCM tag get failed");
        goto done;
    }

    memcpy(encoded, iv, OTSARDB_CIPHER_IV_BYTES);
    if (out_tag) memcpy(out_tag, encoded + OTSARDB_CIPHER_IV_BYTES + plaintext_size,
                        OTSARDB_CIPHER_TAG_BYTES);
    *out = encoded;
    *out_size = total;
    ok = 1;

done:
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) {
        wipe(encoded, total);
        free(encoded);
        *out = NULL;
        *out_size = 0;
    }
    return ok;
#endif
}

int otsardb_cipher_decrypt(const otsardb_cipher *cipher,
                         const void *data,
                         size_t size,
                         const void *aad,
                         size_t aad_size,
                         unsigned char **out,
                         size_t *out_size) {
    if (!cipher || !data || !out || !out_size) {
        set_error("invalid decrypt arguments");
        return 0;
    }
    if (size < OTSARDB_CIPHER_OVERHEAD) {
        set_error("encrypted object shorter than the iv+tag envelope (28 bytes)");
        return 0;
    }
    if (size - OTSARDB_CIPHER_OVERHEAD > (size_t)INT_MAX ||
        aad_size > (size_t)INT_MAX) {
        set_error("input too large for EVP (int range)");
        return 0;
    }

    const unsigned char *iv = (const unsigned char *)data;
    const unsigned char *ciphertext = iv + OTSARDB_CIPHER_IV_BYTES;
    size_t ciphertext_size = size - OTSARDB_CIPHER_OVERHEAD;
    const unsigned char *tag = ciphertext + ciphertext_size;

    size_t plaintext_size = ciphertext_size;
    unsigned char *plaintext = (unsigned char *)malloc(plaintext_size ? plaintext_size : 1u);
    if (!plaintext) {
        set_error("out of memory");
        return 0;
    }

#if defined(__EMSCRIPTEN__)
    if (!otsardb_aes_gcm_pure_decrypt(cipher->key, iv, aad, aad_size,
                                      ciphertext, ciphertext_size, tag,
                                      plaintext)) {
        memset(plaintext, 0, plaintext_size ? plaintext_size : 1u);
        free(plaintext);
        *out = NULL;
        *out_size = 0;
        set_error("AES-256-GCM tag verification failed (wrong key, wrong AAD, or tampered bytes)");
        return 0;
    }
    *out = plaintext;
    *out_size = plaintext_size;
    return 1;
#else
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        free(plaintext);
        set_error("failed to allocate EVP cipher context");
        return 0;
    }

    int ok = 0;
    int out_len = 0;
    int final_len = 0;
    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, OTSARDB_CIPHER_IV_BYTES, NULL) != 1 ||
        EVP_DecryptInit_ex(ctx, NULL, NULL, cipher->key, iv) != 1 ||
        (aad_size > 0 &&
         EVP_DecryptUpdate(ctx, NULL, &out_len, (const unsigned char *)aad, (int)aad_size) != 1) ||
        EVP_DecryptUpdate(ctx, plaintext, &out_len, ciphertext, (int)ciphertext_size) != 1) {
        set_error("EVP AES-256-GCM decrypt failed");
        goto done;
    }
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, OTSARDB_CIPHER_TAG_BYTES,
                            (void *)tag) != 1) {
        set_error("EVP AES-256-GCM tag set failed");
        goto done;
    }
    if (EVP_DecryptFinal_ex(ctx, plaintext + out_len, &final_len) != 1) {
        set_error("AES-256-GCM tag verification failed (wrong key, wrong AAD, or tampered bytes)");
        goto done;
    }

    *out = plaintext;
    *out_size = plaintext_size;
    ok = 1;

done:
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) {
        wipe(plaintext, plaintext_size ? plaintext_size : 1u);
        free(plaintext);
        *out = NULL;
        *out_size = 0;
    }
    return ok;
#endif
}

/* ---------------------------------------------------------------------------
 * Manifest "segment_enc" array parsing 
 *
 * The encoder (commit_publish.c) injects
 * ,"segment_enc":[{"alg":"AES-256-GCM","iv":"..","tag":".."} | {}, ...]
 * before the manifest's closing brace. The parser below is intentionally
 * simple (the same strstr-based style as commit_manifest.c): it looks for
 * the exact `"segment_enc":[` marker and parses `{...}` entries separated
 * by commas until the closing `]`. A plaintext entry is `{}`; an encrypted
 * entry carries alg/iv/tag. Any malformed content fails closed (return 0).
 * ------------------------------------------------------------------------- */

static int enc_parse_hex(const char *text, size_t len, unsigned char *out, size_t out_len) {
    if (len != out_len * 2u) return 0;
    for (size_t i = 0; i < out_len; ++i) {
        int hi = hex_nibble(text[i * 2]);
        int lo = hex_nibble(text[i * 2 + 1]);
        if (hi < 0 || lo < 0) return 0;
        out[i] = (unsigned char)((hi << 4) | lo);
    }
    return 1;
}

static int enc_parse_entry(const char **pp, otsardb_segment_enc_entry *out) {
    const char *p = *pp;
    memset(out, 0, sizeof(*out));
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    if (*p != '{') return 0;
    ++p;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    if (*p == '}') {
        /* Plaintext entry: {} */
        ++p;
        *pp = p;
        return 1;
    }

    static const char ALG_MARKER[] = "\"alg\":\"";
    static const char IV_MARKER[] = ",\"iv\":\"";
    static const char TAG_MARKER[] = ",\"tag\":\"";
    if (strncmp(p, ALG_MARKER, sizeof(ALG_MARKER) - 1) != 0) return 0;
    p += sizeof(ALG_MARKER) - 1;
    const char *alg_end = strchr(p, '"');
    if (!alg_end) return 0;
    size_t alg_len = (size_t)(alg_end - p);
    if (alg_len != sizeof("AES-256-GCM") - 1u ||
        strncmp(p, "AES-256-GCM", sizeof("AES-256-GCM") - 1u) != 0) {
        return 0;
    }
    memcpy(out->alg, p, alg_len);
    out->alg[alg_len] = '\0';
    p = alg_end + 1;

    if (strncmp(p, IV_MARKER, sizeof(IV_MARKER) - 1) != 0) return 0;
    p += sizeof(IV_MARKER) - 1;
    const char *iv_end = strchr(p, '"');
    if (!iv_end) return 0;
    if (!enc_parse_hex(p, (size_t)(iv_end - p), out->iv, OTSARDB_CIPHER_IV_BYTES)) return 0;
    p = iv_end + 1;

    if (strncmp(p, TAG_MARKER, sizeof(TAG_MARKER) - 1) != 0) return 0;
    p += sizeof(TAG_MARKER) - 1;
    const char *tag_end = strchr(p, '"');
    if (!tag_end) return 0;
    if (!enc_parse_hex(p, (size_t)(tag_end - p), out->tag, OTSARDB_CIPHER_TAG_BYTES)) return 0;
    p = tag_end + 1;

    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    if (*p != '}') return 0;
    ++p;
    out->present = 1;
    *pp = p;
    return 1;
}

int otsardb_segment_enc_parse_json(const char *json, size_t size, otsardb_segment_enc_set *out) {
    memset(out, 0, sizeof(*out));
    if (!json || size == 0) return 0;

    char *copy = (char *)malloc(size + 1u);
    if (!copy) return 0;
    memcpy(copy, json, size);
    copy[size] = '\0';

    static const char ARRAY_MARKER[] = "\"segment_enc\":[";
    const char *p = strstr(copy, ARRAY_MARKER);
    if (!p) {
        free(copy);
        return 1; /* absent = legacy plaintext manifest */
    }
    p += sizeof(ARRAY_MARKER) - 1;

    uint32_t count = 0;
    int ok = 1;
    while (count < OTSARDB_SEGMENT_ENC_MAX) {
        if (!enc_parse_entry(&p, &out->entries[count])) {
            ok = 0;
            break;
        }
        ++count;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
        if (*p == ']') break;
        if (*p != ',') {
            ok = 0;
            break;
        }
        ++p;
    }
    if (ok && (count == 0 || *p != ']')) ok = 0;
    free(copy);
    if (!ok) {
        memset(out, 0, sizeof(*out));
        set_error("malformed manifest segment_enc array (fail closed)");
        return 0;
    }
    out->count = count;
    out->present = 1;
    return 1;
}
