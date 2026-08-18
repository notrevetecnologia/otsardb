#ifndef OTSARDB_AES_GCM_PURE_H
#define OTSARDB_AES_GCM_PURE_H

#include <stddef.h>
#include <stdint.h>

#define OTSARDB_AES_GCM_PURE_KEY_BYTES 32u   /* AES-256 */
#define OTSARDB_AES_GCM_PURE_IV_BYTES 12u    /* GCM default IV (96-bit) */
#define OTSARDB_AES_GCM_PURE_TAG_BYTES 16u

/* Pure-C AES-256-GCM (NIST SP 800-38D), no OpenSSL dependency. Used for the
 * WebAssembly/emscripten build where OpenSSL EVP is unavailable. Byte-exact
 * with the OpenSSL-backed otsardb_cipher paths (verified against the NIST
 * GCM test vectors and a cross-check against EVP — project audit trail).
 *
 * Note: encryption here matches the project's stream usage (same-length
 * ciphertext, no padding) and the tag is the standard 128-bit GCM tag.
 */

/* Encrypt plaintext with a 12-byte IV and optional AAD; writes ct (same
 * length as plaintext) and a 16-byte tag. Returns 1 on success, 0 on error. */
int otsardb_aes_gcm_pure_encrypt(const uint8_t key[OTSARDB_AES_GCM_PURE_KEY_BYTES],
                                 const uint8_t iv[OTSARDB_AES_GCM_PURE_IV_BYTES],
                                 const void *aad, size_t aad_size,
                                 const void *plaintext, size_t plaintext_size,
                                 uint8_t *ct,
                                 uint8_t out_tag[OTSARDB_AES_GCM_PURE_TAG_BYTES]);

/* Decrypt ct (same length as plaintext) with a 12-byte IV, optional AAD and
 * 16-byte tag. Returns 1 on success (tag verified), 0 on failure. */
int otsardb_aes_gcm_pure_decrypt(const uint8_t key[OTSARDB_AES_GCM_PURE_KEY_BYTES],
                                 const uint8_t iv[OTSARDB_AES_GCM_PURE_IV_BYTES],
                                 const void *aad, size_t aad_size,
                                 const void *ct, size_t ct_size,
                                 const uint8_t tag[OTSARDB_AES_GCM_PURE_TAG_BYTES],
                                 uint8_t *plaintext);

#endif /* OTSARDB_AES_GCM_PURE_H */
