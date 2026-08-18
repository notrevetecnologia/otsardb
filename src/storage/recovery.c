#include "recovery.h"

#include "checkpoint_manifest.h"
#include "checkpoint_page.h"
#include "checksum.h"
#include "cipher.h"
#include "commit_manifest.h"
#include "db_metadata.h"
#include "journal.h"
#include "segment.h"
#include "sha256.h"
#include "wal_batch.h"

#include <stdint.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#define otsardb_fileno _fileno
#define otsardb_ftruncate _chsize_s
#else
#include <sys/types.h>
#include <unistd.h>
#define otsardb_fileno fileno
#endif

static int recovery_diag_enabled(void) {
    const char *v = getenv("OTSARDB_DIAG");
    return v && v[0] == '1';
}

static void recovery_diag(const char *fmt, ...) {
    if (!recovery_diag_enabled()) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "otsardb: recovery: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

static int root_join(const char *root, const char *suffix, char *out, size_t out_size) {
    if (!root || !suffix || !out || out_size == 0) return 0;
    size_t root_len = strlen(root);
    int needs_slash = root_len > 0 && root[root_len - 1] != '/';
    int n = snprintf(out, out_size, "%s%s%s", root, needs_slash ? "/" : "", suffix);
    return n > 0 && (size_t)n < out_size;
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

int otsardb_recovery_apply_wal_batch_file(const void *batch_data,
                                        size_t batch_size,
                                        const char *database_path) {
    otsardb_wal_batch_view batch;
    if (!database_path ||
        !otsardb_wal_batch_decode(batch_data, batch_size, &batch)) {
        return 0;
    }

    FILE *file = fopen(database_path, "r+b");
    if (!file) file = fopen(database_path, "w+b");
    if (!file) return 0;

    for (uint32_t i = 0; i < batch.frame_count; ++i) {
        uint32_t page_no = 0;
        const unsigned char *page = NULL;
        if (!otsardb_wal_batch_frame(&batch, i, &page_no, &page)) {
            recovery_diag("apply_wal_batch_file: frame %u/%u decode failed", i, batch.frame_count);
            fclose(file);
            return 0;
        }

        uint64_t offset = ((uint64_t)page_no - 1u) * batch.page_size;
#if defined(_WIN32)
        if (offset > INT64_MAX || _fseeki64(file, (__int64)offset, SEEK_SET) != 0) {
#else
        if (offset > INT64_MAX || fseeko(file, (off_t)offset, SEEK_SET) != 0) {
#endif
            recovery_diag("apply_wal_batch_file: seek failed page_no=%u page_size=%u", page_no, batch.page_size);
            fclose(file);
            return 0;
        }

        if (fwrite(page, 1, batch.page_size, file) != batch.page_size) {
            recovery_diag("apply_wal_batch_file: short write page_no=%u size=%u", page_no, batch.page_size);
            fclose(file);
            return 0;
        }
    }

    if (fflush(file) != 0) {
        fclose(file);
        return 0;
    }

    uint64_t final_size = batch.database_size_pages * (uint64_t)batch.page_size;
    if (final_size > INT64_MAX) {
        fclose(file);
        return 0;
    }

    int fd = otsardb_fileno(file);
#if defined(_WIN32)
    if (fd < 0 || otsardb_ftruncate(fd, final_size) != 0) {
#else
    if (fd < 0 || ftruncate(fd, (off_t)final_size) != 0) {
#endif
        fclose(file);
        return 0;
    }

#if !defined(_WIN32)
    if (fsync(fd) != 0) {
        fclose(file);
        return 0;
    }
#endif

    return fclose(file) == 0;
}

static int read_manifest_object(otsardb_object_store *store,
                                const char *key,
                                const char *expected_sha256,
                                otsardb_commit_manifest *out_manifest,
                                otsardb_segment_enc_set *out_enc) {
    otsardb_store_object object = {0};
    if (otsardb_store_get(store, key, &object) != OTSARDB_STORE_OK) return 0;

    char actual_sha256[OTSARDB_SHA256_HEX_SIZE + 1];
    int ok = otsardb_sha256_hex_data(object.data, object.size, actual_sha256) &&
             (!expected_sha256 || expected_sha256[0] == '\0' ||
              strcmp(actual_sha256, expected_sha256) == 0) &&
             otsardb_commit_manifest_decode_json((const char *)object.data,
                                               object.size,
                                               out_manifest) &&
             otsardb_segment_enc_parse_json((const char *)object.data,
                                          object.size,
                                          out_enc);
    otsardb_store_release_object(store, &object);
    return ok;
}

/* AAD = "<absolute object key>\n<commit_id>". malloc'd. */
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

/* Decrypt one encrypted segment envelope into the plaintext .otsseg object.
 * Returns 1 on success (*out owns the plaintext); 0 fails closed (missing
 * cipher or tag verification failure). */
static int decrypt_segment_envelope(const otsardb_cipher *cipher,
                                    const char *absolute_key,
                                    const char *commit_id,
                                    const unsigned char *envelope,
                                    size_t envelope_size,
                                    unsigned char **out,
                                    size_t *out_size) {
    if (!cipher) {
        recovery_diag("segment %s is encrypted (%s) but no OTSARDB_S3_ENC_KEY is "
                      "configured; refusing to decode (fail closed)",
                      absolute_key, OTSARDB_CIPHER_ALG_NAME);
        return 0;
    }
    char *aad = NULL;
    size_t aad_len = 0;
    if (!segment_aad_build(absolute_key, commit_id, &aad, &aad_len)) return 0;
    int ok = otsardb_cipher_decrypt(cipher,
                                  envelope,
                                  envelope_size,
                                  aad,
                                  aad_len,
                                  out,
                                  out_size);
    free(aad);
    if (!ok) {
        recovery_diag("segment %s failed AES-256-GCM tag verification (%s); "
                      "wrong key or tampered object — failing closed",
                      absolute_key, otsardb_cipher_last_error());
    }
    return ok;
}

static int apply_manifest_segments(otsardb_object_store *store,
                                   const char *database_root,
                                   const otsardb_commit_manifest *manifest,
                                   const otsardb_segment_enc_set *enc,
                                   const otsardb_cipher *cipher,
                                   const char *database_path) {
    for (uint32_t i = 0; i < manifest->segment_count; ++i) {
        const otsardb_manifest_segment_ref *ref = &manifest->segments[i];

        /* v3 segment keys are suffixes relative to the database root; legacy
         * v1/v2 flat segment_key is already an absolute object key. */
        char absolute_key[OTSARDB_KEY_MAX + 1];
        const char *object_key = ref->key;
        if (manifest->format_version >= 3) {
            if (!root_join(database_root, ref->key, absolute_key, sizeof(absolute_key))) return 0;
            object_key = absolute_key;
        }

        otsardb_store_object object = {0};
        if (otsardb_store_get(store, object_key, &object) != OTSARDB_STORE_OK) {
            recovery_diag("apply_manifest_segments: get failed key=%s", object_key);
            return 0;
        }

        char actual_sha256[OTSARDB_SHA256_HEX_SIZE + 1];
        otsardb_segment_view segment;
        unsigned char *decompressed = NULL;
        size_t decompressed_size = 0;
        const otsardb_segment_enc_entry *entry =
            enc && enc->present && i < enc->count ? &enc->entries[i] : NULL;
        const int encrypted = entry != NULL && entry->present;

        /* The manifest sha256/size cover the STORED bytes (the envelope for
         * encrypted segments): verified exactly like the plaintext path. */
        int ok = object.size == ref->size &&
                 otsardb_sha256_hex_data(object.data, object.size, actual_sha256) &&
                 strcmp(actual_sha256, ref->sha256) == 0;
        if (!ok) {
            recovery_diag("apply_manifest_segments: verify failed key=%s size=%llu want=%llu sha=%s want_sha=%s",
                          object_key, (unsigned long long)object.size, (unsigned long long)ref->size,
                          actual_sha256[0] ? actual_sha256 : "(n/a)", ref->sha256);
        }

        /* decrypt the envelope (tag-verified, AAD = object
         * key + commit id) before the plaintext segment decode. A wrong or
         * missing key fails closed here — never silently garbage. */
        unsigned char *decrypted = NULL;
        size_t decrypted_size = 0;
        if (ok && encrypted) {
            ok = decrypt_segment_envelope(cipher,
                                          object_key,
                                          manifest->commit_id,
                                          (const unsigned char *)object.data,
                                          object.size,
                                          &decrypted,
                                          &decrypted_size) &&
                 otsardb_segment_decode(decrypted, decrypted_size, &segment);
        } else if (ok) {
            ok = otsardb_segment_decode(object.data, object.size, &segment);
        }
        if (ok) {
            ok = segment.kind == OTSARDB_SEGMENT_WAL_FRAMES &&
                 strcmp(segment.commit_id, manifest->commit_id) == 0;
        }
        if (ok && segment.compressed) {
            ok = otsardb_segment_decompress(&segment, (void **)&decompressed, &decompressed_size) &&
                 otsardb_recovery_apply_wal_batch_file(decompressed,
                                                     decompressed_size,
                                                     database_path);
        } else if (ok) {
            ok = otsardb_recovery_apply_wal_batch_file(segment.payload,
                                                     segment.payload_size,
                                                     database_path);
        }
        free(decompressed);
        free(decrypted);
        otsardb_store_release_object(store, &object);
        if (!ok) return 0;
    }
    return 1;
}

/*
 * Collect the manifest chain keys from head down to stop_generation
 * (inclusive when non-zero), validating every link exactly like recovery:
 * - each manifest object's SHA-256 against the chain link (HEAD or the
 * parent's parent_manifest_sha256);
 * - generation continuity (parent_generation + 1 == generation) and
 * commit_id continuity (parent_commit_id);
 * - database_id/page_size match against metadata;
 * - genesis termination rules (empty parent ids, empty parent hash for
 * v2+).
 *
 * On success the keys are returned newest-first (keys[0] == head) with
 * *out_key_count set and 1 is returned; on failure the partial keys are
 * freed and 0 is returned. No segments are downloaded.
 *
 * factored out of walk_manifest_chain so the delta
 * materialization path validates the chain through the same machinery.
 */
static int collect_chain_keys(otsardb_object_store *store,
                              const char *database_root,
                              const otsardb_db_metadata *metadata,
                              const otsardb_head *head,
                              uint64_t stop_generation,
                              char ***out_keys,
                              size_t *out_key_count) {
    uint64_t expected_generation = head->generation;
    char expected_commit_id[OTSARDB_COMMIT_ID_MAX + 1];
    char current_key[OTSARDB_KEY_MAX + 1];
    char expected_manifest_sha[OTSARDB_MANIFEST_SHA256_HEX_SIZE + 1];
    snprintf(expected_commit_id, sizeof(expected_commit_id), "%s", head->commit_id);
    snprintf(current_key, sizeof(current_key), "%s", head->commit_key);
    snprintf(expected_manifest_sha, sizeof(expected_manifest_sha), "%s", head->commit_sha256);

    char **keys = NULL;
    size_t keys_capacity = 0;
    size_t key_count = 0;

    int ok = 1;
    while (expected_generation > 0) {
        if (key_count >= OTSARDB_RECOVERY_MAX_COMMITS) {
            ok = 0;
            break;
        }
        if (key_count == keys_capacity) {
            size_t next_capacity = keys_capacity ? keys_capacity * 2u : 16u;
            if (next_capacity > OTSARDB_RECOVERY_MAX_COMMITS) next_capacity = OTSARDB_RECOVERY_MAX_COMMITS;
            char **next_keys = (char **)realloc(keys, next_capacity * sizeof(*next_keys));
            if (!next_keys) {
                ok = 0;
                break;
            }
            keys = next_keys;
            keys_capacity = next_capacity;
        }

        otsardb_commit_manifest manifest = {0};
        otsardb_segment_enc_set enc;
        memset(&manifest, 0, sizeof(manifest));
        int read_ok = read_manifest_object(store, current_key, expected_manifest_sha, &manifest, &enc);
        if (!read_ok ||
            manifest.generation != expected_generation ||
            strcmp(manifest.database_id, metadata->database_id) != 0 ||
            strcmp(manifest.commit_id, expected_commit_id) != 0 ||
            manifest.page_size != metadata->page_size ||
            manifest.parent_generation + 1u != manifest.generation) {
            recovery_diag("walk: manifest check failed key=%s want_gen=%llu want_sha=%s read_ok=%d got_gen=%llu got_id=%s",
                          current_key, (unsigned long long)expected_generation,
                          expected_manifest_sha, read_ok,
                          (unsigned long long)manifest.generation, manifest.commit_id);
            otsardb_commit_manifest_free(&manifest);
            ok = 0;
            break;
        }

        keys[key_count] = (char *)malloc(strlen(current_key) + 1u);
        if (!keys[key_count]) {
            otsardb_commit_manifest_free(&manifest);
            ok = 0;
            break;
        }
        strcpy(keys[key_count], current_key);
        ++key_count;

        if (stop_generation != 0 && manifest.generation == stop_generation) {
            otsardb_commit_manifest_free(&manifest);
            break;
        }

        if (manifest.parent_generation == 0) {
            if (manifest.parent_commit_id[0] != '\0' ||
                (manifest.format_version >= 2 && manifest.parent_manifest_sha256[0] != '\0')) {
                ok = 0;
            }
            otsardb_commit_manifest_free(&manifest);
            break;
        }
        if (manifest.parent_commit_id[0] == '\0') {
            otsardb_commit_manifest_free(&manifest);
            ok = 0;
            break;
        }

        expected_generation = manifest.parent_generation;
        snprintf(expected_commit_id, sizeof(expected_commit_id), "%s", manifest.parent_commit_id);
        if (!build_manifest_key(database_root,
                                expected_commit_id,
                                current_key,
                                sizeof(current_key))) {
            otsardb_commit_manifest_free(&manifest);
            ok = 0;
            break;
        }

        if (manifest.format_version >= 2) {
            if (strlen(manifest.parent_manifest_sha256) != OTSARDB_MANIFEST_SHA256_HEX_SIZE) {
                otsardb_commit_manifest_free(&manifest);
                ok = 0;
                break;
            }
            snprintf(expected_manifest_sha,
                     sizeof(expected_manifest_sha),
                     "%s",
                     manifest.parent_manifest_sha256);
        } else {
            /* Legacy manifest v1 has no hash link. The top v1 object is still
             * verified by HEAD and every segment remains content-verified. */
            expected_manifest_sha[0] = '\0';
        }
        otsardb_commit_manifest_free(&manifest);
    }

    if (!ok) {
        for (size_t i = 0; i < key_count; ++i) free(keys[i]);
        free(keys);
        *out_keys = NULL;
        *out_key_count = 0;
        return 0;
    }
    *out_keys = keys;
    *out_key_count = key_count;
    return 1;
}

/*
 * Apply the segments of every manifest whose key is in `keys` (newest-first
 * order) to database_path, oldest first, verifying each object's SHA-256 +
 * size against its chain-authenticated manifest (and decrypting encrypted
 * segments per).
 *
 * factored out of walk_manifest_chain and shared with the
 * delta materialization path, which passes key_count-1 (dropping the OLDEST
 * key — the delta base at generation base_generation) so the cached image's
 * own generations are never re-fetched.
 */
static int apply_manifest_keys(otsardb_object_store *store,
                               const char *database_root,
                               char **keys,
                               size_t key_count,
                               const otsardb_cipher *cipher,
                               const char *database_path) {
    for (size_t i = key_count; i > 0; --i) {
        otsardb_commit_manifest manifest = {0};
        otsardb_segment_enc_set enc;
        if (!read_manifest_object(store, keys[i - 1], NULL, &manifest, &enc) ||
            !apply_manifest_segments(store, database_root, &manifest, &enc, cipher, database_path)) {
            otsardb_commit_manifest_free(&manifest);
            return 0;
        }
        otsardb_commit_manifest_free(&manifest);
    }
    return 1;
}

/*
 * Walk the manifest chain from HEAD down to genesis, validating the hash
 * links/generation continuity. When apply_segments is non-zero, each manifest
 * is also applied (oldest first) to database_path. The walk is deterministic
 * on immutable objects, so validating first and applying second touches the
 * same manifests in the same order.
 *
 * stop_generation > 0 stops the walk at that generation (inclusive); the
 * manifest at stop_generation is NOT applied. Use 0 for full-chain.
 */
static int walk_manifest_chain(otsardb_object_store *store,
                               const char *database_root,
                               const otsardb_db_metadata *metadata,
                               const otsardb_head *head,
                               const char *database_path,
                               int apply_segments,
                               uint64_t stop_generation,
                               const otsardb_cipher *cipher) {
    char **keys = NULL;
    size_t key_count = 0;
    if (!collect_chain_keys(store, database_root, metadata, head,
                            stop_generation, &keys, &key_count)) {
        return 0;
    }

    int ok = 1;
    if (stop_generation == 0 && (key_count == 0 || apply_segments)) {
        /* The chain must terminate at genesis. Keys are collected newest-first
         * (keys[0] is HEAD), so the oldest entry is keys[key_count - 1]. */
        otsardb_commit_manifest genesis;
        otsardb_segment_enc_set genesis_enc;
        int genesis_ok = key_count > 0 &&
                         read_manifest_object(store, keys[key_count - 1], NULL, &genesis, &genesis_enc) &&
                         genesis.parent_generation == 0;
        if (key_count > 0) otsardb_commit_manifest_free(&genesis);
        if (!genesis_ok) ok = 0;
    }

    if (ok && apply_segments) {
        ok = apply_manifest_keys(store, database_root, keys, key_count, cipher, database_path);
    }

    for (size_t i = 0; i < key_count; ++i) free(keys[i]);
    free(keys);
    return ok;
}

int otsardb_recovery_walk_chain(otsardb_object_store *store,
                              const char *database_root,
                              const otsardb_db_metadata *metadata,
                              const otsardb_head *head,
                              uint64_t stop_generation,
                              otsardb_chain_visitor visitor,
                              void *visitor_ctx) {
    if (!store || !database_root || !metadata || !head) return 0;

    uint64_t expected_generation = head->generation;
    char expected_commit_id[OTSARDB_COMMIT_ID_MAX + 1];
    char current_key[OTSARDB_KEY_MAX + 1];
    char expected_manifest_sha[OTSARDB_MANIFEST_SHA256_HEX_SIZE + 1];
    snprintf(expected_commit_id, sizeof(expected_commit_id), "%s", head->commit_id);
    snprintf(current_key, sizeof(current_key), "%s", head->commit_key);
    snprintf(expected_manifest_sha, sizeof(expected_manifest_sha), "%s", head->commit_sha256);

    /* Keys are collected newest-first (keys[0] is HEAD) so the oldest entry
     * is keys[key_count - 1] (used for the genesis re-check below). */
    char **keys = NULL;
    size_t keys_capacity = 0;
    size_t key_count = 0;

    int ok = 1;
    while (expected_generation > 0) {
        if (key_count >= OTSARDB_RECOVERY_MAX_COMMITS) {
            ok = 0;
            break;
        }
        if (key_count == keys_capacity) {
            size_t next_capacity = keys_capacity ? keys_capacity * 2u : 16u;
            if (next_capacity > OTSARDB_RECOVERY_MAX_COMMITS) next_capacity = OTSARDB_RECOVERY_MAX_COMMITS;
            char **next_keys = (char **)realloc(keys, next_capacity * sizeof(*next_keys));
            if (!next_keys) {
                ok = 0;
                break;
            }
            keys = next_keys;
            keys_capacity = next_capacity;
        }

        otsardb_commit_manifest manifest = {0};
        otsardb_segment_enc_set enc;
        memset(&manifest, 0, sizeof(manifest));
            if (!read_manifest_object(store, current_key, expected_manifest_sha, &manifest, &enc) ||
                manifest.generation != expected_generation ||
                strcmp(manifest.database_id, metadata->database_id) != 0 ||
                strcmp(manifest.commit_id, expected_commit_id) != 0 ||
                manifest.page_size != metadata->page_size ||
                manifest.parent_generation + 1u != manifest.generation) {
                otsardb_commit_manifest_free(&manifest);
                ok = 0;
                break;
            }

        if (visitor && !visitor(visitor_ctx, current_key, expected_manifest_sha, &manifest, &enc)) {
            otsardb_commit_manifest_free(&manifest);
            ok = 0;
            break;
        }

        keys[key_count] = (char *)malloc(strlen(current_key) + 1u);
        if (!keys[key_count]) {
            otsardb_commit_manifest_free(&manifest);
            ok = 0;
            break;
        }
        strcpy(keys[key_count], current_key);
        ++key_count;

        if (stop_generation != 0 && manifest.generation == stop_generation) {
            otsardb_commit_manifest_free(&manifest);
            break;
        }

        if (manifest.parent_generation == 0) {
            if (manifest.parent_commit_id[0] != '\0' ||
                (manifest.format_version >= 2 && manifest.parent_manifest_sha256[0] != '\0')) {
                ok = 0;
            }
            otsardb_commit_manifest_free(&manifest);
            break;
        }
        if (manifest.parent_commit_id[0] == '\0') {
            otsardb_commit_manifest_free(&manifest);
            ok = 0;
            break;
        }

        expected_generation = manifest.parent_generation;
        snprintf(expected_commit_id, sizeof(expected_commit_id), "%s", manifest.parent_commit_id);
        if (!build_manifest_key(database_root,
                                expected_commit_id,
                                current_key,
                                sizeof(current_key))) {
            otsardb_commit_manifest_free(&manifest);
            ok = 0;
            break;
        }

        if (manifest.format_version >= 2) {
            if (strlen(manifest.parent_manifest_sha256) != OTSARDB_MANIFEST_SHA256_HEX_SIZE) {
                otsardb_commit_manifest_free(&manifest);
                ok = 0;
                break;
            }
            snprintf(expected_manifest_sha,
                     sizeof(expected_manifest_sha),
                     "%s",
                     manifest.parent_manifest_sha256);
        } else {
            /* Legacy manifest v1 has no hash link. The top v1 object is still
             * verified by HEAD and every segment remains content-verified. */
            expected_manifest_sha[0] = '\0';
        }
        otsardb_commit_manifest_free(&manifest);
    }

    if (ok && stop_generation == 0) {
        /* The chain must terminate at genesis: re-read the oldest manifest
         * and verify it really has no parent (defence in depth, matching
         * recovery's own genesis re-check). */
        otsardb_commit_manifest genesis;
        otsardb_segment_enc_set genesis_enc;
        int genesis_ok = key_count > 0 &&
                         read_manifest_object(store, keys[key_count - 1], NULL, &genesis, &genesis_enc) &&
                         genesis.parent_generation == 0;
        if (key_count > 0) otsardb_commit_manifest_free(&genesis);
        if (!genesis_ok) ok = 0;
    }

    for (size_t i = 0; i < key_count; ++i) free(keys[i]);
    free(keys);
    return ok;
}

int otsardb_recovery_apply_checkpoint_pages(otsardb_object_store *store,
                                          const char *database_root,
                                          const otsardb_checkpoint_manifest *cp,
                                          const char *database_path) {
    for (uint32_t i = 0; i < cp->page_group_count; ++i) {
        const otsardb_checkpoint_page_group_ref *ref = &cp->page_groups[i];

        char key[OTSARDB_KEY_MAX + 1];
        if (!root_join(database_root, ref->key, key, sizeof(key))) return 0;

        otsardb_store_object object = {0};
        if (otsardb_store_get(store, key, &object) != OTSARDB_STORE_OK) return 0;

        char actual[OTSARDB_SHA256_HEX_SIZE + 1];
        int ok = otsardb_sha256_hex_data(object.data, object.size, actual) &&
                 strcmp(actual, ref->sha256) == 0 &&
                 object.size == ref->size;
        if (!ok) { otsardb_store_release_object(store, &object); return 0; }

        otsardb_checkpoint_page_view view;
        if (!otsardb_checkpoint_page_decode(object.data, object.size, &view)) {
            otsardb_store_release_object(store, &object); return 0;
        }

        FILE *file = fopen(database_path, "r+b");
        if (!file) file = fopen(database_path, "w+b");
        if (!file) { otsardb_store_release_object(store, &object); return 0; }

        for (uint32_t j = 0; j < view.page_count; ++j) {
            uint32_t page_no = 0;
            int compressed = 0;
            const unsigned char *page_data = NULL;
            size_t stored_size = 0;
            if (!otsardb_checkpoint_page_frame(&view, j, &page_no, &compressed,
                                             &page_data, &stored_size)) {
                fclose(file); otsardb_store_release_object(store, &object); return 0;
            }
            /* (failure-mode audit finding B6): fail closed on
             * any record this tree's encoder never produces. A compressed
             * record (LZ4 flag set) would be written verbatim as compressed
             * garbage, and a stored size that is not exactly one page would
             * make the fwrite below read past the record buffer (or write a
             * short page). The encoder always writes flags=0 with
             * stored_size == page_size. */
            if (compressed || stored_size != view.page_size) {
                fclose(file); otsardb_store_release_object(store, &object); return 0;
            }

            uint64_t offset = ((uint64_t)page_no - 1u) * view.page_size;
#if defined(_WIN32)
            if (offset > INT64_MAX || _fseeki64(file, (__int64)offset, SEEK_SET) != 0) {
#else
            if (offset > INT64_MAX || fseeko(file, (off_t)offset, SEEK_SET) != 0) {
#endif
                fclose(file); otsardb_store_release_object(store, &object); return 0;
            }
            if (fwrite(page_data, 1, view.page_size, file) != view.page_size) {
                fclose(file); otsardb_store_release_object(store, &object); return 0;
            }
        }
        fclose(file);
        otsardb_store_release_object(store, &object);
    }
    return 1;
}

/* 64-bit file size (the images can exceed the 32-bit LONG range on
 * Windows, so plain ftell is not sufficient). */
static int file_size_u64(const char *path, uint64_t *out) {
    if (!path || !out) return 0;
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
#if defined(_WIN32)
    int ok = _fseeki64(file, (__int64)0, SEEK_END) == 0;
    if (ok) {
        __int64 pos = _ftelli64(file);
        ok = pos >= 0 && (uint64_t)pos <= UINT64_MAX;
        if (ok) *out = (uint64_t)pos;
    }
#else
    int ok = fseeko(file, (off_t)0, SEEK_END) == 0;
    if (ok) {
        off_t pos = ftello(file);
        ok = pos >= 0;
        if (ok) *out = (uint64_t)pos;
    }
#endif
    fclose(file);
    return ok;
}

/* (D2/B3): verify a LOCAL database image against a
 * chain-authenticated manifest's whole-file post-apply checksum. Returns 1
 * when the manifest carries no checksum (the 0063/legacy trade-off, same as
 * validate_head_checksum) or the file matches; 0 on mismatch. */
static int validate_file_against_manifest_checksum(const char *path,
                                                   uint32_t page_size,
                                                   uint64_t database_size_pages,
                                                   const char *expected_hex) {
    if (expected_hex[0] == '\0') return 1; /* chain predates checksums */
    uint64_t expected = 0;
    if (!otsardb_checksum_parse(expected_hex, &expected)) return 0;

    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    uint64_t actual = otsardb_checksum_file(file, page_size, database_size_pages);
    fclose(file);
    return actual == expected;
}

/* When the HEAD manifest carries a rolling post-apply checksum, verify the
 * materialised database file matches it. Returns 1 when the checksum is
 * absent OR matches; 0 on mismatch (divergent/corrupted state). */
static int validate_head_checksum(otsardb_object_store *store,
                                  const char *database_root,
                                  const otsardb_head *head,
                                  const char *database_path) {
    (void)database_root;
    const char *manifest_key = head->commit_key;

    char actual_sha256[OTSARDB_MANIFEST_SHA256_HEX_SIZE + 1];
    otsardb_store_object object = {0};
    if (otsardb_store_get(store, manifest_key, &object) != OTSARDB_STORE_OK) return 0;

    /* (audit B4): authenticate the manifest read. The object
     * at head.commit_key must hash to head.commit_sha256 before it is
     * decoded and its checksum trusted. The chain walk already authenticated
     * this manifest, but this is a SECOND independent GET — a manifest
     * swapped between the walk and this read (TOCTOU) would otherwise skip
     * the whole-file checksum gate. Fail closed on mismatch. */
    otsardb_commit_manifest manifest = {0};
    int ok = otsardb_sha256_hex_data(object.data, object.size, actual_sha256) &&
             strcmp(actual_sha256, head->commit_sha256) == 0 &&
             otsardb_commit_manifest_decode_json((const char *)object.data,
                                                object.size,
                                                &manifest);
    otsardb_store_release_object(store, &object);
    if (!ok) {
        recovery_diag("validate_head_checksum: manifest auth failed key=%s",
                      manifest_key);
        return 0;
    }

    ok = validate_file_against_manifest_checksum(database_path,
                                                 manifest.page_size,
                                                 manifest.database_size_pages,
                                                 manifest.post_apply_checksum);
    if (!ok) {
        recovery_diag("validate_head_checksum: MISMATCH expected=%s key=%s",
                      manifest.post_apply_checksum[0] ? manifest.post_apply_checksum
                                                      : "(absent)",
                      manifest_key);
    }
    otsardb_commit_manifest_free(&manifest);
    return ok;
}

/* ---------------------------------------------------------------------------
 * (failure-mode audit finding C2): CHECKPOINT.json pointer
 * loss fallback.
 *
 * GC (OTSARDB_GC=1, default keep window 0) deletes the history strictly
 * below the checkpoint's covered generation. If the CHECKPOINT.json POINTER
 * is then lost (S3 lifecycle policy, manual cleanup, partial restore, or the
 * B3-amplified deletion path), recovery's full-chain fallback hits the
 * missing manifests/segments and the open fails (availability loss —
 * fail-closed, not silent). The checkpoint MANIFEST itself survives GC and
 * is content-addressed (its object key == sha256 of its JSON), so it can be
 * rediscovered by listing the checkpoints/ prefix, re-verified by its own
 * key hash, and re-bound to the authenticated chain exactly like the pointer
 * path (B2/B3) before its pages are applied.
 *
 * Only a checkpoint whose covered generation still binds to the current
 * chain is usable (a swept checkpoint's covered manifest is gone, so its
 * binding fails); the highest bound covered generation is used.
 */
typedef struct cp_discovery_ctx {
    otsardb_object_store *store;
    const char *database_root;
    const otsardb_db_metadata *metadata;
    const otsardb_head *head;
    /* Candidate checkpoint-manifest logical keys, collected during the list
     * pass. The S3 list iterator holds the store's request mutex, so the
     * callback must NOT perform any store operation (a nested GET would
     * deadlock/abort); the keys are evaluated AFTER the list completes. */
    char **keys;
    size_t key_count;
    size_t key_capacity;
    otsardb_checkpoint_manifest best;
    int best_set;
    uint64_t best_covered;
} cp_discovery_ctx;

/* A checkpoint manifest logical key is "<64 hex>.json" with no '/' (the
 * physical key is "checkpoints/<sha>.json", sha == sha256 of the JSON). */
static int cp_manifest_list_key_valid(const char *key) {
    size_t len = strlen(key);
    if (len != 64u + 5u || strcmp(key + 64u, ".json") != 0) return 0;
    for (size_t i = 0; i < 64u; ++i) {
        char c = key[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) {
            return 0;
        }
    }
    return 1;
}

/* List pass: validate the logical key shape and collect it. No store
 * operations here — the S3 list iterator holds the request mutex. */
static int cp_discovery_collect(const char *logical_key, void *opaque) {
    cp_discovery_ctx *ctx = (cp_discovery_ctx *)opaque;
    if (!cp_manifest_list_key_valid(logical_key)) {
        recovery_diag("discovery: candidate key invalid '%s'", logical_key);
        return 0;
    }
    if (ctx->key_count == ctx->key_capacity) {
        size_t next_capacity = ctx->key_capacity ? ctx->key_capacity * 2u : 16u;
        char **next = (char **)realloc(ctx->keys, next_capacity * sizeof(*next));
        if (!next) return 1; /* abort the listing on OOM */
        ctx->keys = next;
        ctx->key_capacity = next_capacity;
    }
    char *copy = (char *)malloc(strlen(logical_key) + 1u);
    if (!copy) return 1;
    strcpy(copy, logical_key);
    ctx->keys[ctx->key_count++] = copy;
    return 0;
}

static void cp_discovery_free_keys(cp_discovery_ctx *ctx) {
    for (size_t i = 0; i < ctx->key_count; ++i) free(ctx->keys[i]);
    free(ctx->keys);
    ctx->keys = NULL;
    ctx->key_count = 0;
    ctx->key_capacity = 0;
}

/* Evaluate one collected checkpoint manifest: fetch it, re-verify it by its
 * own key hash, decode, identity-check and chain-bind (exactly like the
 * pointer path's B2/B3 gates). */
static int cp_discovery_evaluate_one(cp_discovery_ctx *ctx,
                                     const char *logical_key) {
    char suffix[256];
    if (snprintf(suffix, sizeof(suffix), "checkpoints/%s", logical_key) >=
        (int)sizeof(suffix)) {
        return 0;
    }
    char key[OTSARDB_KEY_MAX + 1];
    if (!root_join(ctx->database_root, suffix, key, sizeof(key))) return 0;

    otsardb_store_object obj = {0};
    if (otsardb_store_get(ctx->store, key, &obj) != OTSARDB_STORE_OK) {
        recovery_diag("discovery: candidate get failed %s", key);
        return 0;
    }

    /* The manifest key is content-addressed: sha256(bytes) == the sha in
     * the key — the pointer's checkpoint_sha256 role is re-established
     * without the pointer object. */
    char actual[OTSARDB_SHA256_HEX_SIZE + 1];
    int ok = otsardb_sha256_hex_data(obj.data, obj.size, actual) &&
             strncmp(actual, logical_key, 64) == 0;
    if (!ok) {
        recovery_diag("discovery: candidate sha mismatch key=%s actual=%.*s",
                      logical_key, (int)OTSARDB_SHA256_HEX_SIZE, actual);
        otsardb_store_release_object(ctx->store, &obj);
        return 0;
    }

    /* the manifest, covered manifest and enc set are large
     * (128-segment/128-page-group arrays); heap-allocate them so a deep
     * session->checkpoint->recovery call chain does not exhaust the thread
     * stack. */
    otsardb_checkpoint_manifest *cp =
        (otsardb_checkpoint_manifest *)calloc(1, sizeof(*cp));
    if (!cp) {
        otsardb_store_release_object(ctx->store, &obj);
        return 0;
    }
    ok = otsardb_checkpoint_manifest_decode((const char *)obj.data, obj.size, cp);
    if (!ok) {
        recovery_diag("discovery: candidate decode failed");
        free(cp);
        otsardb_store_release_object(ctx->store, &obj);
        return 0;
    }
    otsardb_store_release_object(ctx->store, &obj);

    /* Identity + range gates (mirror the pointer path's B2 checks). */
    ok = cp->covered_generation > 0 &&
         cp->covered_generation <= ctx->head->generation &&
         strcmp(cp->database_id, ctx->metadata->database_id) == 0 &&
         cp->page_size == ctx->metadata->page_size &&
         cp->database_size_pages > 0 &&
         cp->base_commit_id[0] != '\0' &&
         cp->base_manifest_sha256[0] != '\0';
    if (!ok) {
        recovery_diag("discovery: candidate identity failed cpg=%llu cp_db=%s cp_page=%u want_db=%s want_page=%u",
                      (unsigned long long)cp->covered_generation,
                      cp->database_id, cp->page_size,
                      ctx->metadata->database_id, ctx->metadata->page_size);
        free(cp);
        return 0;
    }

    /* Chain-bind: the covered generation's chain-authenticated manifest
     * must be this checkpoint's base (commit_id + base_manifest_sha256). */
    char **cp_keys = NULL;
    size_t cp_key_count = 0;
    otsardb_commit_manifest *covered =
        (otsardb_commit_manifest *)calloc(1, sizeof(*covered));
    otsardb_segment_enc_set *covered_enc =
        (otsardb_segment_enc_set *)calloc(1, sizeof(*covered_enc));
    int bound = 0;
    if (covered && covered_enc) {
        bound = collect_chain_keys(ctx->store, ctx->database_root,
                                   ctx->metadata, ctx->head,
                                   cp->covered_generation,
                                   &cp_keys, &cp_key_count) &&
                cp_key_count > 0;
        if (bound) {
            bound =
                read_manifest_object(ctx->store, cp_keys[cp_key_count - 1],
                                     NULL, covered, covered_enc) &&
                covered->generation == cp->covered_generation &&
                strcmp(covered->commit_id, cp->base_commit_id) == 0;
        }
        if (bound) {
            otsardb_store_object covered_obj = {0};
            if (otsardb_store_get(ctx->store, cp_keys[cp_key_count - 1],
                                  &covered_obj) == OTSARDB_STORE_OK) {
                char covered_sha[OTSARDB_SHA256_HEX_SIZE + 1];
                bound = otsardb_sha256_hex_data(covered_obj.data,
                                                covered_obj.size,
                                                covered_sha) &&
                        strcmp(covered_sha, cp->base_manifest_sha256) == 0;
                otsardb_store_release_object(ctx->store, &covered_obj);
            } else {
                bound = 0;
            }
        }
    }
    if (cp_keys) {
        for (size_t i = 0; i < cp_key_count; ++i) free(cp_keys[i]);
        free(cp_keys);
    }
    if (covered) otsardb_commit_manifest_free(covered);
    free(covered);
    free(covered_enc);
    if (!bound) {
        recovery_diag("discovery: candidate chain-bind failed cpg=%llu base=%s",
                      (unsigned long long)cp->covered_generation,
                      cp->base_commit_id);
    }
    if (bound && (!ctx->best_set ||
                  cp->covered_generation > ctx->best_covered)) {
        ctx->best = *cp;
        ctx->best_covered = cp->covered_generation;
        ctx->best_set = 1;
    }
    free(cp);
    return 0;
}

/* Materialise a discovered checkpoint: apply its pages, re-verify the base
 * (size + checksum when the covered manifest carries one), replay the
 * generations above the covered one and validate the head checksum — the
 * same sequence as the pointer path. */
static int recovery_apply_discovered_checkpoint(cp_discovery_ctx *ctx,
                                                const otsardb_checkpoint_manifest *cp,
                                                const char *database_path,
                                                const otsardb_cipher *cipher) {
    otsardb_object_store *store = ctx->store;
    const char *database_root = ctx->database_root;
    const otsardb_head *head = ctx->head;

    char **cp_keys = NULL;
    size_t cp_key_count = 0;
    if (!collect_chain_keys(store, database_root, ctx->metadata, head,
                            cp->covered_generation, &cp_keys, &cp_key_count) ||
        cp_key_count == 0) {
        if (cp_keys) {
            for (size_t i = 0; i < cp_key_count; ++i) free(cp_keys[i]);
            free(cp_keys);
        }
        return 0;
    }

    /* heap-allocate the covered manifest / enc set (large
     * 128-segment arrays) to keep the deep session->checkpoint->recovery
     * call chain within the thread stack budget. */
    otsardb_commit_manifest *covered =
        (otsardb_commit_manifest *)calloc(1, sizeof(*covered));
    otsardb_segment_enc_set *covered_enc =
        (otsardb_segment_enc_set *)calloc(1, sizeof(*covered_enc));
    int bound = covered && covered_enc &&
                read_manifest_object(store, cp_keys[cp_key_count - 1],
                                     NULL, covered, covered_enc);
    remove(database_path);
    if (!bound || !otsardb_recovery_apply_checkpoint_pages(store, database_root,
                                                           cp, database_path)) {
        remove(database_path);
        if (covered) otsardb_commit_manifest_free(covered);
        free(covered);
        free(covered_enc);
        for (size_t i = 0; i < cp_key_count; ++i) free(cp_keys[i]);
        free(cp_keys);
        return 0;
    }

    /* B3-style re-validation of the materialised base image against the
     * covered generation's chain-authenticated manifest. */
    int cp_apply = 1;
    {
        uint64_t want_size = covered->database_size_pages * (uint64_t)covered->page_size;
        uint64_t got_size = 0;
        if (!file_size_u64(database_path, &got_size) || got_size != want_size) {
            recovery_diag("restore_store: discovered checkpoint base size mismatch "
                          "got=%llu want=%llu covered_gen=%llu",
                          (unsigned long long)got_size,
                          (unsigned long long)want_size,
                          (unsigned long long)cp->covered_generation);
            cp_apply = 0;
        }
    }
    if (cp_apply && !validate_file_against_manifest_checksum(
                        database_path, covered->page_size,
                        covered->database_size_pages,
                        covered->post_apply_checksum)) {
        recovery_diag("restore_store: discovered checkpoint base checksum mismatch "
                      "covered_gen=%llu",
                      (unsigned long long)cp->covered_generation);
        cp_apply = 0;
    }
    if (cp_apply) {
        size_t apply_count = cp_key_count;
        if (covered->post_apply_checksum[0] != '\0') apply_count = cp_key_count - 1;
        cp_apply = apply_manifest_keys(store, database_root, cp_keys,
                                       apply_count, cipher, database_path) &&
                   validate_head_checksum(store, database_root, head, database_path);
    }
    if (!cp_apply) {
        remove(database_path);
        recovery_diag("restore_store: discovered checkpoint path failed covered_gen=%llu",
                      (unsigned long long)cp->covered_generation);
    }
    otsardb_commit_manifest_free(covered);
    free(covered);
    free(covered_enc);
    for (size_t i = 0; i < cp_key_count; ++i) free(cp_keys[i]);
    free(cp_keys);
    return cp_apply;
}

static int restore_from_discovered_checkpoint(otsardb_object_store *store,
                                              const char *database_root,
                                              const otsardb_db_metadata *metadata,
                                              const otsardb_head *head,
                                              const char *database_path,
                                              const otsardb_cipher *cipher) {
    if (!store || !database_root || !metadata || !head) return 0;

    char checkpoints_prefix[OTSARDB_KEY_MAX + 1];
    if (!root_join(database_root, "checkpoints/", checkpoints_prefix,
                   sizeof(checkpoints_prefix))) {
        return 0;
    }

    cp_discovery_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.store = store;
    ctx.database_root = database_root;
    ctx.metadata = metadata;
    ctx.head = head;
    recovery_diag("discovery: listing %s", checkpoints_prefix);
    if (otsardb_store_list(store, checkpoints_prefix,
                           cp_discovery_collect, &ctx) != OTSARDB_STORE_OK) {
        cp_discovery_free_keys(&ctx);
        recovery_diag("discovery: list failed");
        return 0;
    }
    /* Evaluate each collected candidate AFTER the list has released the
     * store's request mutex (the list iterator must not nest store calls). */
    for (size_t i = 0; i < ctx.key_count; ++i) {
        cp_discovery_evaluate_one(&ctx, ctx.keys[i]);
    }
    cp_discovery_free_keys(&ctx);
    if (!ctx.best_set) return 0;

    return recovery_apply_discovered_checkpoint(&ctx, &ctx.best,
                                                database_path, cipher);
}

int otsardb_recovery_restore_store_ex(otsardb_object_store *store,
                                    const char *database_root,
                                    const char *database_path,
                                    const otsardb_cipher *cipher) {
    if (!store || !database_root || !database_path || database_root[0] == '\0') {
        return 0;
    }

    otsardb_db_metadata metadata;
    if (otsardb_db_metadata_read(store, database_root, &metadata) != OTSARDB_METADATA_OK) {
        recovery_diag("restore_store: metadata read failed root=%s", database_root);
        return 0;
    }

    otsardb_head head;
    char *head_etag = NULL;
    if (otsardb_journal_read_head(store, database_root, &head, &head_etag) != OTSARDB_JOURNAL_OK) {
        free(head_etag);
        recovery_diag("restore_store: head read failed root=%s", database_root);
        return 0;
    }
    free(head_etag);
    if (strcmp(head.database_id, metadata.database_id) != 0 || head.generation == 0 ||
        head.generation > OTSARDB_RECOVERY_MAX_COMMITS ||
        strlen(head.commit_sha256) != OTSARDB_MANIFEST_SHA256_HEX_SIZE) {
        recovery_diag("restore_store: head validation failed gen=%llu dbid=%s sha_len=%zu",
                      (unsigned long long)head.generation, head.database_id,
                      strlen(head.commit_sha256));
        return 0;
    }

    /* Attempt checkpoint-accelerated recovery: materialise the checkpoint
     * pages, then apply only the manifest segments newer than it.
     *
     * (B2): the checkpoint is no longer trusted on the
     * strength of its pointer hash alone. Before its pages are applied the
     * manifest identity is validated exactly like the lazy path does
     * (lazy_hydration.c): database_id and page_size must match the
     * metadata, database_size_pages must be non-zero, the manifest's
     * covered_generation must equal the pointer's, the pointer's
     * database_id must match, and the chain manifest at the covered
     * generation (authenticated by the hash-linked walk) must carry the
     * checkpoint's base_commit_id and hash to base_manifest_sha256 — a
     * stale/foreign checkpoint from another database root or another chain
     * state can no longer apply its pages over this chain.
     *
     * (B3): the checkpoint base image is RE-VALIDATED
     * against the covered generation's chain-authenticated manifest before
     * any newer segments are replayed: the file size must equal
     * database_size_pages * page_size, and when the covered manifest
     * carries a post-apply checksum the file must match it. A checkpoint
     * whose covered_generation was inflated by a concurrent writer (pages
     * at G, claim G') is detected here and fails closed to the full chain
     * walk — it can no longer serve a wrong image (nor let GC behind the
     * inflated coverage delete generations the fallback needs). */
    otsardb_checkpoint_pointer cp_ptr;
    char *cp_etag = NULL;
    if (otsardb_checkpoint_ptr_read(store, database_root, &cp_ptr, &cp_etag) ==
            OTSARDB_CHECKPOINT_PTR_OK &&
        cp_ptr.covered_generation > 0 &&
        cp_ptr.covered_generation <= head.generation &&
        cp_ptr.checkpoint_key[0] != '\0' &&
        strcmp(cp_ptr.database_id, metadata.database_id) == 0) {
        free(cp_etag);

        otsardb_store_object cp_obj = {0};
        if (otsardb_store_get(store, cp_ptr.checkpoint_key, &cp_obj) == OTSARDB_STORE_OK) {
            char cp_sha[OTSARDB_CHECKPOINT_SHA256_HEX_SIZE + 1];
            otsardb_checkpoint_manifest cp;
            if (otsardb_sha256_hex_data(cp_obj.data, cp_obj.size, cp_sha) &&
                strcmp(cp_sha, cp_ptr.checkpoint_sha256) == 0 &&
                otsardb_checkpoint_manifest_decode((const char *)cp_obj.data,
                                                 cp_obj.size, &cp)) {
                otsardb_store_release_object(store, &cp_obj);

                    /* B2: checkpoint identity vs metadata + pointer + chain. */
                    int cp_id_ok =
                        cp.covered_generation == cp_ptr.covered_generation &&
                        cp.covered_generation > 0 &&
                        cp.covered_generation <= head.generation &&
                        strcmp(cp.database_id, metadata.database_id) == 0 &&
                        cp.page_size == metadata.page_size &&
                        cp.database_size_pages > 0 &&
                        cp.base_commit_id[0] != '\0' &&
                        cp.base_manifest_sha256[0] != '\0';
                    if (!cp_id_ok) {
                        recovery_diag("restore_store: checkpoint identity validation failed "
                                      "cpg=%llu cp_db=%s cp_page=%u want_db=%s want_page=%u key=%s",
                                      (unsigned long long)cp.covered_generation,
                                      cp.database_id, cp.page_size,
                                      metadata.database_id, metadata.page_size,
                                      cp_ptr.checkpoint_key);
                    }

                char **cp_keys = NULL;
                size_t cp_key_count = 0;
                int cp_bound = 0;
                otsardb_commit_manifest covered = {0};
                otsardb_segment_enc_set covered_enc;
                if (cp_id_ok &&
                    collect_chain_keys(store, database_root, &metadata, &head,
                                       cp.covered_generation,
                                       &cp_keys, &cp_key_count) &&
                    cp_key_count > 0) {
                    /* The oldest collected key is the manifest at the
                     * covered generation. It is already chain-authenticated;
                     * bind it to the checkpoint by commit_id + SHA-256. */
                    cp_bound =
                        read_manifest_object(store, cp_keys[cp_key_count - 1],
                                             NULL, &covered, &covered_enc) &&
                        covered.generation == cp.covered_generation &&
                        strcmp(covered.commit_id, cp.base_commit_id) == 0;
                    if (cp_bound) {
                        otsardb_store_object covered_obj = {0};
                        if (otsardb_store_get(store, cp_keys[cp_key_count - 1],
                                              &covered_obj) == OTSARDB_STORE_OK) {
                            char covered_sha[OTSARDB_SHA256_HEX_SIZE + 1];
                            cp_bound =
                                otsardb_sha256_hex_data(covered_obj.data,
                                                        covered_obj.size,
                                                        covered_sha) &&
                                strcmp(covered_sha, cp.base_manifest_sha256) == 0;
                            otsardb_store_release_object(store, &covered_obj);
                        } else {
                            cp_bound = 0;
                        }
                    }
                }

                if (cp_bound) {
                    remove(database_path);
                    int cp_apply =
                        otsardb_recovery_apply_checkpoint_pages(
                            store, database_root, &cp, database_path);

                    /* B3: re-validate the materialised base image against
                     * the covered generation's manifest before replaying
                     * anything above it. Size is always checked; the
                     * rolling checksum when the covered manifest carries
                     * one (an absent checksum keeps the documented
                     * checksum-less residual). */
                    if (cp_apply) {
                        uint64_t want_size =
                            covered.database_size_pages * (uint64_t)covered.page_size;
                        uint64_t got_size = 0;
                        if (!file_size_u64(database_path, &got_size) ||
                            got_size != want_size) {
                            recovery_diag("restore_store: checkpoint base size mismatch "
                                          "got=%llu want=%llu key=%s",
                                          (unsigned long long)got_size,
                                          (unsigned long long)want_size,
                                          cp_ptr.checkpoint_key);
                            cp_apply = 0;
                        }
                    }
                    if (cp_apply &&
                        !validate_file_against_manifest_checksum(
                            database_path, covered.page_size,
                            covered.database_size_pages,
                            covered.post_apply_checksum)) {
                        recovery_diag("restore_store: checkpoint base checksum mismatch "
                                      "gen=%llu key=%s",
                                      (unsigned long long)cp.covered_generation,
                                      cp_ptr.checkpoint_key);
                        cp_apply = 0;
                    }
                    if (cp_apply) {
                        /* Replay only the generations ABOVE the covered one:
                         * the (verified) checkpoint base already contains the
                         * covered state, so its own segments are never
                         * re-fetched. When the covered manifest carried no
                         * checksum the base was only size-verified; for that
                         * residual case the covered generation's segments are
                         * ALSO re-applied (idempotent full-page frames) so a
                         * stale base self-heals exactly as the pre-0135 path
                         * did. */
                        size_t apply_count = cp_key_count;
                        if (covered.post_apply_checksum[0] != '\0') {
                            apply_count = cp_key_count - 1;
                        }
                        cp_apply =
                            apply_manifest_keys(store, database_root, cp_keys,
                                                apply_count, cipher,
                                                database_path) &&
                            validate_head_checksum(store, database_root, &head,
                                                   database_path);
                    }
                    if (cp_apply) {
                        for (size_t i = 0; i < cp_key_count; ++i) free(cp_keys[i]);
                        free(cp_keys);
                        otsardb_commit_manifest_free(&covered);
                        return 1;
                    }
                    recovery_diag("restore_store: checkpoint path failed gen=%llu cpg=%llu key=%s",
                                  (unsigned long long)head.generation,
                                  (unsigned long long)cp_ptr.covered_generation,
                                  cp_ptr.checkpoint_key);
                    remove(database_path);
                }
                if (cp_keys) {
                    for (size_t i = 0; i < cp_key_count; ++i) free(cp_keys[i]);
                    free(cp_keys);
                }
                if (!cp_bound && cp_id_ok) {
                    recovery_diag("restore_store: checkpoint chain binding failed "
                                  "cpg=%llu base_commit=%s key=%s",
                                  (unsigned long long)cp.covered_generation,
                                  cp.base_commit_id,
                                  cp_ptr.checkpoint_key);
                }
                otsardb_commit_manifest_free(&covered);
            } else {
                otsardb_store_release_object(store, &cp_obj);
            }
        }
    } else {
        free(cp_etag);
    }

    /* (C2): CHECKPOINT.json pointer loss fallback. When the
     * pointer path did not restore (pointer absent, or its checkpoint failed
     * validation) but the checkpoint OBJECTS survive, rediscover the
     * checkpoint manifest from the content-addressed checkpoints/ prefix,
     * re-verify it and re-bind it to the chain before applying its pages.
     * Without this, a lost pointer plus swept below-covered history makes
     * the full-chain fallback below fail (availability loss). */
    if (restore_from_discovered_checkpoint(store, database_root, &metadata, &head,
                                           database_path, cipher)) {
        return 1;
    }

    /* Fallback: full manifest-chain recovery. */
    if (!walk_manifest_chain(store, database_root, &metadata, &head, database_path, 0, 0, cipher)) {
        recovery_diag("restore_store: full chain WALK failed gen=%llu commit=%s",
                      (unsigned long long)head.generation, head.commit_id);
        return 0;
    }
    remove(database_path);
    if (!walk_manifest_chain(store, database_root, &metadata, &head, database_path, 1, 0, cipher) ||
        !validate_head_checksum(store, database_root, &head, database_path)) {
        recovery_diag("restore_store: full chain APPLY or checksum failed gen=%llu commit=%s",
                      (unsigned long long)head.generation, head.commit_id);
        remove(database_path);
        return 0;
    }
    return 1;
}

int otsardb_recovery_restore_store(otsardb_object_store *store,
                                 const char *database_root,
                                 const char *database_path) {
    /* Legacy entry point: falls back to the thread-local session cipher (set
     * by s3_database.cpp at open), which is NULL in every pre-0079 caller —
     * the plaintext path is byte-identical. */
    return otsardb_recovery_restore_store_ex(store, database_root, database_path,
                                           otsardb_cipher_thread_get());
}

/* Local file copy (plain fopen read/write loop). Used to stage the delta
 * into a temp image next to the base and to publish the verified temp image
 * to database_path (works across volumes where rename would fail). */
static int copy_file_bytes(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) return 0;
    FILE *out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return 0;
    }
    char buf[65536];
    int ok = 1;
    for (;;) {
        size_t n = fread(buf, 1, sizeof(buf), in);
        if (n > 0 && fwrite(buf, 1, n, out) != n) {
            ok = 0;
            break;
        }
        if (n < sizeof(buf)) {
            if (ferror(in)) ok = 0;
            break;
        }
    }
    if (fflush(out) != 0) ok = 0;
    fclose(in);
    fclose(out);
    return ok;
}

int otsardb_recovery_materialize_delta_ex(otsardb_object_store *store,
                                        const char *database_root,
                                        const char *base_image_path,
                                        uint64_t base_generation,
                                        const char *base_manifest_sha256,
                                        const char *database_path,
                                        otsardb_head *out_head,
                                        const otsardb_cipher *cipher) {
    if (!store || !database_root || database_root[0] == '\0' ||
        !base_image_path || base_image_path[0] == '\0' ||
        !database_path || database_path[0] == '\0' ||
        base_generation == 0 ||
        !base_manifest_sha256 ||
        strlen(base_manifest_sha256) != OTSARDB_MANIFEST_SHA256_HEX_SIZE ||
        strcmp(base_image_path, database_path) == 0) {
        return 0;
    }

    otsardb_db_metadata metadata;
    if (otsardb_db_metadata_read(store, database_root, &metadata) != OTSARDB_METADATA_OK) {
        recovery_diag("materialize_delta: metadata read failed root=%s", database_root);
        return 0;
    }

    otsardb_head head;
    char *head_etag = NULL;
    if (otsardb_journal_read_head(store, database_root, &head, &head_etag) != OTSARDB_JOURNAL_OK) {
        free(head_etag);
        recovery_diag("materialize_delta: head read failed root=%s", database_root);
        return 0;
    }
    free(head_etag);
    if (strcmp(head.database_id, metadata.database_id) != 0 || head.generation == 0 ||
        head.generation > OTSARDB_RECOVERY_MAX_COMMITS ||
        strlen(head.commit_sha256) != OTSARDB_MANIFEST_SHA256_HEX_SIZE) {
        recovery_diag("materialize_delta: head validation failed gen=%llu dbid=%s sha_len=%zu",
                      (unsigned long long)head.generation, head.database_id,
                      strlen(head.commit_sha256));
        return 0;
    }
    if (head.generation <= base_generation) {
        /* Nothing newer than the base to apply: the caller should have taken
         * the cache-probe path for an unchanged HEAD. Fail closed. */
        recovery_diag("materialize_delta: head gen=%llu not newer than base gen=%llu",
                      (unsigned long long)head.generation,
                      (unsigned long long)base_generation);
        return 0;
    }

    /* Validate the chain down to base_generation (inclusive) with the exact
     * recovery checks; the collected keys are newest-first. */
    char **keys = NULL;
    size_t key_count = 0;
    if (!collect_chain_keys(store, database_root, &metadata, &head,
                            base_generation, &keys, &key_count) ||
        key_count == 0) {
        recovery_diag("materialize_delta: chain walk failed base_gen=%llu head_gen=%llu",
                      (unsigned long long)base_generation,
                      (unsigned long long)head.generation);
        return 0;
    }

    /* Base link: the oldest collected key (generation base_generation) must
     * hash to base_manifest_sha256 — the manifest SHA-256 the cached image
     * was saved under. This is what binds the local base image to this
     * remote chain; any mismatch fails closed (full-path fallback).
     *
     * (D2): the binding is now also IMAGE-LEVEL, not just
     * manifest-hash-level. The base file must have exactly the size the
     * base manifest declares (database_size_pages * page_size), and when
     * the base manifest carries a post-apply checksum the file must match
     * it — a tampered / bit-rotten / crash-mixed cached image (D1) is
     * detected BEFORE it is diffed, instead of being caught only by the
     * head checksum gate (or never, on a checksum-less head). Any mismatch
     * returns 0 and the caller falls back to the full path. */
    {
        otsardb_commit_manifest base_manifest = {0};
        otsardb_segment_enc_set base_enc;
        int base_ok = read_manifest_object(store, keys[key_count - 1],
                                           base_manifest_sha256,
                                           &base_manifest, &base_enc) &&
                      base_manifest.generation == base_generation;
        if (base_ok) {
            uint64_t expected_size =
                base_manifest.database_size_pages *
                (uint64_t)base_manifest.page_size;
            uint64_t actual_size = 0;
            if (!file_size_u64(base_image_path, &actual_size) ||
                actual_size != expected_size) {
                recovery_diag("materialize_delta: base image size mismatch "
                              "base=%s got=%llu want=%llu",
                              base_image_path,
                              (unsigned long long)actual_size,
                              (unsigned long long)expected_size);
                base_ok = 0;
            }
            if (base_ok && !validate_file_against_manifest_checksum(
                               base_image_path, base_manifest.page_size,
                               base_manifest.database_size_pages,
                               base_manifest.post_apply_checksum)) {
                recovery_diag("materialize_delta: base image checksum mismatch "
                              "base=%s want=%s",
                              base_image_path,
                              base_manifest.post_apply_checksum);
                base_ok = 0;
            }
        }
        otsardb_commit_manifest_free(&base_manifest);
        if (!base_ok) {
            recovery_diag("materialize_delta: base manifest link failed key=%s want_sha=%s",
                          keys[key_count - 1], base_manifest_sha256);
            for (size_t i = 0; i < key_count; ++i) free(keys[i]);
            free(keys);
            return 0;
        }
    }

    /* Stage the delta into a TEMP image next to the base: a crash mid-apply
     * leaves the base image (and database_path) untouched. */
    size_t temp_len = strlen(base_image_path) + 16u;
    char *temp_path = (char *)malloc(temp_len);
    if (!temp_path) {
        for (size_t i = 0; i < key_count; ++i) free(keys[i]);
        free(keys);
        return 0;
    }
    snprintf(temp_path, temp_len, "%s.deltatmp", base_image_path);

    int ok = copy_file_bytes(base_image_path, temp_path);
    if (!ok) {
        recovery_diag("materialize_delta: base image copy failed base=%s", base_image_path);
    }
    if (ok) {
        /* Apply ONLY the segments of generations base_generation+1..head.
         * keys are newest-first, so dropping the LAST key (the delta base at
         * generation base_generation) leaves exactly the newer generations,
         * applied oldest first — the cached image already contains the base
         * generations' frames and they must never be re-fetched. */
        ok = apply_manifest_keys(store, database_root, keys, key_count - 1,
                                 cipher, temp_path) &&
             validate_head_checksum(store, database_root, &head, temp_path);
    }
    if (!ok) {
        recovery_diag("materialize_delta: apply or head checksum failed base_gen=%llu head_gen=%llu",
                      (unsigned long long)base_generation,
                      (unsigned long long)head.generation);
        remove(temp_path);
        free(temp_path);
        for (size_t i = 0; i < key_count; ++i) free(keys[i]);
        free(keys);
        return 0;
    }

    /* Publish the verified image. database_path is only ever replaced by a
     * checksum-verified temp image. */
    remove(database_path);
    ok = copy_file_bytes(temp_path, database_path);
    remove(temp_path);
    free(temp_path);
    for (size_t i = 0; i < key_count; ++i) free(keys[i]);
    free(keys);
    if (!ok) {
        recovery_diag("materialize_delta: publish failed dst=%s", database_path);
        return 0;
    }
    if (out_head) *out_head = head;
    return 1;
}

int otsardb_recovery_materialize_delta(otsardb_object_store *store,
                                     const char *database_root,
                                     const char *base_image_path,
                                     uint64_t base_generation,
                                     const char *base_manifest_sha256,
                                     const char *database_path,
                                     otsardb_head *out_head) {
    /* Legacy entry point: falls back to the thread-local session cipher,
     * mirroring otsardb_recovery_restore_store (NULL = plaintext). */
    return otsardb_recovery_materialize_delta_ex(store, database_root,
                                               base_image_path,
                                               base_generation,
                                               base_manifest_sha256,
                                               database_path,
                                               out_head,
                                               otsardb_cipher_thread_get());
}
