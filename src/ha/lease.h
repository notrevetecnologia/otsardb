#ifndef OTSARDB_HA_LEASE_H
#define OTSARDB_HA_LEASE_H

#include "object_store.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* M2 writer lease (phase-2 design). Exactly one process may publish commits
 * for an S3-backed OtsarDB database; the lease object at `<root>/lease.json`
 * records the current owner, a monotonic generation and (
 * optional) the owner's HTTP forward endpoint:
 *
 * {"owner":"<instance-id>","generation":N,"address":"host:port"}
 *
 * The `address` field is written at claim time when the claimant runs a
 * forward server (CLI `--forward-port`); it is what non-leader sessions
 * read to forward write SQL to the leader (see forward.h). Old leases
 * without the field parse fine (address empty -> forwarding disabled).
 *
 * Semantics (Option A, see design-doc implementation note):
 * - `is_live` is judged from the local client clock (`claimed_at` +
 * TTL - skew margin), NOT from server time; the CAS generation is the
 * true fence.
 * - A competitor's claim only takes over an existing lease when the
 * store exposes the object's Last-Modified (`otsardb_store_object
 * .last_modified_secs`) and it proves expired; when the store does not
 * expose it (0 = unknown) the lease is presumed live.
 * - Phase 2: the holder renews every TTL/3 from a
 * background thread and demotes on CAS conflict / consecutive
 * transport errors; a non-holder polls via `otsardb_lease_claim` and
 * takes over an expired lease, then re-runs recovery.
 *
 * Clock-skew envelope (failure-mode audit F2): takeover
 * judges expiry from the store's Last-Modified using the CANDIDATE's own
 * clock, while the holder judges its own liveness from its own clock. A
 * candidate whose clock is ahead by up to OTSARDB_LEASE_SKEW_MARGIN_SECS
 * can therefore take over while the holder still believes it is live; the
 * holder notices only when its own clock reaches TTL - skew or its next
 * renew CAS-conflicts (within TTL/3). The lease-level takeover window is
 * consequently bounded by OTSARDB_LEASE_SKEW_MARGIN_SECS + TTL/3. This
 * window is a LIVENESS window only: the publish fence (wal_publisher.c
 * refresh_head + the 0024 generation anchor) refuses any stale-base
 * publish once a competitor has advanced HEAD, so the DATA-LOSS window is
 * zero regardless of clock skew (see docs/architecture/CONSISTENCY_MODEL.md
 * §5 and).
 *
 * Thread safety (phase 2): the struct carries an internal mutex. The
 * renew thread mutates `etag` / `claimed_at` / `lost`; the publish fence
 * reads `lost` / `claimed_at` via `otsardb_lease_is_live`. All internal
 * state access is mutex-guarded; network I/O is never performed while
 * the mutex is held (state is snapshotted, then committed under the
 * lock), so the fence is never blocked by a slow store round trip.
 * `otsardb_lease_destroy` must be called only when no other thread can
 * touch the lease (e.g. after joining the renew thread). */

#define OTSARDB_LEASE_TTL_DEFAULT_SECS 30u
/* Operational minimum for the OTSARDB_LEASE_TTL environment variable;
 * enforced by the caller (s3_database.cpp). lease.c itself fails closed:
 * TTLs at or below the skew margin only make leases expire faster. */
#define OTSARDB_LEASE_TTL_MIN_SECS 10u
/* Conservative liveness margin subtracted from TTL when judging expiry
 * from a clock (Last-Modified or local claimed_at). */
#define OTSARDB_LEASE_SKEW_MARGIN_SECS 5
#define OTSARDB_LEASE_OWNER_MAX 63
#define OTSARDB_LEASE_ADDRESS_MAX 255
#define OTSARDB_LEASE_KEY_MAX 1024
#define OTSARDB_LEASE_JSON_MAX 512

typedef struct otsardb_lease otsardb_lease;

/* claim() result states (mirrors the design doc's return contract). */
#define OTSARDB_LEASE_CLAIMED 1
#define OTSARDB_LEASE_HELD 0
#define OTSARDB_LEASE_ERROR (-1)

/* Claim (or adopt) the writer lease for `database_root`.
 *
 * absent -> create `<root>/lease.json` with If-None-Match:*
 * at generation 1;
 * same owner -> adopt via CAS If-Match at the same generation
 * (re-anchors liveness; reconnect path);
 * other owner -> CAS If-Match takeover at generation+1 only when
 * the object's Last-Modified proves the lease
 * expired (TTL - skew); otherwise NOT claimed;
 * store error -> error (NOT held).
 *
 * ttl_secs: 0 uses OTSARDB_LEASE_TTL_DEFAULT_SECS. TTLs below
 * OTSARDB_LEASE_TTL_MIN_SECS are intentionally NOT clamped here (they only
 * shorten the lease); the operational minimum is enforced where the
 * OTSARDB_LEASE_TTL environment variable is read.
 *
 * Returns a live lease on success (non-NULL) or NULL. The outcome is
 * additionally reported through *claim_state_out (OTSARDB_LEASE_CLAIMED /
 * HELD / ERROR, optional) and *epoch_out receives the generation claimed.
 * When the lease is held by another live owner, holder_out/holder_cap
 * (optional) receive the current owner and address_out/address_cap
 * (optional) receive the owner's published forward endpoint
 * ("host:port"; empty when the holder did not publish one). The optional
 * `address` argument is written into the lease object at create/adopt/
 * takeover time when non-NULL and non-empty: it must be
 * plain text without '"' or '\\'.
 */
otsardb_lease *otsardb_lease_claim(otsardb_object_store *store,
                               const char *database_root,
                               const char *instance_id,
                               const char *address,
                               uint64_t ttl_secs,
                               uint64_t *epoch_out,
                               int *claim_state_out,
                               char *holder_out,
                               size_t holder_cap,
                               char *address_out,
                               size_t address_cap);

/* Re-anchor the lease with CAS If-Match at the same generation.
 * 0 = renewed; 1 = CAS conflict (lease lost to another writer, permanently
 * demoted); 2 = transport/store error (lease state unchanged). Renewal
 * preserves the address published at claim time. */
int otsardb_lease_renew(otsardb_lease *lease);

/* 1 = the lease carries a published forward endpoint; copies it into
 * out/out_cap ("host:port"). 0 = no address published. Thread-safe. */
int otsardb_lease_address(const otsardb_lease *lease, char *out, size_t out_cap);

/* 1 = still the owner and not expired (local clock, TTL - skew margin);
 * 0 = lost (CAS conflict) or expired. Thread-safe. */
int otsardb_lease_is_live(const otsardb_lease *lease);

/* Shared expiry predicate: an object with age `age_secs` seconds is
 * expired once the age reaches TTL - skew margin (clamped at 0 so tiny
 * TTLs fail closed). Used by claim() for takeover, by is_live() for the
 * holder, and by the phase-2 candidate poller to judge a foreign lease's
 * age (`now - last_modified_secs`) before deciding to CAS. */
int otsardb_lease_age_is_expired(uint64_t ttl_secs, int64_t age_secs);

/* Best-effort handoff: deletes the lease object while the releaser is
 * still the live owner. A lost or expired lease is left untouched so a
 * faster takeover is never destroyed. */
void otsardb_lease_release(otsardb_lease *lease);

void otsardb_lease_destroy(otsardb_lease *lease);

#ifdef __cplusplus
}
#endif

#endif
