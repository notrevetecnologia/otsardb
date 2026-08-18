#ifndef OTSARDB_WAL_PUBLISHER_H
#define OTSARDB_WAL_PUBLISHER_H

#include "commit_publish.h"
#include "object_store.h"
#include "sqlite_wal.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct otsardb_wal_publisher {
    otsardb_object_store *store;
    otsardb_write_strategy strategy;
    char database_root[OTSARDB_KEY_MAX + 1];
    char database_id[OTSARDB_COMMIT_ID_MAX + 1];

    /* Maximum encoded payload bytes per published segment. A single
     * transaction larger than this is split into multiple bounded segments
     * for one commit (manifest v4). 0 means the default limit. */
    uint64_t segment_payload_limit;

    int head_loaded;
    int metadata_ready;
    uint64_t current_generation;
    char current_commit_id[OTSARDB_COMMIT_ID_MAX + 1];
    char current_manifest_sha256[OTSARDB_MANIFEST_SHA256_HEX_SIZE + 1];
    char current_head_etag[128];

    /* Generation of the local execution image the SQLite side is writing
     * against. When set, a remote HEAD that has advanced beyond this value
     * means another process committed — the local image is stale and any
     * write must fail (SQLITE_BUSY) instead of silently losing the other
     * writer's acknowledged commit. */
    int local_image_generation_known;
    uint64_t local_image_generation;

    /* proof that the live local database file equals the
     * parent state (the image recovery materialised at the current remote
     * HEAD). The rolling checksum may read old-page contributions from the
     * local file ONLY while this proof holds; otherwise a fresh/disposable
     * local file (OtsarDB's own execution-state thesis) would XOR out wrong
     * old pages and publish a post_apply_checksum that fails recovery
     * . Established ONLY by the recovery/materialization
     * anchor — set_local_image_generation is only ever called right after
     * otsardb_recovery_restore_store / otsardb_lazy_restore_store materialised
     * the image, and while the anchor does not exceed the current head the
     * file IS the parent image for every page the session never touched
     * (the 0024 generation fence refuses writes once another writer
     * advances the head). A file-checksum comparison against the parent
     * manifest was rejected: in WAL mode the file lags at the last
     * checkpoint and never byte-equals the parent mid-session (observed in
     * test development). Cleared whenever the page map is
     * cleared (WAL generation reset, chain change) and re-established by
     * the anchor or by retry_publish's re-materialisation; while unproven,
     * any commit that would need an old-page file read publishes NO
     * post_apply_checksum — recovery treats an absent checksum as "chain
     * predates checksums" and skips the gate, so the durability contract
     * holds and a wrong checksum is never published. */
    int local_image_verified;

    /* Verification target stashed by refresh_head while resurrecting the
     * parent's rolling checksum: the parent manifest's page size and page
     * count. Used by publish_one_commit() to decide which pages need an old
     * value at all (pages beyond the parent state have old contribution 0
     * by definition and never touch the local file). */
    uint32_t local_image_verify_page_size;
    uint64_t local_image_verify_page_count;

    /* Rolling DB checksum of the state the local image currently represents.
     * checksum_known is 1 once the chain carries checksums; page_contribs
     * tracks per-page CRC contributions so a new commit can XOR-out old
     * pages and XOR-in new ones in O(changed pages). */
    int checksum_known;
    uint64_t state_checksum;
    uint32_t page_contrib_count;
    uint32_t *page_contrib_nos;
    uint64_t *page_contrib_values;

    int wal_generation_known;
    uint32_t wal_salt1;
    uint32_t wal_salt2;
    uint32_t published_commit_count;
    int wal_cursor_ready;
    otsardb_sqlite_wal_cursor wal_cursor;

    /* M2 writer lease fence (design): when
     * set, publish_one_commit refuses every publish with SQLITE_BUSY unless
     * lease_fence(lease_ctx) returns non-zero. The callback is process-local
     * (no remote round trip per commit); the HEAD CAS update inside
     * otsardb_publish_commit is the physical barrier that fences a lease that
     * was stolen by a faster takeover. */
    int (*lease_fence)(void *lease_ctx);
    void *lease_ctx;

    /* M3 conflict detection: set when the LAST publish
     * failure was a genuine page-overlap CAS conflict — another writer
     * committed overlapping pages between our parent and the current remote
     * HEAD. The sync hook then returns SQLITE_BUSY_SNAPSHOT (the extended
     * code) so clients can distinguish snapshot conflicts from a plain
     * stale-writer retry (SQLITE_BUSY). Reset at the start of every sync. */
    int sqlite_extended_conflict;

    /* (Model B synchronous dual publish) / 0091 (Model C
     * quorum N-of-M): the replica destination handle. Lazily opened from
     * the OTSARDB_S3_REPLICA_* / REPLICA2_* / REPLICA3_* environment on the
     * first sync that publishes a commit; NULL when no replica is
     * configured (the single-destination path stays byte-identical to
     * pre-0072 behavior). The handle manages a LIST of destinations (up to
     * OTSARDB_REPLICA_MAX_DESTINATIONS) and applies the OTSARDB_S3_QUORUM
     * decision rule (default = every destination). Owned by the publisher;
     * released by otsardb_wal_publisher_destroy. The replica module
     * (src/storage/replica.c) is allowed to set it through
     * otsardb_replica_attach (test hook) and to append destinations. */
    struct otsardb_replica *replica;

    /* (Model B synchronous dual publish): identity of a
     * dual-publish retry still pending on the replica. Set when the primary
     * publish of `dual_pending_commit_id` succeeded but the replica phase
     * failed and the cached HEAD rolled back. refresh_head must treat the
     * remote HEAD reaching this commit as OUR OWN progress (the page map and
     * the local-image proof describe exactly the state the retry re-computes)
     * instead of clearing them as a foreign chain change; without this the
     * retry would re-publish a checksum-less manifest and could never ack.
     * Cleared when the dual operation completes (either destination durable
     * + acked) or the retry is abandoned (genuine conflict, WAL reset). */
    int dual_pending;
    char dual_pending_commit_id[OTSARDB_COMMIT_ID_MAX + 1];

    uint64_t wal_bytes_read_last_sync;
    uint64_t wal_bytes_read_total;
} otsardb_wal_publisher;

#define OTSARDB_WAL_PUBLISHER_DEFAULT_SEGMENT_PAYLOAD_LIMIT (16u * 1024u * 1024u)

int otsardb_wal_publisher_init(otsardb_wal_publisher *publisher,
                             otsardb_object_store *store,
                             const char *database_root,
                             const char *database_id,
                             otsardb_write_strategy strategy);

/* Set the maximum encoded payload bytes per segment (0 restores the default).
 * A transaction exceeding the limit is published as multiple ordered
 * segments of one commit. Frames per chunk are derived from this budget. */
void otsardb_wal_publisher_set_segment_payload_limit(otsardb_wal_publisher *publisher,
                                                   uint64_t limit_bytes);

/* Declare the generation of the local execution image the SQLite side was
 * materialised from (the remote HEAD generation at open/restore time). Any
 * later sync that discovers a remote HEAD beyond this value refuses writes
 * with SQLITE_BUSY, protecting against lost updates from a stale local
 * image racing a concurrent writer. */
void otsardb_wal_publisher_set_local_image_generation(otsardb_wal_publisher *publisher,
                                                    uint64_t generation);

/* Install (or clear) the M2 writer lease fence: when `lease_fence` is
 * non-NULL, every commit publish is refused with SQLITE_BUSY unless
 * lease_fence(lease_ctx) returns non-zero. The callback must be safe to
 * call from the WAL sync hook context (typically otsardb_lease_is_live). */
void otsardb_wal_publisher_set_lease_fence(otsardb_wal_publisher *publisher,
                                         int (*lease_fence)(void *lease_ctx),
                                         void *lease_ctx);

/* release resources owned by the publisher. Today this is
 * the lazily-opened replica destination (its S3 store instance); the
 * replica module is designed so the session path may call this from its
 * teardown (the s3_database.cpp destroy_session wiring is a documented
 * follow-up — see). */
void otsardb_wal_publisher_destroy(otsardb_wal_publisher *publisher);

/* — operator promotion (0069 decision point 6; the session
 * calls this at open when otsardb_replica_promote_requested() is true).
 *
 * Promotes a replica destination to PRIMARY:
 * 1. lazily opens the replica handle (OTSARDB_S3_REPLICA_* env; fail closed);
 * 2. refuses while the OLD PRIMARY (this publisher's store) is
 * lease-held/authoritative — its <root>/lease.json must be absent or
 * provably expired (offline/disaster-recovery operator contract);
 * 3. picks the destination with the NEWEST COMMITTED
 * ledger/HEAD among the handle's destinations (safe-last-commit);
 * refuses when no destination holds any COMMITTED commit;
 * 4. raises the picked destination's epoch by CAS (If-Match N -> N+1,
 * If-None-Match create when absent) — the loser of a concurrent
 * promotion race is refused (split-brain drill);
 * 5. runs FULL recovery from the promoted store into `local_db_path`
 * (chain validation + materialisation).
 *
 * On success *out_epoch (optional) receives the raised epoch and the
 * handle's session epoch is updated (its next replica-phase fence matches
 * the promoted record). otsardb_replica_promoted_index(&publisher->replica)
 * reports which destination was promoted (the session reopens it as its
 * primary store). Returns 1 on success; 0 with a human message in `err` and
 * the journal-level cause in *out_rc (OTSARDB_JOURNAL_CONFLICT =
 * authoritative refusal: lease-held old primary, lost promotion race, or
 * nothing-to-promote).
 *
 * NOTE: the session-side STORE SWAP (the promoted store becomes the
 * session's primary, the old primary becomes its replica) lives in
 * s3_database.cpp and is a documented follow-up; this entry point is the
 * tested primitive of the promotion protocol. */
int otsardb_wal_publisher_promote(otsardb_wal_publisher *publisher,
                                const char *local_db_path,
                                uint64_t *out_epoch,
                                char *err,
                                size_t err_size,
                                otsardb_journal_result *out_rc);

/*
 * Incremental callback for otsardb_local_vfs_set_wal_reader_sync_hook().
 * The normal path reads the 32-byte WAL header plus only frames after the
 * last remotely durable commit cursor. A process restart may rebuild that
 * cursor once by scanning committed frames up to the commit encoded in HEAD.
 * db_read_at/db_read_ctx optionally provide random access to the local
 * database file (used to compute rolling checksums over replaced pages);
 * pass NULL when unavailable.
 */
int otsardb_wal_publisher_sync_reader(const char *wal_name,
                                    uint64_t wal_size,
                                    otsardb_sqlite_wal_read_at read_at,
                                    void *read_ctx,
                                    otsardb_sqlite_wal_read_at db_read_at,
                                    void *db_read_ctx,
                                    void *ctx);

/* Backward-compatible full-image entry point for tooling/legacy tests. */
int otsardb_wal_publisher_sync(const char *wal_name,
                             const void *wal_data,
                             size_t wal_size,
                             void *ctx);

/*
 * M3 phase 3: refresh the publisher's cached remote HEAD
 * state (generation, commit id, manifest hash, head etag, and the parent's
 * rolling checksum) without going through a WAL sync. Same logic as the
 * refresh performed at the start of every otsardb_wal_publisher_sync_reader.
 * Returns SQLITE_OK on success, including "no HEAD yet" (generation 0);
 * SQLITE_IOERR_READ / SQLITE_MISMATCH on store or identity errors.
 * Must be called on the same thread that owns the publisher (no concurrent
 * sync).
 */
int otsardb_wal_publisher_refresh_head(otsardb_wal_publisher *publisher);

/* the 0065 bounded retry cycle is shared with the replica
 * module (src/storage/replica.c) so replica-phase store calls reuse the SAME
 * policy knobs (OTSARDB_PUBLISH_RETRIES / OTSARDB_PUBLISH_RETRY_BASE_MS) and
 * retryable classification as the primary publish stages. */
int otsardb_wal_publisher_rc_retryable(otsardb_journal_result rc);
int otsardb_wal_publisher_retry_cycle(int (*attempt)(void *opaque,
                                                   otsardb_journal_result *out_rc),
                                    void *opaque,
                                    otsardb_journal_result *out_rc);

/* ---------------------------------------------------------------------------
 * bounded exponential-backoff retry for transient publish
 * failures (policy:).
 *
 * The publish path (segment PUTs, manifest PUT, HEAD CAS, and the 
 * 0060 parallel segment pre-upload) re-attempts TRANSPORT-class failures
 * with the SAME commit request under bounded exponential backoff:
 *
 * - OTSARDB_PUBLISH_RETRIES total publish attempts including the first;
 * default 3, clamped 1..10 (1 disables retry);
 * - OTSARDB_PUBLISH_RETRY_BASE_MS backoff base delay; default 250, clamped
 * 0..60000; delays double per attempt.
 *
 * Retryable: OTSARDB_JOURNAL_ERROR (transport) and OTSARDB_JOURNAL_AMBIGUOUS
 * (timeout; every attempt re-resolves by commit identity — a durable commit
 * reports success, never a second HEAD). NEVER retried: CONFLICT (HEAD CAS
 * precondition failure — the fencing result), NOT_FOUND, INVALID,
 * UNSUPPORTED. On final failure the first error is returned unchanged: the
 * sync fails and the commit is not acknowledged. The 0063 verified-parent
 * checksum invariant is undisturbed: the rolling checksum is computed once
 * per commit before the retry cycle.
 * ------------------------------------------------------------------------- */

/*
 * M3 phase 3: explicit re-materialise-and-retry for a
 * DISJOINT-page conflict (the failed publish's page set does not intersect
 * any interleaved commit's page set — see otsardb_publish_commit_detect_conflict).
 *
 * PRECONDITIONS (caller contract):
 * - otsardb_publish_commit (via the publisher) previously returned a CAS
 * CONFLICT and the caller classified it as disjoint (page overlap == 0);
 * - frames are the SAME already-validated, fsynced WAL frame references the
 * failed attempt used; their page images are re-read from the local WAL
 * file "<local_db_path>-wal" at frame_offset + 24 (frame header) — the
 * caller must keep that file present and fsynced;
 * - local_db_path is the local execution image the transaction ran against
 * (materialised at the failed attempt's parent generation);
 * - page_size and database_size_pages match the values passed to the
 * original publish;
 * - no concurrent sync: call on the same thread that owns the publisher.
 *
 * Behavior:
 * - refreshes the remote HEAD; if it is UNCHANGED (still the failed
 * attempt's parent) returns 0 (nothing to re-materialise; the caller may
 * retry the normal publish path);
 * - re-checks disjointness against the interleaved chain (defence in depth,
 * same walk as otsardb_publish_commit_detect_conflict): genuine overlap ->
 * returns -1 WITHOUT touching the store or the local file;
 * - otherwise re-materialises the local database image at the new remote
 * HEAD (otsardb_recovery_restore_store over local_db_path), resets the
 * publisher's session page map (old contributions refer to the discarded
 * image), re-bases the rolling checksum on the new parent, and re-publishes
 * the SAME frames onto the new HEAD with a fresh commit id (the failed
 * attempt's commit id already names an orphan manifest with the OLD parent
 * identity, which immutability forbids rewriting — see).
 *
 * Returns:
 * 1 retry committed (new HEAD includes this commit and the winner's);
 * 0 remote HEAD unchanged — nothing to do, caller retries normally;
 * -1 genuine page-overlap conflict after all (or on re-publish: the retry
 * conflicted again WITH page overlap) — do NOT auto-retry, surface
 * SQLITE_BUSY_SNAPSHOT and re-execute the transaction at the new HEAD;
 * -2 error (store I/O, recovery failure, missing WAL/db file, invalid
 * arguments, or a second disjoint CAS failure on re-publish).
 */
int otsardb_wal_publisher_retry_publish(otsardb_wal_publisher *publisher,
                                      const char *local_db_path,
                                      uint32_t page_size,
                                      uint64_t database_size_pages,
                                      const otsardb_sqlite_wal_frame_ref *frames,
                                      uint32_t frame_count);

#ifdef __cplusplus
}
#endif

#endif
