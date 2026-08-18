#include "commit_publish.h"
#include "checkpoint_manifest.h"
#include "cipher.h"
#include "recovery.h"
#include "segment.h"
#include "sha256.h"

#if !defined(__EMSCRIPTEN__)
#include <openssl/crypto.h>
#endif

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__EMSCRIPTEN__)
static void wipe(void *p, size_t n) { memset(p, 0, n); }
#else
static void wipe(void *p, size_t n) { OPENSSL_cleanse(p, n); }
#endif

static int root_join(const char *root, const char *suffix, char *out, size_t out_size) {
    if (!root || !suffix || !out || out_size == 0) return 0;
    size_t len = strlen(root);
    int needs_slash = len > 0 && root[len - 1] != '/';
    int n = snprintf(out, out_size, "%s%s%s", root, needs_slash ? "/" : "", suffix);
    return n > 0 && (size_t)n < out_size;
}

static otsardb_journal_result existing_object_matches(otsardb_object_store *store,
                                                     const char *key,
                                                     const void *data,
                                                     size_t size) {
    otsardb_store_object existing = {0};
    otsardb_store_result rc = otsardb_store_get(store, key, &existing);
    if (rc == OTSARDB_STORE_NOT_FOUND) return OTSARDB_JOURNAL_NOT_FOUND;
    if (rc != OTSARDB_STORE_OK) return OTSARDB_JOURNAL_ERROR;
    int identical = existing.size == size && (size == 0 || memcmp(existing.data, data, size) == 0);
    otsardb_store_release_object(store, &existing);
    return identical ? OTSARDB_JOURNAL_OK : OTSARDB_JOURNAL_CONFLICT;
}

static otsardb_journal_result immutable_put(otsardb_object_store *store,
                                          const char *key,
                                          const void *data,
                                          size_t size) {
    if (otsardb_store_supports(store, OTSARDB_STORE_CAP_CONDITIONAL_CREATE)) {
        otsardb_store_put_options options = {0};
        options.if_none_match = 1;
        otsardb_store_result rc = otsardb_store_put(store, key, data, size, &options, NULL);
        if (rc == OTSARDB_STORE_OK) return OTSARDB_JOURNAL_OK;
        if (rc == OTSARDB_STORE_PRECONDITION_FAILED) {
            return existing_object_matches(store, key, data, size);
        }
        if (rc == OTSARDB_STORE_TIMEOUT) {
            otsardb_journal_result resolved = existing_object_matches(store, key, data, size);
            return resolved == OTSARDB_JOURNAL_NOT_FOUND ? OTSARDB_JOURNAL_AMBIGUOUS : resolved;
        }
        if (rc == OTSARDB_STORE_UNSUPPORTED) return OTSARDB_JOURNAL_UNSUPPORTED;
        return OTSARDB_JOURNAL_ERROR;
    }

    otsardb_journal_result existing = existing_object_matches(store, key, data, size);
    if (existing == OTSARDB_JOURNAL_OK || existing == OTSARDB_JOURNAL_CONFLICT) return existing;
    if (existing != OTSARDB_JOURNAL_NOT_FOUND) return existing;

    otsardb_store_put_options options = {0};
    otsardb_store_result rc = otsardb_store_put(store, key, data, size, &options, NULL);
    if (rc == OTSARDB_STORE_OK) return OTSARDB_JOURNAL_OK;
    if (rc == OTSARDB_STORE_TIMEOUT) {
        otsardb_journal_result resolved = existing_object_matches(store, key, data, size);
        return resolved == OTSARDB_JOURNAL_NOT_FOUND ? OTSARDB_JOURNAL_AMBIGUOUS : resolved;
    }
    if (rc == OTSARDB_STORE_UNSUPPORTED) return OTSARDB_JOURNAL_UNSUPPORTED;
    return OTSARDB_JOURNAL_ERROR;
}

static otsardb_write_strategy effective_strategy(const otsardb_commit_request *request) {
    return request->write_strategy == OTSARDB_WRITE_STRATEGY_DEFAULT
               ? OTSARDB_WRITE_STRATEGY_CAS
               : request->write_strategy;
}

static int valid_parent_request(const otsardb_commit_request *request) {
    const char *parent_id = request->parent_commit_id;
    const char *parent_sha = request->parent_manifest_sha256;

    if (request->create_head_if_missing) {
        return request->parent_generation == 0 &&
               (!parent_id || parent_id[0] == '\0') &&
               (!parent_sha || parent_sha[0] == '\0');
    }

    return request->parent_generation > 0 &&
           parent_id && parent_id[0] != '\0' &&
           parent_sha && strlen(parent_sha) == OTSARDB_MANIFEST_SHA256_HEX_SIZE;
}

static otsardb_journal_result validate_single_writer_parent(otsardb_object_store *store,
                                                           const otsardb_commit_request *request) {
    otsardb_head current;
    char *etag = NULL;
    otsardb_journal_result rc = otsardb_journal_read_head(store, request->database_root, &current, &etag);
    free(etag);

    if (request->create_head_if_missing) {
        return rc == OTSARDB_JOURNAL_NOT_FOUND ? OTSARDB_JOURNAL_OK
                                              : (rc == OTSARDB_JOURNAL_OK ? OTSARDB_JOURNAL_CONFLICT : rc);
    }

    if (rc != OTSARDB_JOURNAL_OK) return rc;
    if (strcmp(current.database_id, request->database_id) != 0 ||
        current.generation != request->parent_generation ||
        strcmp(current.commit_id, request->parent_commit_id) != 0 ||
        strcmp(current.commit_sha256, request->parent_manifest_sha256) != 0) {
        return OTSARDB_JOURNAL_CONFLICT;
    }
    return OTSARDB_JOURNAL_OK;
}

static int resolve_segment_inputs(const otsardb_commit_request *request,
                                  const otsardb_segment_input **out_segments,
                                  uint32_t *out_count) {
    if (request->segments) {
        if (request->segment_payload) return 0;
        *out_segments = request->segments;
        *out_count = request->segment_count;
    } else if (request->segment_payload) {
        if (request->segment_count != 0) return 0;
        *out_segments = NULL;
        *out_count = 1;
    } else {
        return 0;
    }
    return 1;
}

/* Page sets recorded in the manifest must be sorted, deduped and 1-based so
 * the v4 format invariant holds and conflict detection can intersect them. */
static int valid_page_input(const uint32_t *pages, uint32_t page_count) {
    if (page_count > 0 && !pages) return 0;
    for (uint32_t i = 0; i < page_count; ++i) {
        if (pages[i] == 0) return 0;
        if (i > 0 && pages[i] <= pages[i - 1]) return 0;
    }
    return 1;
}

/* ---------------------------------------------------------------------------
 * at-rest segment encryption.
 *
 * When a cipher is active (request->cipher or the thread-local session
 * cipher), every encoded segment object is stored as the AES-256-GCM
 * envelope [iv(12)][ciphertext][tag(16)]:
 *
 * 1. the plaintext .otsseg bytes are encoded exactly as before;
 * 2. otsardb_cipher_encrypt_stream produces the AAD-independent ciphertext
 * stream with a fresh random IV;
 * 3. the content-addressed key is derived from sha256(iv || stream) —
 * the FULL stored envelope cannot seed the key because the tag depends
 * on the AAD which contains the key (fixed-point circularity; see
 * cipher.h). The manifest's sha256/size fields cover the FULL stored
 * envelope bytes;
 * 4. AAD = "<absolute object key>\n<commit_id>" (binds object identity +
 * manifest context); the authenticated pass produces the tag;
 * 5. the manifest gains the additive top-level "segment_enc" array.
 *
 * Encrypted segments never record frame_offsets: byte-range reads cannot
 * decrypt a GCM slice (the tag covers the whole ciphertext), so lazy
 * hydration whole-fetches them.
 * ------------------------------------------------------------------------- */

/* AAD = "<absolute object key>\n<commit_id>" (malloc'd). */
static int segment_aad_build(const char *absolute_key,
                             const char *commit_id,
                             char **out_aad,
                             size_t *out_aad_len) {
    size_t key_len = strlen(absolute_key);
    size_t id_len = strlen(commit_id);
    if (key_len > SIZE_MAX - id_len - 2u) return 0;
    size_t total = key_len + id_len + 2u;
    char *aad = (char *)malloc(total);
    if (!aad) return 0;
    memcpy(aad, absolute_key, key_len);
    aad[key_len] = '\n';
    memcpy(aad + key_len + 1u, commit_id, id_len);
    aad[key_len + 1u + id_len] = '\0';
    *out_aad = aad;
    *out_aad_len = total - 1u;
    return 1;
}

/* Encrypt the encoded segment object in place: *data/*size become the
 * envelope, entry records iv/tag. out_key_hex receives the content-addressed
 * key hash sha256(iv || ciphertext) — the FULL stored envelope hash cannot
 * seed the key (the tag depends on the AAD which contains the key), so the
 * OBJECT KEY is derived from the stream and the manifest's sha256 field
 * covers the full stored envelope. Returns 1 on success (caller frees the
 * NEW *data; on failure *data is freed and returned NULL). */
static int segment_encrypt_envelope(const otsardb_cipher *cipher,
                                    const char *database_root,
                                    const char *commit_id,
                                    unsigned char **data,
                                    size_t *size,
                                    otsardb_segment_enc_entry *entry,
                                    char out_key_hex[OTSARDB_SEGMENT_SHA256_HEX_SIZE + 1]) {
    memset(entry, 0, sizeof(*entry));

    unsigned char iv[OTSARDB_CIPHER_IV_BYTES];
    unsigned char *stream = NULL;
    size_t stream_size = 0;
    if (!otsardb_cipher_encrypt_stream(cipher, *data, *size, iv, &stream, &stream_size)) {
        free(*data);
        *data = NULL;
        return 0;
    }

    /* Content-addressed key over iv || stream. */
    unsigned char key_digest[OTSARDB_SHA256_SIZE];
    char key_hex[OTSARDB_SEGMENT_SHA256_HEX_SIZE + 1];
    size_t iv_stream_len = stream_size + OTSARDB_CIPHER_IV_BYTES;
    unsigned char *iv_stream = (unsigned char *)malloc(iv_stream_len ? iv_stream_len : 1u);
    if (!iv_stream) {
        free(stream);
        free(*data);
        *data = NULL;
        return 0;
    }
    memcpy(iv_stream, iv, OTSARDB_CIPHER_IV_BYTES);
    memcpy(iv_stream + OTSARDB_CIPHER_IV_BYTES, stream, stream_size);
    if (!otsardb_sha256(iv_stream, iv_stream_len, key_digest)) {
        wipe(iv_stream, iv_stream_len);
        free(iv_stream);
        free(stream);
        free(*data);
        *data = NULL;
        return 0;
    }
    otsardb_sha256_hex(key_digest, key_hex);
    wipe(iv_stream, iv_stream_len);
    free(iv_stream);
    free(stream);
    memcpy(out_key_hex, key_hex, OTSARDB_SEGMENT_SHA256_HEX_SIZE + 1u);

    char suffix[OTSARDB_KEY_MAX + 1];
    int n = snprintf(suffix,
                     sizeof(suffix),
                     "segments/sha256/%.2s/%s.otsseg",
                     key_hex,
                     key_hex);
    if (n <= 0 || (size_t)n >= sizeof(suffix)) {
        free(*data);
        *data = NULL;
        return 0;
    }
    char absolute_key[OTSARDB_KEY_MAX + 1];
    if (!root_join(database_root, suffix, absolute_key, sizeof(absolute_key))) {
        free(*data);
        *data = NULL;
        return 0;
    }

    char *aad = NULL;
    size_t aad_len = 0;
    if (!segment_aad_build(absolute_key, commit_id, &aad, &aad_len)) {
        free(*data);
        *data = NULL;
        return 0;
    }

    unsigned char *envelope = NULL;
    size_t envelope_size = 0;
    int ok = otsardb_cipher_encrypt_with_aad(cipher,
                                           iv,
                                           aad,
                                           aad_len,
                                           *data,
                                           *size,
                                           &envelope,
                                           &envelope_size,
                                           entry->tag);
    wipe(aad, aad_len);
    free(aad);
    if (!ok) {
        free(*data);
        *data = NULL;
        return 0;
    }
    memcpy(entry->iv, iv, OTSARDB_CIPHER_IV_BYTES);
    snprintf(entry->alg, sizeof(entry->alg), "%s", OTSARDB_CIPHER_ALG_NAME);
    entry->present = 1;

    free(*data);
    *data = envelope;
    *size = envelope_size;
    return 1;
}

/* JSON byte size of one "segment_enc" entry ({} or the full record). */
static size_t enc_entry_json_size(const otsardb_segment_enc_entry *entry) {
    if (!entry->present) return 2u; /* {} */
    char iv_hex[OTSARDB_CIPHER_IV_BYTES * 2u + 1u];
    char tag_hex[OTSARDB_CIPHER_TAG_BYTES * 2u + 1u];
    for (size_t i = 0; i < OTSARDB_CIPHER_IV_BYTES; ++i) {
        static const char digits[] = "0123456789abcdef";
        iv_hex[i * 2u] = digits[entry->iv[i] >> 4];
        iv_hex[i * 2u + 1u] = digits[entry->iv[i] & 0x0fu];
    }
    iv_hex[OTSARDB_CIPHER_IV_BYTES * 2u] = '\0';
    for (size_t i = 0; i < OTSARDB_CIPHER_TAG_BYTES; ++i) {
        static const char digits[] = "0123456789abcdef";
        tag_hex[i * 2u] = digits[entry->tag[i] >> 4];
        tag_hex[i * 2u + 1u] = digits[entry->tag[i] & 0x0fu];
    }
    tag_hex[OTSARDB_CIPHER_TAG_BYTES * 2u] = '\0';
    return (size_t)snprintf(NULL, 0, "{\"alg\":\"%s\",\"iv\":\"%s\",\"tag\":\"%s\"}",
                            entry->alg, iv_hex, tag_hex);
}

static int enc_entry_json_write(char *buf,
                                size_t size,
                                const otsardb_segment_enc_entry *entry) {
    if (!entry->present) {
        if (size < 3u) return -1;
        buf[0] = '{';
        buf[1] = '}';
        buf[2] = '\0';
        return 2;
    }
    char iv_hex[OTSARDB_CIPHER_IV_BYTES * 2u + 1u];
    char tag_hex[OTSARDB_CIPHER_TAG_BYTES * 2u + 1u];
    for (size_t i = 0; i < OTSARDB_CIPHER_IV_BYTES; ++i) {
        static const char digits[] = "0123456789abcdef";
        iv_hex[i * 2u] = digits[entry->iv[i] >> 4];
        iv_hex[i * 2u + 1u] = digits[entry->iv[i] & 0x0fu];
    }
    iv_hex[OTSARDB_CIPHER_IV_BYTES * 2u] = '\0';
    for (size_t i = 0; i < OTSARDB_CIPHER_TAG_BYTES; ++i) {
        static const char digits[] = "0123456789abcdef";
        tag_hex[i * 2u] = digits[entry->tag[i] >> 4];
        tag_hex[i * 2u + 1u] = digits[entry->tag[i] & 0x0fu];
    }
    tag_hex[OTSARDB_CIPHER_TAG_BYTES * 2u] = '\0';
    int n = snprintf(buf, size, "{\"alg\":\"%s\",\"iv\":\"%s\",\"tag\":\"%s\"}",
                     entry->alg, iv_hex, tag_hex);
    return n > 0 && (size_t)n < size ? n : -1;
}

/* Inject the additive top-level "segment_enc" array into an encoded manifest
 * JSON (before its closing brace) and recompute the manifest SHA-256 over
 * the FINAL text. *json/*size are replaced on success. */
static int manifest_inject_segment_enc(char **json,
                                       size_t *size,
                                       char out_sha256[OTSARDB_MANIFEST_SHA256_HEX_SIZE + 1],
                                       const otsardb_segment_enc_entry *entries,
                                       uint32_t count) {
    size_t array_bytes = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (i > 0) ++array_bytes;
        array_bytes += enc_entry_json_size(&entries[i]);
    }
    const char header[] = ",\"segment_enc\":[";
    const size_t header_size = sizeof(header) - 1u;
    const size_t inject_bytes = header_size + array_bytes + 1u; /* + ']' */

    const char *text = *json;
    size_t text_size = *size;
    /* The encoder output ends with '}' + '\n' (with or without the
     * post_apply_checksum field before it). Splice before the LAST '}'. */
    size_t closing = text_size;
    while (closing > 0 && text[closing - 1u] == '\n') --closing;
    if (closing == 0 || text[closing - 1u] != '}') return 0;

    size_t new_size = text_size + inject_bytes;
    if (new_size > (size_t)INT_MAX) return 0;
    char *out = (char *)malloc(new_size);
    if (!out) return 0;
    memcpy(out, text, closing - 1u);
    memcpy(out + closing - 1u, header, header_size);
    char *p = out + closing - 1u + header_size;
    for (uint32_t i = 0; i < count; ++i) {
        if (i > 0) *p++ = ',';
        int n = enc_entry_json_write(p, (size_t)(out + new_size - p), &entries[i]);
        if (n < 0) {
            free(out);
            return 0;
        }
        p += n;
    }
    *p++ = ']';
    memcpy(p, text + closing - 1u, text_size - (closing - 1u)); /* "}\n" */

    if (!otsardb_sha256_hex_data(out, new_size, out_sha256)) {
        free(out);
        return 0;
    }
    free(*json);
    *json = out;
    *size = new_size;
    return 1;
}

/* The cipher for this publish: request override, else the thread-local
 * session cipher (set by s3_database.cpp at open). NULL = plaintext. */
static const otsardb_cipher *request_cipher(const otsardb_commit_request *request) {
    return request->cipher ? request->cipher : otsardb_cipher_thread_get();
}

otsardb_journal_result otsardb_publish_commit(otsardb_object_store *store,
                                          const otsardb_commit_request *request,
                                          otsardb_commit_result *out_result) {
    const otsardb_segment_input *segment_inputs = NULL;
    uint32_t segment_count = 0;
    if (!store || !request || !out_result || !request->database_root ||
        !request->database_id || !request->commit_id || request->commit_id[0] == '\0' ||
        request->page_size == 0 ||
        !resolve_segment_inputs(request, &segment_inputs, &segment_count) ||
        segment_count == 0 || segment_count > OTSARDB_MANIFEST_MAX_SEGMENTS ||
        strlen(request->database_id) > OTSARDB_COMMIT_ID_MAX ||
        strlen(request->commit_id) > OTSARDB_COMMIT_ID_MAX ||
        (request->parent_commit_id && strlen(request->parent_commit_id) > OTSARDB_COMMIT_ID_MAX) ||
        !valid_parent_request(request)) {
        return OTSARDB_JOURNAL_INVALID;
    }

    otsardb_write_strategy strategy = effective_strategy(request);
    if (strategy != OTSARDB_WRITE_STRATEGY_CAS &&
        strategy != OTSARDB_WRITE_STRATEGY_SINGLE_WRITER) {
        return OTSARDB_JOURNAL_INVALID;
    }

    for (uint32_t i = 0; i < segment_count; ++i) {
        if (segment_inputs &&
            !valid_page_input(segment_inputs[i].pages, segment_inputs[i].page_count)) {
            return OTSARDB_JOURNAL_INVALID;
        }
    }

    if (strategy == OTSARDB_WRITE_STRATEGY_CAS) {
        if (request->create_head_if_missing) {
            if (!otsardb_store_supports(store, OTSARDB_STORE_CAP_CONDITIONAL_CREATE)) {
                return OTSARDB_JOURNAL_UNSUPPORTED;
            }
            if (request->expected_head_etag) return OTSARDB_JOURNAL_INVALID;
        } else {
            if (!otsardb_store_supports(store, OTSARDB_STORE_CAP_CONDITIONAL_UPDATE)) {
                return OTSARDB_JOURNAL_UNSUPPORTED;
            }
            if (!request->expected_head_etag) return OTSARDB_JOURNAL_INVALID;
        }
    } else {
        if (request->expected_head_etag) return OTSARDB_JOURNAL_INVALID;
        otsardb_journal_result parent_rc = validate_single_writer_parent(store, request);
        if (parent_rc != OTSARDB_JOURNAL_OK) return parent_rc;
    }

    memset(out_result, 0, sizeof(*out_result));

    otsardb_commit_manifest manifest = {0};
    memset(&manifest, 0, sizeof(manifest));
    manifest.format_version = OTSARDB_COMMIT_MANIFEST_VERSION;
    manifest.generation = request->parent_generation + 1u;
    manifest.parent_generation = request->parent_generation;
    manifest.page_size = request->page_size;
    manifest.database_size_pages = request->database_size_pages;
    manifest.segment_count = segment_count;
    snprintf(manifest.database_id, sizeof(manifest.database_id), "%s", request->database_id);
    snprintf(manifest.commit_id, sizeof(manifest.commit_id), "%s", request->commit_id);
    snprintf(manifest.parent_commit_id,
             sizeof(manifest.parent_commit_id),
             "%s",
             request->parent_commit_id ? request->parent_commit_id : "");
    snprintf(manifest.parent_manifest_sha256,
             sizeof(manifest.parent_manifest_sha256),
             "%s",
             request->parent_manifest_sha256 ? request->parent_manifest_sha256 : "");
    if (request->post_apply_checksum && request->post_apply_checksum[0] != '\0') {
        snprintf(manifest.post_apply_checksum,
                 sizeof(manifest.post_apply_checksum),
                 "%s",
                 request->post_apply_checksum);
    }

    /* Upload every immutable segment first. Until HEAD points at the manifest,
     * all of these objects are prepared/orphan state. */
    const otsardb_cipher *cipher = request_cipher(request);
    otsardb_segment_enc_entry enc_entries[OTSARDB_MANIFEST_MAX_SEGMENTS];
    memset(enc_entries, 0, sizeof(enc_entries));
    int any_encrypted = 0;
    for (uint32_t i = 0; i < segment_count; ++i) {
        const void *payload = segment_inputs ? segment_inputs[i].payload : request->segment_payload;
        size_t payload_size = segment_inputs ? segment_inputs[i].payload_size
                                             : request->segment_payload_size;
        if ((!payload && payload_size != 0) || payload_size > UINT64_MAX) {
            return OTSARDB_JOURNAL_INVALID;
        }

        unsigned char *segment_data = NULL;
        size_t segment_size = 0;
        char segment_sha256[65];
        otsardb_segment_frame_info frames;
        memset(&frames, 0, sizeof(frames));
        if (!otsardb_segment_encode_with_frames(OTSARDB_SEGMENT_WAL_FRAMES,
                                              request->commit_id,
                                              payload,
                                              payload_size,
                                              &segment_data,
                                              &segment_size,
                                              segment_sha256,
                                              1,
                                              &frames)) {
            otsardb_segment_frame_info_release(&frames);
            return OTSARDB_JOURNAL_ERROR;
        }

        /* encrypt the whole encoded segment object with the
         * session/request cipher. The envelope replaces the plaintext object;
         * the manifest sha256/size below then describe the STORED envelope.
         * The OBJECT KEY is derived from the ciphertext stream
         * (sha256(iv||ct)) — the full envelope hash cannot seed the key
         * (fixed-point circularity, see cipher.h) — and the AAD binds that
         * key, so the manifest's key field is what decryption reconstructs. */
        int segment_encrypted = 0;
        char enc_key_hex[OTSARDB_SEGMENT_SHA256_HEX_SIZE + 1];
        if (cipher) {
            if (!segment_encrypt_envelope(cipher,
                                          request->database_root,
                                          request->commit_id,
                                          &segment_data,
                                          &segment_size,
                                          &enc_entries[i],
                                          enc_key_hex)) {
                otsardb_segment_frame_info_release(&frames);
                return OTSARDB_JOURNAL_ERROR;
            }
            if (!otsardb_sha256_hex_data(segment_data, segment_size, segment_sha256)) {
                free(segment_data);
                otsardb_segment_frame_info_release(&frames);
                return OTSARDB_JOURNAL_ERROR;
            }
            segment_encrypted = 1;
            any_encrypted = 1;
        }

        char segment_suffix[OTSARDB_KEY_MAX + 1];
        const char *key_hash = segment_encrypted ? enc_key_hex : segment_sha256;
        int n = snprintf(segment_suffix,
                         sizeof(segment_suffix),
                         "segments/sha256/%.2s/%s.otsseg",
                         key_hash,
                         key_hash);
        if (n <= 0 || (size_t)n >= sizeof(segment_suffix)) {
            free(segment_data);
            return OTSARDB_JOURNAL_INVALID;
        }
        char segment_key[OTSARDB_KEY_MAX + 1];
        if (!root_join(request->database_root, segment_suffix, segment_key, sizeof(segment_key))) {
            free(segment_data);
            return OTSARDB_JOURNAL_INVALID;
        }

        otsardb_journal_result rc = immutable_put(store, segment_key, segment_data, segment_size);
        free(segment_data);
        if (rc != OTSARDB_JOURNAL_OK) {
            otsardb_segment_frame_info_release(&frames);
            return rc;
        }

        /* Manifest v4 stores the suffix relative to the database root. */
        size_t suffix_len = strlen(segment_suffix);
        if (suffix_len == 0 ||
            suffix_len >= sizeof(manifest.segments[i].key)) {
            otsardb_segment_frame_info_release(&frames);
            return OTSARDB_JOURNAL_INVALID;
        }
        memcpy(manifest.segments[i].key, segment_suffix, suffix_len + 1u);
        snprintf(manifest.segments[i].sha256,
                 sizeof(manifest.segments[i].sha256),
                 "%s",
                 segment_sha256);
        manifest.segments[i].size = (uint64_t)segment_size;
        /* v4: per-segment page set recorded in the manifest. The pointer is
         * borrowed from the request for the duration of this call (and for
         * the result manifest the caller receives). */
        manifest.segments[i].page_count = segment_inputs ? segment_inputs[i].page_count : 0;
        manifest.segments[i].pages = segment_inputs
                                         ? (uint32_t *)segment_inputs[i].pages
                                         : NULL;
        /* v4 + record the per-frame block offsets (sorted
         * page order, contiguous block ranges) so lazy hydration can fetch
         * exactly the needed frame bytes. Only when the encoder's block
         * table matches the request's page set exactly â€” any mismatch (or a
         * non-WAL-batch payload that fell back to v2) records NO offsets and
         * lazy hydration falls back to the whole-segment fetch. The offsets
         * are owned by this call and released after the manifest JSON is
         * built: the borrow-only result manifest never carries them.
         * encrypted segments NEVER record offsets — a GCM
         * slice cannot be authenticated/decrypted without the whole
         * ciphertext, so lazy hydration whole-fetches them. */
        if (!segment_encrypted &&
            segment_inputs && segment_inputs[i].pages &&
            frames.count == segment_inputs[i].page_count) {
            int pages_match = 1;
            for (uint32_t p = 0; p < frames.count; ++p) {
                if (frames.page_nos[p] != segment_inputs[i].pages[p]) {
                    pages_match = 0;
                    break;
                }
            }
            if (pages_match) {
                manifest.segments[i].frame_offsets = frames.offsets;
                frames.offsets = NULL;
            }
        }
        otsardb_segment_frame_info_release(&frames);
    }

    char *manifest_json = NULL;
    size_t manifest_size = 0;
    char manifest_sha256[65];
    if (!otsardb_commit_manifest_encode_json(&manifest,
                                           &manifest_json,
                                           &manifest_size,
                                           manifest_sha256)) {
        for (uint32_t i = 0; i < segment_count; ++i) {
            free(manifest.segments[i].frame_offsets);
            manifest.segments[i].frame_offsets = NULL;
        }
        return OTSARDB_JOURNAL_ERROR;
    }
    /* inject the additive "segment_enc" array (one entry per
     * segment, aligned by index; {} for plaintext segments) and recompute
     * the manifest SHA-256 over the FINAL published text — HEAD and the
     * parent chain authenticate it like every other manifest field. */
    if (any_encrypted &&
        !manifest_inject_segment_enc(&manifest_json,
                                     &manifest_size,
                                     manifest_sha256,
                                     enc_entries,
                                     segment_count)) {
        for (uint32_t i = 0; i < segment_count; ++i) {
            free(manifest.segments[i].frame_offsets);
            manifest.segments[i].frame_offsets = NULL;
        }
        free(manifest_json);
        return OTSARDB_JOURNAL_ERROR;
    }
    /* The manifest JSON (the durable artifact) carries the offsets; the
     * in-memory result manifest must stay borrow-only, so release them. */
    for (uint32_t i = 0; i < segment_count; ++i) {
        free(manifest.segments[i].frame_offsets);
        manifest.segments[i].frame_offsets = NULL;
    }

    char manifest_suffix[OTSARDB_KEY_MAX + 1];
    int n = snprintf(manifest_suffix, sizeof(manifest_suffix), "commits/%s.json", request->commit_id);
    if (n <= 0 || (size_t)n >= sizeof(manifest_suffix)) {
        free(manifest_json);
        return OTSARDB_JOURNAL_INVALID;
    }
    char manifest_key[OTSARDB_KEY_MAX + 1];
    if (!root_join(request->database_root, manifest_suffix, manifest_key, sizeof(manifest_key))) {
        free(manifest_json);
        return OTSARDB_JOURNAL_INVALID;
    }

    otsardb_journal_result rc = immutable_put(store, manifest_key, manifest_json, manifest_size);
    free(manifest_json);
    if (rc != OTSARDB_JOURNAL_OK) return rc;

    otsardb_head head;
    memset(&head, 0, sizeof(head));
    head.format_version = 1;
    head.generation = manifest.generation;
    snprintf(head.database_id, sizeof(head.database_id), "%s", request->database_id);
    snprintf(head.commit_id, sizeof(head.commit_id), "%s", request->commit_id);
    snprintf(head.commit_key, sizeof(head.commit_key), "%s", manifest_key);
    snprintf(head.commit_sha256, sizeof(head.commit_sha256), "%s", manifest_sha256);

    char *new_etag = NULL;
    if (strategy == OTSARDB_WRITE_STRATEGY_CAS) {
        rc = otsardb_journal_publish_head(store,
                                        request->database_root,
                                        &head,
                                        request->expected_head_etag,
                                        request->create_head_if_missing,
                                        &new_etag);
    } else {
        rc = otsardb_journal_publish_head_unconditional(store,
                                                      request->database_root,
                                                      &head,
                                                      &new_etag);
    }
    if (rc != OTSARDB_JOURNAL_OK) {
        free(new_etag);
        return rc;
    }

    out_result->head = head;
    out_result->manifest = manifest;
    if (new_etag) {
        snprintf(out_result->head_etag, sizeof(out_result->head_etag), "%s", new_etag);
        free(new_etag);
    }
    return OTSARDB_JOURNAL_OK;
}

static int build_manifest_key(const char *database_root,
                              const char *commit_id,
                              char *out,
                              size_t out_size) {
    char suffix[OTSARDB_KEY_MAX + 1];
    int n = snprintf(suffix, sizeof(suffix), "commits/%s.json", commit_id);
    if (n <= 0 || (size_t)n >= sizeof(suffix)) return 0;
    return root_join(database_root, suffix, out, out_size);
}

static otsardb_journal_result read_manifest(otsardb_object_store *store,
                                          const char *key,
                                          const char *expected_sha256,
                                          otsardb_commit_manifest *out_manifest) {
    otsardb_store_object object = {0};
    otsardb_store_result rc = otsardb_store_get(store, key, &object);
    if (rc == OTSARDB_STORE_NOT_FOUND) return OTSARDB_JOURNAL_NOT_FOUND;
    if (rc != OTSARDB_STORE_OK) return OTSARDB_JOURNAL_ERROR;

    char actual_sha256[OTSARDB_MANIFEST_SHA256_HEX_SIZE + 1];
    int ok = otsardb_sha256_hex_data(object.data, object.size, actual_sha256) &&
             (!expected_sha256 || expected_sha256[0] == '\0' ||
              strcmp(actual_sha256, expected_sha256) == 0) &&
             otsardb_commit_manifest_decode_json((const char *)object.data,
                                               object.size,
                                               out_manifest);
    otsardb_store_release_object(store, &object);
    return ok ? OTSARDB_JOURNAL_OK : OTSARDB_JOURNAL_INVALID;
}

/* (audit A5): the checkpoint-covered boundary used to make a
 * resolve whose walk descends into the GC-swept region DEFINITIVE instead of
 * an error.
 *
 * GC (gc.c) deletes only history strictly BELOW the checkpoint's covered
 * generation, keeping the covered manifest and everything above it live.
 * A resolve whose requested parent_generation sits below the covered
 * boundary walks past the covered manifest into the swept region and, until
 * this helper, returned OTSARDB_JOURNAL_NOT_FOUND (error-class, state
 * UNKNOWN) — the audit's "What a correct system should do" for A5 is
 * "resolve from the checkpoint base manifest (kept alive)". This helper
 * establishes that boundary exactly as GC and recovery do, WITHOUT pulling a
 * dependency on checkpoint_manifest.c into the direct-compile test targets:
 * the CHECKPOINT.json pointer's covered_generation is read from its JSON, the
 * checkpoint manifest object is hash-bound to the pointer, and the
 * manifest's covered_generation must equal the pointer's and lie within the
 * head's range. Only when a valid checkpoint proves the boundary may a
 * swept-region read be classified as checkpoint-covered (definitive
 * SUPERSEDED); without it, a missing manifest is indistinguishable from
 * genuine corruption and the resolve keeps failing closed with NOT_FOUND. */

static int extract_u64_field(const char *json, const char *name, uint64_t *out) {
    if (!json || !name || !out) return 0;
    char pattern[80];
    if (snprintf(pattern, sizeof(pattern), "\"%s\":", name) >= (int)sizeof(pattern)) return 0;
    const char *p = strstr(json, pattern);
    if (!p) return 0;
    p += strlen(pattern);
    while (*p == ' ' || *p == '\t') ++p;
    if (*p == '-') return 0;
    char *end = NULL;
    errno = 0;
    unsigned long long value = strtoull(p, &end, 10);
    if (end == p || errno == ERANGE) return 0;
    *out = (uint64_t)value;
    return 1;
}

static int extract_string_field(const char *json,
                                const char *name,
                                char *out,
                                size_t out_size) {
    if (!json || !name || !out || out_size == 0) return 0;
    char pattern[80];
    if (snprintf(pattern, sizeof(pattern), "\"%s\":\"", name) >= (int)sizeof(pattern)) return 0;
    const char *p = strstr(json, pattern);
    if (!p) return 0;
    p += strlen(pattern);
    const char *end = strchr(p, '"');
    if (!end) return 0;
    size_t len = (size_t)(end - p);
    if (len >= out_size) return 0;
    memcpy(out, p, len);
    out[len] = '\0';
    return 1;
}

static int resolve_covered_generation(otsardb_object_store *store,
                                      const char *database_root,
                                      uint64_t head_generation,
                                      uint64_t *out_covered) {
    if (!store || !database_root || !out_covered) return 0;

    char ptr_key[OTSARDB_KEY_MAX + 1];
    if (!root_join(database_root, "CHECKPOINT.json", ptr_key, sizeof(ptr_key))) return 0;

    otsardb_store_object ptr_obj = {0};
    if (otsardb_store_get(store, ptr_key, &ptr_obj) != OTSARDB_STORE_OK) return 0;

    uint64_t covered = 0;
    char cp_sha[OTSARDB_CHECKPOINT_SHA256_HEX_SIZE + 1];
    char cp_key[OTSARDB_KEY_MAX + 1];
    int ok = extract_u64_field((const char *)ptr_obj.data,
                               "covered_generation", &covered) &&
             extract_string_field((const char *)ptr_obj.data,
                                  "checkpoint_sha256",
                                  cp_sha, sizeof(cp_sha)) &&
             strlen(cp_sha) == OTSARDB_CHECKPOINT_SHA256_HEX_SIZE &&
             extract_string_field((const char *)ptr_obj.data,
                                  "checkpoint_key",
                                  cp_key, sizeof(cp_key));
    otsardb_store_release_object(store, &ptr_obj);
    if (!ok) return 0;
    if (covered == 0 || covered > head_generation) return 0;

    /* Hash-bind the checkpoint manifest to the pointer, exactly like gc.c
     * and recovery do before trusting its covered_generation. */
    otsardb_store_object cp_obj = {0};
    if (otsardb_store_get(store, cp_key, &cp_obj) != OTSARDB_STORE_OK) return 0;
    char actual_sha[OTSARDB_CHECKPOINT_SHA256_HEX_SIZE + 1];
    uint64_t manifest_covered = 0;
    ok = otsardb_sha256_hex_data(cp_obj.data, cp_obj.size, actual_sha) &&
         strcmp(actual_sha, cp_sha) == 0 &&
         extract_u64_field((const char *)cp_obj.data,
                           "covered_generation", &manifest_covered) &&
         manifest_covered == covered;
    otsardb_store_release_object(store, &cp_obj);
    if (!ok) return 0;

    *out_covered = covered;
    return 1;
}

otsardb_journal_result otsardb_resolve_commit(otsardb_object_store *store,
                                          const char *database_root,
                                          const char *commit_id,
                                          uint64_t parent_generation,
                                          otsardb_commit_state *out_state) {
    if (!store || !database_root || !commit_id || commit_id[0] == '\0' || !out_state) {
        return OTSARDB_JOURNAL_INVALID;
    }
    *out_state = OTSARDB_COMMIT_STATE_UNKNOWN;

    otsardb_head head;
    char *etag = NULL;
    otsardb_journal_result rc = otsardb_journal_read_head(store, database_root, &head, &etag);
    free(etag);
    if (rc == OTSARDB_JOURNAL_NOT_FOUND) {
        *out_state = OTSARDB_COMMIT_STATE_NOT_COMMITTED;
        return OTSARDB_JOURNAL_OK;
    }
    if (rc != OTSARDB_JOURNAL_OK) return rc;

    if (strcmp(head.commit_id, commit_id) == 0) {
        *out_state = OTSARDB_COMMIT_STATE_COMMITTED;
        return OTSARDB_JOURNAL_OK;
    }
    if (head.generation <= parent_generation) {
        *out_state = OTSARDB_COMMIT_STATE_NOT_COMMITTED;
        return OTSARDB_JOURNAL_OK;
    }

    char current_commit_id[OTSARDB_COMMIT_ID_MAX + 1];
    char current_key[OTSARDB_KEY_MAX + 1];
    char expected_manifest_sha[OTSARDB_MANIFEST_SHA256_HEX_SIZE + 1];
    snprintf(current_commit_id, sizeof(current_commit_id), "%s", head.commit_id);
    snprintf(current_key, sizeof(current_key), "%s", head.commit_key);
    snprintf(expected_manifest_sha, sizeof(expected_manifest_sha), "%s", head.commit_sha256);
    uint64_t current_generation = head.generation;

    /* (audit finding F05): bounded walk. Every step is a
     * manifest GET; a store with bucket-write access could force an
     * arbitrarily long valid hash-linked chain, holding the writer for hours
     * of sequential fetches. The walk refuses chains at least as deep as the
     * recovery budget (same >= semantics as recovery's key_count guard) and
     * fails closed with JOURNAL_INVALID. */
    uint64_t steps = 0;
    while (current_generation > parent_generation) {
        if (++steps >= OTSARDB_RECOVERY_MAX_COMMITS) {
            return OTSARDB_JOURNAL_INVALID;
        }
        if (strcmp(current_commit_id, commit_id) == 0) {
            *out_state = OTSARDB_COMMIT_STATE_COMMITTED;
            return OTSARDB_JOURNAL_OK;
        }

        otsardb_commit_manifest manifest = {0};
        rc = read_manifest(store, current_key, expected_manifest_sha, &manifest);
        if (rc == OTSARDB_JOURNAL_NOT_FOUND) {
            /* (audit A5): the walk descended past the
             * checkpoint-covered boundary into the GC-swept region (the
             * requested parent_generation sits >= 2 generations below the
             * covered generation). The checkpoint base manifest is kept alive
             * by GC, so the covered boundary is determinable; the commit is
             * definitively NOT on the authenticated live chain
             * (generations [covered..head]) and the chain has already passed
             * the commit's parent generation (head.generation >
             * parent_generation), so the definitive classification is
             * SUPERSEDED — never an error-class result. Only when a valid
             * checkpoint proves the boundary; a missing manifest in the live
             * region (or with no checkpoint at all) stays fail-closed
             * NOT_FOUND (genuine corruption is never masked as SUPERSEDED). */
            uint64_t covered = 0;
            if (current_generation < head.generation &&
                resolve_covered_generation(store, database_root, head.generation,
                                           &covered) &&
                current_generation < covered) {
                *out_state = OTSARDB_COMMIT_STATE_SUPERSEDED;
                return OTSARDB_JOURNAL_OK;
            }
            return rc;
        }
        if (rc != OTSARDB_JOURNAL_OK) return rc;
        if (manifest.generation != current_generation ||
            strcmp(manifest.commit_id, current_commit_id) != 0) {
            otsardb_commit_manifest_free(&manifest);
            return OTSARDB_JOURNAL_INVALID;
        }
        if (manifest.parent_generation < parent_generation) {
            otsardb_commit_manifest_free(&manifest);
            break;
        }
        if (manifest.parent_generation == parent_generation) {
            *out_state = strcmp(manifest.parent_commit_id, commit_id) == 0
                             ? OTSARDB_COMMIT_STATE_COMMITTED
                             : OTSARDB_COMMIT_STATE_SUPERSEDED;
            otsardb_commit_manifest_free(&manifest);
            return OTSARDB_JOURNAL_OK;
        }
        if (manifest.parent_commit_id[0] == '\0') {
            otsardb_commit_manifest_free(&manifest);
            break;
        }

        current_generation = manifest.parent_generation;
        snprintf(current_commit_id, sizeof(current_commit_id), "%s", manifest.parent_commit_id);
        if (!build_manifest_key(database_root, current_commit_id, current_key, sizeof(current_key))) {
            otsardb_commit_manifest_free(&manifest);
            return OTSARDB_JOURNAL_INVALID;
        }
        if (manifest.format_version >= 2) {
            snprintf(expected_manifest_sha,
                     sizeof(expected_manifest_sha),
                     "%s",
                     manifest.parent_manifest_sha256);
        } else {
            expected_manifest_sha[0] = '\0';
        }
        otsardb_commit_manifest_free(&manifest);
    }

    *out_state = OTSARDB_COMMIT_STATE_SUPERSEDED;
    return OTSARDB_JOURNAL_OK;
}

/* The request carries per-segment page sets only when it uses the segments
 * array AND every segment declares pages. The legacy single-segment payload
 * path (and any segment with pages == NULL) means "unknown page set": the
 * conservative rule treats it as overlapping everything. */
static int request_has_page_info(const otsardb_commit_request *request,
                                 const otsardb_segment_input *segment_inputs,
                                 uint32_t segment_count) {
    if (!segment_inputs) return 0;
    for (uint32_t i = 0; i < segment_count; ++i) {
        if (!segment_inputs[i].pages) return 0;
    }
    return 1;
}

/* Both arrays are sorted (v4 invariant for manifest page sets; publish-time
 * validation for request page sets). Merge-compare for any common element. */
static int sorted_u32_any_intersect(const uint32_t *a,
                                    uint32_t a_count,
                                    const uint32_t *b,
                                    uint32_t b_count) {
    uint32_t i = 0;
    uint32_t j = 0;
    while (i < a_count && j < b_count) {
        if (a[i] < b[j]) {
            ++i;
        } else if (b[j] < a[i]) {
            ++j;
        } else {
            return 1;
        }
    }
    return 0;
}

static int request_overlaps_manifest(const otsardb_segment_input *segment_inputs,
                                     uint32_t segment_count,
                                     const otsardb_commit_manifest *manifest) {
    for (uint32_t s = 0; s < manifest->segment_count; ++s) {
        const otsardb_manifest_segment_ref *ref = &manifest->segments[s];
        /* Empty/unknown page set in an interleaved manifest (legacy v1-v3,
         * or a v4 manifest published without pages): conservative overlap. */
        if (ref->page_count == 0 || !ref->pages) return 1;
        for (uint32_t r = 0; r < segment_count; ++r) {
            if (sorted_u32_any_intersect(segment_inputs[r].pages,
                                         segment_inputs[r].page_count,
                                         ref->pages,
                                         ref->page_count)) {
                return 1;
            }
        }
    }
    return 0;
}

otsardb_journal_result otsardb_publish_commit_detect_conflict(otsardb_object_store *store,
                                                          const char *database_root,
                                                          const otsardb_commit_request *request,
                                                          otsardb_commit_state *out_state,
                                                          int *out_page_overlap) {
    if (!store || !database_root || database_root[0] == '\0' || !request ||
        !out_state || !out_page_overlap ||
        !request->commit_id || request->commit_id[0] == '\0' ||
        !valid_parent_request(request)) {
        return OTSARDB_JOURNAL_INVALID;
    }

    const otsardb_segment_input *segment_inputs = NULL;
    uint32_t segment_count = 0;
    if (!resolve_segment_inputs(request, &segment_inputs, &segment_count)) {
        return OTSARDB_JOURNAL_INVALID;
    }
    for (uint32_t i = 0; i < segment_count; ++i) {
        if (segment_inputs &&
            !valid_page_input(segment_inputs[i].pages, segment_inputs[i].page_count)) {
            return OTSARDB_JOURNAL_INVALID;
        }
    }

    *out_state = OTSARDB_COMMIT_STATE_UNKNOWN;
    *out_page_overlap = 0;
    int request_pages_known = request_has_page_info(request, segment_inputs, segment_count);
    if (!request_pages_known) *out_page_overlap = 1;

    otsardb_head head;
    char *etag = NULL;
    otsardb_journal_result rc = otsardb_journal_read_head(store, database_root, &head, &etag);
    free(etag);
    if (rc == OTSARDB_JOURNAL_NOT_FOUND) {
        *out_state = OTSARDB_COMMIT_STATE_NOT_COMMITTED;
        return OTSARDB_JOURNAL_OK;
    }
    if (rc != OTSARDB_JOURNAL_OK) return rc;

    if (strcmp(head.commit_id, request->commit_id) == 0) {
        *out_state = OTSARDB_COMMIT_STATE_COMMITTED;
        *out_page_overlap = 0;
        return OTSARDB_JOURNAL_OK;
    }
    if (head.generation <= request->parent_generation) {
        *out_state = OTSARDB_COMMIT_STATE_NOT_COMMITTED;
        return OTSARDB_JOURNAL_OK;
    }

    char current_commit_id[OTSARDB_COMMIT_ID_MAX + 1];
    char current_key[OTSARDB_KEY_MAX + 1];
    char expected_manifest_sha[OTSARDB_MANIFEST_SHA256_HEX_SIZE + 1];
    snprintf(current_commit_id, sizeof(current_commit_id), "%s", head.commit_id);
    snprintf(current_key, sizeof(current_key), "%s", head.commit_key);
    snprintf(expected_manifest_sha, sizeof(expected_manifest_sha), "%s", head.commit_sha256);
    uint64_t current_generation = head.generation;

    /* (audit finding F05): bounded walk, same cap and
     * fail-closed JOURNAL_INVALID semantics as otsardb_resolve_commit. */
    uint64_t steps = 0;
    while (current_generation > request->parent_generation) {
        if (++steps >= OTSARDB_RECOVERY_MAX_COMMITS) {
            return OTSARDB_JOURNAL_INVALID;
        }
        if (strcmp(current_commit_id, request->commit_id) == 0) {
            *out_state = OTSARDB_COMMIT_STATE_COMMITTED;
            *out_page_overlap = 0;
            return OTSARDB_JOURNAL_OK;
        }

        otsardb_commit_manifest manifest = {0};
        rc = read_manifest(store, current_key, expected_manifest_sha, &manifest);
        if (rc == OTSARDB_JOURNAL_NOT_FOUND) {
            /* (audit A5): same definitive classification as
             * otsardb_resolve_commit — a walk that descends below the
             * checkpoint-covered boundary into the GC-swept region resolves
             * SUPERSEDED instead of failing with NOT_FOUND, but only when a
             * valid checkpoint proves the boundary (otherwise genuine
             * corruption stays fail-closed). */
            uint64_t covered = 0;
            if (current_generation < head.generation &&
                resolve_covered_generation(store, database_root, head.generation,
                                           &covered) &&
                current_generation < covered) {
                *out_state = OTSARDB_COMMIT_STATE_SUPERSEDED;
                otsardb_commit_manifest_free(&manifest);
                return OTSARDB_JOURNAL_OK;
            }
            otsardb_commit_manifest_free(&manifest);
            return rc;
        }
        if (rc != OTSARDB_JOURNAL_OK) {
            otsardb_commit_manifest_free(&manifest);
            return rc;
        }
        if (manifest.generation != current_generation ||
            strcmp(manifest.commit_id, current_commit_id) != 0) {
            otsardb_commit_manifest_free(&manifest);
            return OTSARDB_JOURNAL_INVALID;
        }

        /* This manifest sits strictly between our parent and the remote HEAD:
         * another writer committed it on top of our parent. Its pages count
         * against us. */
        if (request_pages_known &&
            request_overlaps_manifest(segment_inputs,
                                      segment_count,
                                      &manifest)) {
            *out_page_overlap = 1;
        }

        if (manifest.parent_generation < request->parent_generation) {
            otsardb_commit_manifest_free(&manifest);
            break;
        }
        if (manifest.parent_generation == request->parent_generation) {
            *out_state = strcmp(manifest.parent_commit_id, request->commit_id) == 0
                             ? OTSARDB_COMMIT_STATE_COMMITTED
                             : OTSARDB_COMMIT_STATE_SUPERSEDED;
            otsardb_commit_manifest_free(&manifest);
            return OTSARDB_JOURNAL_OK;
        }
        if (manifest.parent_commit_id[0] == '\0') {
            otsardb_commit_manifest_free(&manifest);
            break;
        }

        current_generation = manifest.parent_generation;
        snprintf(current_commit_id, sizeof(current_commit_id), "%s", manifest.parent_commit_id);
        if (!build_manifest_key(database_root, current_commit_id, current_key, sizeof(current_key))) {
            otsardb_commit_manifest_free(&manifest);
            return OTSARDB_JOURNAL_INVALID;
        }
        if (manifest.format_version >= 2) {
            snprintf(expected_manifest_sha,
                     sizeof(expected_manifest_sha),
                     "%s",
                     manifest.parent_manifest_sha256);
        } else {
            expected_manifest_sha[0] = '\0';
        }
        otsardb_commit_manifest_free(&manifest);
    }

    *out_state = OTSARDB_COMMIT_STATE_SUPERSEDED;
    return OTSARDB_JOURNAL_OK;
}
