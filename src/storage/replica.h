#ifndef OTSARDB_REPLICA_H
#define OTSARDB_REPLICA_H

#include "capability_probe.h"
#include "commit_publish.h"
#include "journal.h"
#include "object_store.h"

#include <stddef.h>
#include <stdint.h>

/* Forward declaration (GCC strict C): the tag must be visible before the
 * prototypes below — declaring `struct otsardb_wal_publisher` inside a
 * parameter list creates a file-scope-local tag that never matches the
 * real definition in wal_publisher.h (Linux portability fix). */
struct otsardb_wal_publisher;

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * — Model B synchronous dual publish (HA plan P5, Level F,
 * design section 6).
 *
 * A replica writer module: when a replica destination is configured via the
 * OTSARDB_S3_REPLICA_* environment, the publish path becomes DUAL — the
 * primary publish (segments -> manifest -> HEAD CAS, unchanged) is followed
 * by a replica phase that makes BOTH destinations durable before the sync
 * returns success. The durability contract (/0021/0065)
 * becomes: ack == COMMITTED ledger entry on BOTH destinations.
 *
 * adds the promotion / reconciliation / split-brain layer
 * (0069 decision points 6-9):
 * - OTSARDB_S3_PROMOTE=1 (otsardb_replica_promote_requested) asks the session
 * to promote the replica destination: the epoch record is RAISED by CAS
 * (If-None-Match create when absent, If-Match N -> N+1 otherwise) and
 * full recovery runs from the promoted store. The old primary must not
 * hold a live writer lease (offline/disaster-recovery operator
 * contract): promotion is REFUSED while its <root>/lease.json is
 * provably live, so a two-promote race can never fence two primaries
 * (the CAS raise is the fence: the loser's If-Match fails with 412 and
 * the promotion is refused).
 * - The replica phase re-verifies the destination epoch on every commit
 * (otsardb_replica_replicate_commit): a session whose adopted epoch no
 * longer matches the destination's record (it was promoted by another
 * operator) is FENCED — its dual publish fails closed (CONFLICT class,
 * SQLITE_BUSY) instead of advancing a promoted replica.
 * - Reconciliation / return-to-primary: when the replica HEAD lags the
 * primary chain (a failed dual publish left the replica behind), the
 * replica phase walks the primary chain down from the commit's parent,
 * REPLAYS the gap (re-publishes each gap commit's db.json/segments/
 * manifest from the primary store and flips the ledgers COMMITTED on
 * both destinations, ABORTED -> PREPARED -> COMMITTED on the primary),
 * then proceeds with the new commit. A foreign replica HEAD still fails
 * closed (CONFLICT class — no reconciliation of an unrelated chain).
 *
 * — Model C quorum N-of-M (
 * MULTI_S3_DESIGN.md section 7): the replica module manages a LIST of up to
 * OTSARDB_REPLICA_MAX_DESTINATIONS destinations (OTSARDB_S3_REPLICA_*,
 * OTSARDB_S3_REPLICA2_*, OTSARDB_S3_REPLICA3_*). M = 1 primary + R destinations;
 * a commit is ACKED when Q of the M destinations (primary always counts as
 * 1) hold every object AND their COMMITTED ledger entry:
 * - OTSARDB_S3_QUORUM (default = M, i.e. ALL destinations must confirm) is
 * the minimum number of destinations of M required for the ack;
 * Q=1 means primary-only durability (replicas best-effort); values
 * outside 1..M clamp to M (all). The default (Q = M) with R = 1 is
 * Q=2-of-2 — byte-identical to 0072 Model B.
 * - a destination that fails with a TRANSPORT-class result (ERROR/
 * AMBIGUOUS after the 0065 budget) is marked lagging (best-effort);
 * when the remaining destinations still reach Q the sync ACKS and the
 * lagging destination is caught up by the 0077 gap-replay on a later
 * dual publish. Authoritative failures (CONFLICT/NOT_FOUND/INVALID/
 * UNSUPPORTED) fail the sync regardless of Q: a foreign/fenced/broken
 * destination can never be safely caught up.
 * - below-Q failures keep the exact 0072 contract: best-effort ABORTED
 * ledger on the primary, publisher rollback, INCONCLUSIVE, same-session
 * retry completes the same commit id.
 * - OTSARDB_S3_PROMOTE (0089) picks the destination with the NEWEST
 * COMMITTED ledger/HEAD among the destinations (safe-last-commit);
 * otsardb_replica_promoted_index reports which destination was promoted.
 * ------------------------------------------------------------------------- */

#define OTSARDB_REPLICA_EPOCH_OWNER_MAX 63
#define OTSARDB_REPLICA_MAX_DESTINATIONS 3

#define OTSARDB_REPLICA_STATE_PREPARED "PREPARED"
#define OTSARDB_REPLICA_STATE_COMMITTED "COMMITTED"
#define OTSARDB_REPLICA_STATE_ABORTED "ABORTED"
#define OTSARDB_REPLICA_STATE_MAX 16

/* Per-commit replication ledger entry, written to
 * <root>/replica/<commit_id>.json on BOTH destinations (content differs only
 * in `destination` and the etag fields). Fields per the 0069 design:
 * commit_id, generation, parent_generation, manifest_sha256, state, model,
 * destination, etags, written_at_utc. */
typedef struct otsardb_replica_ledger {
    char commit_id[OTSARDB_COMMIT_ID_MAX + 1];
    uint64_t generation;
    uint64_t parent_generation;
    char manifest_sha256[OTSARDB_SHA256_HEX_MAX + 1];
    char state[OTSARDB_REPLICA_STATE_MAX];
    char primary_etag[128];
    char replica_etag[128];
    char model[8];
    char destination[16];
    char written_at_utc[32];
} otsardb_replica_ledger;

typedef struct otsardb_replica otsardb_replica;

/* Returns 1 when ANY OTSARDB_S3_REPLICA_* variable is set (the operator
 * intends dual publish); 0 when the single-destination path runs untouched.
 * The default path must stay byte-identical to pre-0072 behavior, so the
 * publisher consults this before any replica work. */
int otsardb_replica_configured(void);

/* Open the replica destinations from the environment. Destination 0:
 * OTSARDB_S3_REPLICA_ENDPOINT (required)
 * OTSARDB_S3_REPLICA_REGION (default us-east-1)
 * OTSARDB_S3_REPLICA_ACCESS_KEY (optional; provider chain otherwise)
 * OTSARDB_S3_REPLICA_SECRET_KEY (optional)
 * OTSARDB_S3_REPLICA_BUCKET (required)
 * destinations 1 and 2 use the same rules with the
 * OTSARDB_S3_REPLICA2_* and OTSARDB_S3_REPLICA3_* variable sets. ANY replica
 * variable of ANY set means the operator intends multi-destination publish:
 * the open validates completeness per set and FAILS CLOSED (never a silent
 * single-destination fallback). A destination set that is entirely absent
 * is simply not configured (the handle then holds fewer destinations).
 * Every destination shares the primary's database_root and object keys.
 *
 * Steps: create each destination's store instance through the S3 adapter
 * (compiled only when the build defines OTSARDB_ENABLE_S3 for otsardb_core —
 * otherwise a fail-closed stub), run the capability probe (same 0035 retry
 * policy as the primary open; the per-endpoint probe verdict cache of
 * is not consulted — the replica always probes), derive CAS
 * vs SINGLE_WRITER from the verdict, and write the one-time epoch record
 * <root>/replica/epoch.json on the PRIMARY and on EVERY destination
 * (If-None-Match create; a 412 PRECONDITION_FAILED means a previous session
 * already created it and is handled as success).
 *
 * Fail-closed: any probe/store/epoch failure returns NULL with a human
 * message in `err`; the caller MUST fail the sync — never silently fall
 * back to a single destination. */
otsardb_replica *otsardb_replica_open(otsardb_object_store *primary_store,
                                  const char *database_root,
                                  const char *database_id,
                                  char *err,
                                  size_t err_size);

/* Release the replica handle. `store_borrowed` handles are never destroyed
 * (the test-attach path owns them). */
void otsardb_replica_close(otsardb_replica *replica);

/* Test hook (deterministic no-S3 tests): attach an already-probed store as
 * ONE destination, bypassing the environment/open path. `strategy` is the
 * write strategy to use against it; `borrowed` != 0 means the caller keeps
 * ownership (the replica never destroys the store). the
 * attach APPENDS a destination to the handle's list — repeated attaches
 * build the M = 1 + R destination set (the primary's epoch record is
 * bootstrapped once per handle, every destination's epoch once). Returns 1
 * on success. */
int otsardb_replica_attach(struct otsardb_wal_publisher *publisher,
                         otsardb_object_store *store,
                         otsardb_write_strategy strategy,
                         int borrowed);

/* ---------------------------------------------------------------------------
 * — Model C quorum N-of-M.
 * ------------------------------------------------------------------------- */

/* The operator's OTSARDB_S3_QUORUM knob: the minimum number of destinations
 * of M (M = 1 primary + R destinations; the primary always counts as 1)
 * that must hold the COMMITTED ledger for the sync to ack. Returns 0 when
 * unset / 0 / garbage (the safe default: EVERY destination must confirm —
 * with R = 1 that is the byte-identical 0072 Model B contract). A positive
 * value is returned verbatim; effective clamping to 1..M happens in
 * otsardb_replica_quorum. */
int otsardb_replica_quorum_requested(void);

/* The effective quorum for `replica`: the requested value clamped to
 * 1..M, or M when no quorum is requested (default = all destinations).
 * M = destination_count + 1 (the primary). */
int otsardb_replica_quorum(const otsardb_replica *replica);

/* Number of replica destinations currently managed by the handle (R). */
size_t otsardb_replica_destination_count(const otsardb_replica *replica);

/* The index of the destination promoted by the last successful
 * otsardb_replica_promote (-1 = none promoted yet). The session uses it to
 * reopen the promoted store as its primary (s3_database.cpp 0089 wiring). */
int otsardb_replica_promoted_index(const otsardb_replica *replica);

/* Open the Nth replica destination (0 = OTSARDB_S3_REPLICA_*, 1 =
 * OTSARDB_S3_REPLICA2_*, 2 = OTSARDB_S3_REPLICA3_*) as a READ-ONLY store with
 * the same rules as otsardb_replica_open_reader (zero writes to the database
 * root; the capability-probe verdict object outside the root is the only
 * write). the promoted session uses this to reopen the
 * destination that otsardb_replica_promoted_index selected. A destination
 * index whose environment set is absent fails closed (never silently opens
 * a different destination). */
otsardb_object_store *otsardb_replica_open_reader_at(int destination_index,
                                                  const char *database_root,
                                                  const char *database_id,
                                                  otsardb_store_probe_report *out_report,
                                                  char *err,
                                                  size_t err_size);

/* (promoted sessions): attach EVERY environment-configured
 * destination EXCEPT `exclude_index` as an owned replica destination of
 * `publisher` (the promoted destination is the session's primary now). Used
 * by the s3_database.cpp promote branch to extend the 0089 role-swap
 * semantics to all destinations: after the swap, the old primary AND every
 * remaining destination are replicated to by the promoted session. The
 * attached stores are opened through the S3 adapter and owned by the
 * publisher's replica handle. Returns 1 on success (including "no other
 * destinations configured"); 0 with a human message in `err` otherwise
 * (fail-closed: the promoted session must not silently drop a destination). */
int otsardb_replica_attach_env_others(struct otsardb_wal_publisher *publisher,
                                    int exclude_index,
                                    char *err,
                                    size_t err_size);

/* Run the Model B/C replica phase for one commit already durably published on
 * the primary (primary HEAD == commit_id at `generation`):
 *
 * 1. read + classify the replica HEAD (missing-at-genesis / parent / self
 * / lag / ahead / error) — a lagging or foreign replica fails closed;
 * 2. replicate metadata.json (only when absent on the replica);
 * 3. replicate every segment (encoded exactly like commit_publish.c —
 * content-addressed immutable PUT with GET+compare);
 * 4. replicate the manifest (GET the exact published bytes from the
 * primary, then immutable PUT);
 * 5. write the PREPARED ledger entry on BOTH destinations (CAS-guarded);
 * 6. CAS the replica HEAD to the same commit id (If-Match the replica's
 * own current etag / If-None-Match at genesis) — the single global
 * selection: every HEAD that exists points at the same commit id;
 * 7. write the COMMITTED ledger entry on BOTH destinations — the ack
 * barrier of the durability contract.
 *
 * Every store call reuses the bounded retry cycle
 * (otsardb_wal_publisher_retry_cycle: OTSARDB_PUBLISH_RETRIES /
 * OTSARDB_PUBLISH_RETRY_BASE_MS); re-attempts are idempotent (GET+compare,
 * content-addressed objects, fixed commit identity).
 *
 * (Model C): the phase runs per destination over the
 * handle's destination list with the quorum decision point at the end —
 * returns 1 when Q of M destinations (primary counts as 1) hold the commit
 * + COMMITTED ledger; a transport-class failure on one destination is
 * absorbed while the quorum is still reachable (the destination stays
 * lagging and the 0077 gap-replay catches it up on a later dual publish);
 * an authoritative failure (CONFLICT/NOT_FOUND/INVALID/UNSUPPORTED) on any
 * destination and a below-quorum transport result both return 0 with the
 * journal-level cause in *out_rc.
 *
 * Returns 1 on success (the durability contract's ack barrier reached for
 * the quorum); 0 with *out_rc carrying the journal-level cause — the caller
 * then marks the primary ledger ABORTED and fails the sync (the commit is
 * NOT acked; INCONCLUSIVE per the durability contract). */
int otsardb_replica_replicate_commit(otsardb_replica *replica,
                                   otsardb_object_store *primary_store,
                                   const char *database_root,
                                   const char *commit_id,
                                   uint64_t generation,
                                   uint64_t parent_generation,
                                   const char *parent_commit_id,
                                   const char *parent_manifest_sha256,
                                   const char *manifest_sha256,
                                   const char *primary_head_etag,
                                   const otsardb_segment_input *segments,
                                   uint32_t segment_count,
                                   otsardb_journal_result *out_rc);

/* Best-effort ABORTED ledger entry on the PRIMARY (0072 failure contract:
 * primary OK + replica FAIL -> the primary ledger marks ABORTED and the
 * sync fails). Errors are swallowed by contract (best-effort marker). */
void otsardb_replica_mark_aborted(otsardb_object_store *primary_store,
                                const char *database_root,
                                const char *commit_id,
                                uint64_t generation,
                                uint64_t parent_generation,
                                const char *manifest_sha256,
                                const char *primary_head_etag);

/* ---------------------------------------------------------------------------
 * — promotion / epoch fence / reconciliation
 * ( 0069 decision points 6-9).
 * ------------------------------------------------------------------------- */

/* 1 when the operator asked for promotion (OTSARDB_S3_PROMOTE=1): the session
 * should promote the replica destination to primary at open (raise its
 * epoch by CAS, run full recovery from it) instead of treating it as a
 * plain replica. The session-side wiring (s3_database.cpp) is a documented
 * follow-up; the library entry points below are the tested primitive. */
int otsardb_replica_promote_requested(void);

/* The per-destination epoch record (<root>/replica/epoch.json) as read:
 * {epoch, owner, generation} plus the object etag the CAS raise must match.
 * `exists` is 0 when the record is absent (a raise then creates it with
 * If-None-Match at epoch 1). */
typedef struct otsardb_replica_epoch_view {
    int exists;
    uint64_t epoch;
    uint64_t generation;
    char owner[OTSARDB_REPLICA_EPOCH_OWNER_MAX + 1];
    char etag[128];
} otsardb_replica_epoch_view;

/* Read the replica epoch record. Returns 1 on success (out->exists 0 for a
 * missing record — a legitimate "never created" state), 0 on store/format
 * errors with *out_rc carrying the journal-level cause. */
int otsardb_replica_epoch_read(otsardb_object_store *store,
                             const char *database_root,
                             otsardb_replica_epoch_view *out,
                             otsardb_journal_result *out_rc);

/* RAISE the destination epoch by CAS — the promotion fence (design section
 * 4: "Promotion = CAS raise (read current, write N+1 with If-Match)").
 *
 * When `current` is NULL the record is read first (absent -> If-None-Match
 * create at epoch 1, present -> If-Match N -> N+1, re-reading on a
 * concurrent 412 up to 3 attempts). When `current` is non-NULL the given
 * view is used VERBATIM (the deterministic split-brain drill: two
 * promotions that read the same view cannot both win — the second CAS
 * fails with PRECONDITION_FAILED and the raise reports CONFLICT).
 *
 * Returns 1 on success with *out_new_epoch set; 0 with *out_rc:
 * OTSARDB_JOURNAL_CONFLICT — a concurrent promotion won the CAS (split-brain
 * refused: re-read and retry);
 * OTSARDB_JOURNAL_AMBIGUOUS / ERROR / UNSUPPORTED / INVALID — store causes. */
int otsardb_replica_epoch_raise(otsardb_object_store *store,
                              const char *database_root,
                              const char *owner,
                              const otsardb_replica_epoch_view *current,
                              uint64_t *out_new_epoch,
                              otsardb_journal_result *out_rc);

/* Operator promotion (0069 decision point 6). `replica` is a handle over
 * the TO-BE-PROMOTED destinations; `old_primary_store` is the store the old
 * primary session wrote to.
 *
 * with a destination list the promotion picks the
 * destination with the NEWEST COMMITTED ledger/HEAD (the safe-last-commit:
 * max generation with a COMMITTED ledger entry there; ties keep the
 * earlier destination). otsardb_replica_promoted_index reports the pick.
 *
 * 1. Refuse while the old primary is lease-held/authoritative: its
 * <root>/lease.json is read on `old_primary_store` and judged with the
 * SAME expiry predicate as the lease module (TTL - skew margin from
 * the store's Last-Modified; an unknown Last-Modified presumes live).
 * A live lease means the old primary may still be writing — promoting
 * now would split the database. Promotion is a DISASTER-RECOVERY /
 * OFFLINE operator contract: stop the old primary (or wait
 * OTSARDB_LEASE_TTL for its lease to expire), then promote.
 * 2. Refuse when NO destination holds any COMMITTED commit (nothing to
 * promote — the operator must seed/reconcile first).
 * 3. Raise the picked destination's epoch via CAS (If-Match N -> N+1;
 * If-None-Match create when absent). A 412 from a concurrent promoter
 * refuses the promotion (split-brain drill: two concurrent promotes
 * cannot both win).
 * 4. Run FULL recovery from the promoted store into `local_db_path`
 * (otsardb_recovery_restore_store: validate the whole chain and
 * materialise the local image). The former destination is now primary.
 *
 * On success the handle adopts the raised epoch (its later
 * otsardb_replica_replicate_commit fence matches the destination record) and
 * *out_epoch (optional) receives it. `lease_ttl_secs` 0 reads
 * OTSARDB_LEASE_TTL (default 30 s). Returns 1 on success; 0 with a human
 * message in `err` and the journal-level cause in *out_rc (CONFLICT =
 * authoritative refusal — lease-held primary or lost promotion race). */
int otsardb_replica_promote(otsardb_replica *replica,
                          otsardb_object_store *old_primary_store,
                          const char *local_db_path,
                          uint64_t lease_ttl_secs,
                          char *err,
                          size_t err_size,
                          otsardb_journal_result *out_rc);

/* The session epoch the handle currently holds on its destination (the
 * value its next otsardb_replica_replicate_commit fence must match). */
uint64_t otsardb_replica_epoch(const otsardb_replica *replica);

/* ---------------------------------------------------------------------------
 * — read-replica sessions (0069 decision point
 * "read-replica";).
 *
 * A READER session opens the database reading objects from the REPLICA
 * destination (the replica holds a full, COMMITTED copy of every acked
 * commit). The read path is the SAME OtsarDB format on the replica store:
 * metadata.json / HEAD / commits chain / segments / CHECKPOINT.json are
 * byte-identical to the primary's, so recovery, lazy hydration, cache
 * reuse and the capability probe work against the replica normally.
 * ------------------------------------------------------------------------- */

/* 1 when the operator asked for a READ-ONLY session over the replica
 * destination (OTSARDB_S3_READ_FROM_REPLICA=1). Any other value (0, garbage,
 * absent) means a normal primary session (fail closed: a typo never
 * switches the read path). */
int otsardb_replica_read_requested(void);

/* Open the replica destination 0 (OTSARDB_S3_REPLICA_*) as a READ-ONLY store
 * for a read-replica session. Parses the OTSARDB_S3_REPLICA_* environment
 * exactly like otsardb_replica_open (ENDPOINT and BUCKET required; REGION
 * defaults to us-east-1; ACCESS_KEY/SECRET_KEY optional; the primary's
 * OTSARDB_S3_PREFIX applies), creates the store instance through the S3
 * adapter (compiled only when the build defines OTSARDB_ENABLE_S3 — otherwise
 * a fail-closed stub) and runs the capability probe (retry
 * policy, fail-closed; the 0043 verdict cache is not consulted — the
 * replica always probes, same rule as otsardb_replica_open).
 *
 * Unlike otsardb_replica_open this is the READ path: it performs ZERO writes
 * to the database root — no epoch record bootstrap, no ledgers, no HEAD,
 * no lease (the capability-probe verdict object is the only write, outside
 * the database root, identical to any other open). The caller owns the
 * returned store (release with otsardb_s3_store_destroy).
 *
 * Fail-closed: any probe/store failure returns NULL with a human message
 * in `err`; out_report (optional) receives the probe verdict so the caller
 * can derive the write strategy for publisher bookkeeping (the read-only
 * session never publishes). */
otsardb_object_store *otsardb_replica_open_reader(const char *database_root,
                                              const char *database_id,
                                              otsardb_store_probe_report *out_report,
                                              char *err,
                                              size_t err_size);

#ifdef __cplusplus
}
#endif

#endif
