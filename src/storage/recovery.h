#ifndef OTSARDB_RECOVERY_H
#define OTSARDB_RECOVERY_H

#include "cipher.h"
#include "checkpoint_manifest.h"
#include "commit_manifest.h"
#include "db_metadata.h"
#include "object_store.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bounded-walk budget for hash-linked manifest chains, shared by recovery
 * and the commit conflict walks (F05): no walker may read
 * more manifests than this. Each link is one manifest GET, so the bound is
 * both a time bound (sequential fetches) and a generation cap. Exceeding it
 * fails closed (recovery refuses the chain; resolve/detect-conflict return
 * OTSARDB_JOURNAL_INVALID). */
#define OTSARDB_RECOVERY_MAX_COMMITS 1000000u

/*
 * Applies one committed OtsarDB WAL batch to a local database image.
 * The destination file may be empty/non-existent.
 */
int otsardb_recovery_apply_wal_batch_file(const void *batch_data,
                                        size_t batch_size,
                                        const char *database_path);

/*
 * Reconstructs the latest authoritative database state from db.json,
 * HEAD.json, commit manifests and .otsseg objects in an OtsarDB object store.
 * Existing destination contents are discarded.
 *
 * (B2/B3): the checkpoint-accelerated path additionally
 * validates the checkpoint's identity (database_id/page_size against
 * metadata, covered_generation against the pointer, base_commit_id and
 * base_manifest_sha256 bound to the chain-authenticated manifest at the
 * covered generation) and re-validates the materialised base image against
 * that manifest (size always; rolling post-apply checksum when the covered
 * manifest carries one) BEFORE replaying newer segments — a stale/foreign
 * checkpoint or an inflated covered_generation (concurrent-writer race)
 * fails closed to the full chain walk instead of serving a wrong image.
 *
 * otsardb_recovery_restore_store_ex additionally decrypts
 * segments the chain-authenticated manifest marks encrypted (top-level
 * "segment_enc" array) with the given cipher; a missing/wrong cipher fails
 * closed (the restore fails; encrypted segments are never decoded as
 * garbage). The legacy otsardb_recovery_restore_store falls back to the
 * thread-local session cipher (NULL = plaintext, byte-identical pre-0079
 * behavior).
 */
int otsardb_recovery_restore_store_ex(otsardb_object_store *store,
                                    const char *database_root,
                                    const char *database_path,
                                    const otsardb_cipher *cipher);

int otsardb_recovery_restore_store(otsardb_object_store *store,
                                 const char *database_root,
                                 const char *database_path);

/*
 * Visitor callback for otsardb_recovery_walk_chain. Called once per validated
 * manifest, NEWEST first. The manifest is transient (its owned arrays must
 * be copied by the visitor if kept). Returning 0 aborts the walk as a
 * failure. expected_sha256 is the exact SHA-256 the manifest was verified
 * against (HEAD's commit_sha256 for the newest, the parent's
 * parent_manifest_sha256 for the rest; empty for legacy v1 chains).
 * enc carries the manifest's parsed "segment_enc" set
 * (present == 0 for legacy plaintext manifests).
 */
typedef int (*otsardb_chain_visitor)(void *ctx,
                                   const char *manifest_key,
                                   const char *expected_sha256,
                                   const otsardb_commit_manifest *manifest,
                                   const otsardb_segment_enc_set *enc);

/*
 * Walk the manifest chain from HEAD to genesis (or to stop_generation,
 * inclusive, when non-zero), validating exactly like recovery:
 * - each manifest object's SHA-256 against the chain link (HEAD or the
 * parent's parent_manifest_sha256);
 * - generation continuity (parent_generation + 1 == generation) and
 * commit_id continuity (parent_commit_id);
 * - database_id/page_size match against metadata;
 * - genesis termination rules (empty parent ids, empty parent hash for v2+);
 * - with stop_generation == 0 the genesis manifest is re-read and its
 * parent_generation == 0 verified, as recovery does.
 *
 * The visitor is invoked for every validated manifest (newest first) and may
 * abort by returning 0. Returns 1 when the whole walk validated, 0 on any
 * failure or visitor abort. No segments are downloaded.
 */
int otsardb_recovery_walk_chain(otsardb_object_store *store,
                              const char *database_root,
                              const otsardb_db_metadata *metadata,
                              const otsardb_head *head,
                              uint64_t stop_generation,
                              otsardb_chain_visitor visitor,
                              void *visitor_ctx);

/*
 * Write every page of a decoded checkpoint manifest's page groups into
 * database_path (creating it if needed), verifying each group's SHA-256 and
 * size against the manifest first. Used by checkpoint-accelerated recovery
 * and by lazy hydration's base-image materialisation.
 */
int otsardb_recovery_apply_checkpoint_pages(otsardb_object_store *store,
                                          const char *database_root,
                                          const otsardb_checkpoint_manifest *cp,
                                          const char *database_path);

/*
 * — delta materialization on a cached image (
 * R3 second slice / NVMe tier): materialise the remote head state by
 * applying ONLY the segments of generations base_generation+1..head on top
 * of an existing local image that captures the database at
 * base_generation (a previously saved complete cache image), instead of
 * replaying the genesis..base_generation history.
 *
 * Integrity / fail-closed contract:
 * - the remote chain is validated with the exact checks recovery uses
 * (HEAD manifest SHA-256, parent hash links, generation/commit-id
 * continuity, database_id/page_size anchors, genesis termination);
 * - the walk stops at base_generation INCLUSIVE and the manifest at that
 * generation must hash to base_manifest_sha256 (the manifest SHA-256
 * the cached image was saved under) — this binds the local image to the
 * remote chain;
 * - (D2): the binding is IMAGE-LEVEL, not just
 * manifest-hash-level — before the base image is copied or diffed its
 * size must equal the base manifest's database_size_pages * page_size,
 * and when the base manifest carries a post-apply checksum the base
 * image must match it. A tampered / bit-rotten / crash-mixed cached
 * base image (D1) fails closed here instead of producing a wrong
 * materialized image (previously caught only by the head checksum
 * gate, or never on a checksum-less head);
 * - the delta is applied into a TEMP image created next to the base and
 * published to database_path only after the head manifest's whole-file
 * post_apply_checksum verifies (an absent checksum skips the gate, the
 * same trade-off as full recovery);
 * - ANY mismatch (chain link, base manifest sha, base image size or
 * checksum, segment hash/size, head checksum, unreadable base image)
 * returns 0 WITHOUT touching database_path or base_image_path — the
 * caller falls back to the full materialization path
 * (otsardb_recovery_restore_store_ex / otsardb_lazy_restore_store_ex);
 * a wrong image is never produced.
 *
 * base_generation must be >= 1 and strictly below the remote head
 * generation; base_image_path must exist, differ from database_path and
 * already contain the database state at base_generation (the delta path
 * never re-fetches the base generations' segments). On success *out_head
 * receives the remote head the image now matches, so the caller can anchor
 * the publisher's local-image generation fence (0024) at head.generation.
 *
 * Caller wiring (decide delta-vs-full): read the cached image's state via
 * otsardb_local_cache_read_state; when cached.generation < head.generation
 * (and the cached image file exists) take this path, else the full path.
 * The exact s3_database.cpp call-site change is recorded in .
 *
 * segments the chain-authenticated manifest marks encrypted
 * are decrypted with the given cipher (NULL = plaintext, same rules as
 * otsardb_recovery_restore_store_ex).
 */
int otsardb_recovery_materialize_delta_ex(otsardb_object_store *store,
                                        const char *database_root,
                                        const char *base_image_path,
                                        uint64_t base_generation,
                                        const char *base_manifest_sha256,
                                        const char *database_path,
                                        otsardb_head *out_head,
                                        const otsardb_cipher *cipher);

int otsardb_recovery_materialize_delta(otsardb_object_store *store,
                                     const char *database_root,
                                     const char *base_image_path,
                                     uint64_t base_generation,
                                     const char *base_manifest_sha256,
                                     const char *database_path,
                                     otsardb_head *out_head);

#ifdef __cplusplus
}
#endif

#endif
