#ifndef OTSARDB_LOCAL_CACHE_H
#define OTSARDB_LOCAL_CACHE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OTSARDB_LOCAL_CACHE_COMMIT_ID_MAX 63
#define OTSARDB_LOCAL_CACHE_SHA256_MAX 64

/*
 * State that authorised the current cached database image. The probe
 * compares this against the remote HEAD; any mismatch invalidates the
 * cache and forces a full recovery.
 *
 * (D1): image_size_bytes + image_sha256 additionally bind the
 * cache entry to the IMAGE BYTES (SHA-256 of the cached state.db file), not
 * just to the manifest hash. otsardb_local_cache_save computes them from the
 * source image (callers that zero-initialise the struct and set only the
 * generation/commit fields are unchanged); read_metadata refuses pre-0135
 * metadata files that lack them, so a cache entry whose image bytes were
 * never attested can never be served.
 */
typedef struct otsardb_local_cache_state {
    uint64_t generation;
    char commit_id[OTSARDB_LOCAL_CACHE_COMMIT_ID_MAX + 1];
    char commit_sha256[OTSARDB_LOCAL_CACHE_SHA256_MAX + 1];
    uint64_t image_size_bytes;
    char image_sha256[OTSARDB_LOCAL_CACHE_SHA256_MAX + 1];
} otsardb_local_cache_state;

/*
 * Check whether a valid local cache exists under cache_dir for
 * database_id. If remote_head matches and the cached image is intact,
 * copies it to target_image_path and returns 1. Returns 0 when no
 * cache, the remote state changed, or the image is missing.
 *
 * (D1): the image is verified BEFORE it is served — its
 * size must equal the metadata's image_size_bytes and its SHA-256 must
 * equal the metadata's image_sha256 (computed while copying, so the
 * cached image is never read twice). A crash mid-save can no longer leave
 * a mixed old/new image that passes the probe: the save path publishes
 * the image with a same-directory temp + fsync + atomic rename, and any
 * entry whose bytes do not hash to their attested image_sha256 (tamper,
 * bit rot, a torn pre-0135 save) is rejected and forces full recovery.
 */
int otsardb_local_cache_probe(const char *cache_dir,
                            const char *database_id,
                            const otsardb_local_cache_state *remote_head,
                            const char *target_image_path);

/*
 * Persist the current database image as the new cache. The metadata
 * file is written atomically (temp + fsync + rename); a crash before it
 * is durable leaves the previous cache intact (or no cache at all — both
 * are safe fallbacks).
 *
 * (D1): the IMAGE file is also published atomically —
 * streamed to <image>.tmp in the same directory, fsync'd, then renamed
 * over the old image. A crash mid-save leaves only a stale .tmp (never
 * read) plus either the OLD image with the OLD metadata or the NEW image
 * with the OLD metadata; the probe rejects the mismatched pair and never
 * serves a torn image. The metadata now attests image_size_bytes +
 * image_sha256 (SHA-256 of the saved image), and pre-0135 metadata files
 * without those fields are refused by the probe/read_state.
 *
 * Bounded cache: OTSARDB_CACHE_MAX_MB caps the total
 * tracked image bytes (default 512; fractional values accepted, e.g.
 * 0.01 ~= 10 KiB; 0 = unlimited = pre-0068 behavior). When a save
 * pushes the total over the budget, the OLDEST other databases' images
 * (by last-save time) are evicted; the just-saved image is never
 * evicted by its own save. Sizes are tracked in otsardb-cache-index.json
 * inside otsardb-cache-v1/; a missing or corrupt index is rebuilt from a
 * directory scan on the next save, so eviction state is crash-safe and
 * a lost cache entry only costs a future recovery (S3 stays
 * authoritative).
 */
int otsardb_local_cache_save(const char *cache_dir,
                           const char *database_id,
                           const char *source_image_path,
                           const otsardb_local_cache_state *state);

/*
 * read the cached image's authorising state (generation +
 * commit_id + commit_sha256 — the manifest SHA-256 the image was saved at)
 * WITHOUT copying the image, so the caller can decide between the delta
 * materialization path and the full path: when the saved image is at
 * generation G < remote head.generation and the chain's manifest at G
 * hashes to cached.commit_sha256, otsardb_recovery_materialize_delta applies
 * only the newer segments on top of the cached image. Returns 1 when a
 * valid cache entry (metadata + image file) exists and *out is filled
 * (commit_sha256 is always 64 hex chars — read_metadata enforces it);
 * 0 when absent or corrupt.
 *
 * *out additionally carries image_size_bytes +
 * image_sha256 (the image's attested size/SHA-256, enforced by
 * read_metadata); a pre-0135 metadata file without those fields is
 * treated as corrupt (0) so a delta base whose image bytes were never
 * attested is never offered.
 *
 * otsardb_local_cache_probe is unchanged: it still requires the remote head
 * to match EXACTLY (the unchanged-HEAD fast path, OTSARDB_CACHE_REUSE).
 * No behavior change to save/probe; this accessor is additive.
 */
int otsardb_local_cache_read_state(const char *cache_dir,
                                 const char *database_id,
                                 otsardb_local_cache_state *out);

#ifdef __cplusplus
}
#endif

#endif
