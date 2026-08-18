#ifndef OTSARDB_CIPHER_H
#define OTSARDB_CIPHER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* At-rest segment encryption .
 *
 * Scheme: AES-256-GCM (OpenSSL EVP) over the whole encoded .otsseg object.
 * The stored object layout is:
 *
 * [iv(12)][ciphertext][tag(16)]
 *
 * where ciphertext = AES-256-GCM(key, iv, otsseg bytes, aad). The manifest
 * records the same iv/tag (hex) in the additive top-level "segment_enc"
 * array (one entry per segment, aligned by index) and its sha256/size fields
 * cover the STORED bytes (iv || ciphertext || tag). AAD = the absolute
 * content-addressed object key string ("<root>/segments/sha256/<xx>/<sha>
 * .otsseg") + "\n" + commit_id: the key binds the object identity and the
 * commit_id binds the manifest context. Authenticated decryption (tag
 * check) makes a wrong key or a tampered object fail closed — never
 * silently garbage.
 *
 * Key-derivation order (why the two-phase encrypt API): the object key
 * embeds sha256(iv || ciphertext), and the AAD contains the key — but GCM
 * requires the AAD before the ciphertext (OpenSSL's gcm128 rejects AAD
 * after data), and the tag depends on the AAD. The ciphertext STREAM is
 * AAD-independent (GCM keystream), so the publish path first computes the
 * stream (otsardb_cipher_encrypt_stream), derives the content-addressed key,
 * then runs the full authenticated pass with the real AAD
 * (otsardb_cipher_encrypt_with_aad). Envelope size overhead: 28 bytes per
 * segment object (GCM is a stream mode: ciphertext == plaintext length).
 */

#define OTSARDB_CIPHER_ALG_NAME "AES-256-GCM"
#define OTSARDB_CIPHER_KEY_BYTES 32
#define OTSARDB_CIPHER_KEY_HEX_LEN 64
#define OTSARDB_CIPHER_IV_BYTES 12
#define OTSARDB_CIPHER_TAG_BYTES 16
#define OTSARDB_CIPHER_OVERHEAD (OTSARDB_CIPHER_IV_BYTES + OTSARDB_CIPHER_TAG_BYTES)

typedef struct otsardb_cipher otsardb_cipher;

/* Create a cipher from 32 raw key bytes. The key is copied into the object;
 * otsardb_cipher_destroy zeroises it before freeing. */
otsardb_cipher *otsardb_cipher_create(const unsigned char key[OTSARDB_CIPHER_KEY_BYTES]);

/* Create from a 64-character hex string (== 32 bytes). Returns NULL when
 * the string is not exactly 64 hex characters. */
otsardb_cipher *otsardb_cipher_create_from_hex(const char *hex);

/* Zeroise the key material and free the cipher. */
void otsardb_cipher_destroy(otsardb_cipher *cipher);

/* Returns 1 when s is exactly 64 hex characters (the OTSARDB_S3_ENC_KEY
 * contract). 0 otherwise. */
int otsardb_cipher_valid_hex_key(const char *s);

/* Phase 1 (publish path): compute the AAD-independent ciphertext stream.
 * Generates a fresh random 96-bit IV (returned in out_iv) and produces the
 * raw GCM stream of plaintext into a malloc'd *out_ct (out_ct_size ==
 * plaintext_size). The stream lets the caller derive the content-addressed
 * object key (sha256(iv || ct)) before the AAD exists. Returns 1 on
 * success. */
int otsardb_cipher_encrypt_stream(const otsardb_cipher *cipher,
                                const unsigned char *plaintext,
                                size_t plaintext_size,
                                unsigned char out_iv[OTSARDB_CIPHER_IV_BYTES],
                                unsigned char **out_ct,
                                size_t *out_ct_size);

/* Phase 2 (publish path): full authenticated AES-256-GCM with the CALLER's
 * IV (the one from otsardb_cipher_encrypt_stream) and the real AAD. *out
 * receives malloc'd [iv][ciphertext][tag] bytes (out_size = 28 +
 * plaintext_size); out_tag (may be NULL) receives the tag for the
 * manifest's "segment_enc" record. The produced ciphertext stream is
 * byte-identical to phase 1's. Returns 1 on success. */
int otsardb_cipher_encrypt_with_aad(const otsardb_cipher *cipher,
                                  const unsigned char iv[OTSARDB_CIPHER_IV_BYTES],
                                  const void *aad,
                                  size_t aad_size,
                                  const void *plaintext,
                                  size_t plaintext_size,
                                  unsigned char **out,
                                  size_t *out_size,
                                  unsigned char out_tag[OTSARDB_CIPHER_TAG_BYTES]);

/* Authenticated decrypt of [iv][ciphertext][tag]. Returns 1 on success;
 * 0 on any failure (input shorter than the envelope, tag mismatch — wrong
 * key, wrong AAD, or tampered bytes). The error reason is available from
 * otsardb_cipher_last_error(). */
int otsardb_cipher_decrypt(const otsardb_cipher *cipher,
                         const void *data,
                         size_t size,
                         const void *aad,
                         size_t aad_size,
                         unsigned char **out,
                         size_t *out_size);

/* Last cipher error message (thread-local; survives until the next cipher
 * call on the same thread). Never contains key material. */
const char *otsardb_cipher_last_error(void);

/* Thread-local session cipher registry. s3_database.cpp registers the
 * session's cipher at open and clears it at close; otsardb_publish_commit
 * falls back to it when the commit request carries no cipher (the WAL
 * publisher path builds the request without one). NULL = plaintext path
 * (byte-identical pre-0079 behavior). */
void otsardb_cipher_thread_set(const otsardb_cipher *cipher);
const otsardb_cipher *otsardb_cipher_thread_get(void);

/* ---------------------------------------------------------------------------
 * Manifest "segment_enc" metadata 
 *
 * The manifest JSON stays plaintext. Each segment gains an encryption record
 * in the additive TOP-LEVEL "segment_enc" array (one entry per segment,
 * aligned by index — the v4 segments[] entry parser is strict and stays
 * untouched, so the record cannot live inside the segment literal). An
 * entry is either {} (plaintext segment) or
 * {"alg":"AES-256-GCM","iv":"<24 hex>","tag":"<32 hex>"} (encrypted). The
 * manifest's sha256/size fields for an encrypted segment cover the STORED
 * bytes ([iv][ciphertext][tag]).
 *
 * The manifest chain (HEAD -> parent_manifest_sha256) authenticates this
 * array exactly like every other manifest field: it is part of the hashed
 * manifest object. Manifests without "segment_enc" (all pre-0079 manifests)
 * decode as entirely plaintext.
 * ------------------------------------------------------------------------- */

#define OTSARDB_SEGMENT_ENC_MAX 128u /* == OTSARDB_MANIFEST_MAX_SEGMENTS */

typedef struct otsardb_segment_enc_entry {
    int present;                             /* 1 = segment object is an AES-256-GCM envelope */
    char alg[16];
    unsigned char iv[OTSARDB_CIPHER_IV_BYTES];
    unsigned char tag[OTSARDB_CIPHER_TAG_BYTES];
} otsardb_segment_enc_entry;

typedef struct otsardb_segment_enc_set {
    otsardb_segment_enc_entry entries[OTSARDB_SEGMENT_ENC_MAX];
    uint32_t count;                          /* entries decoded (== segment_count when present) */
    int present;                             /* 1 = the manifest carried "segment_enc" */
} otsardb_segment_enc_set;

/* Parse the "segment_enc" array from a manifest JSON document. Missing
 * field -> *out zeroed with present == 0 (returns 1). A malformed array or
 * an entry count above OTSARDB_SEGMENT_ENC_MAX returns 0 (fail closed). */
int otsardb_segment_enc_parse_json(const char *json, size_t size, otsardb_segment_enc_set *out);

#ifdef __cplusplus
}
#endif

#endif
