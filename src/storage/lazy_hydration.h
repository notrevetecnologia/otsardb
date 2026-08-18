#ifndef OTSARDB_LAZY_HYDRATION_H
#define OTSARDB_LAZY_HYDRATION_H

#include "cipher.h"
#include "journal.h"
#include "object_store.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif/*
 * Lazy hydration (extended by): a cold remote
 * open validates the manifest chain and materialises only the newest
 * checkpoint base image, marking every other segment LAZY. SQLite page reads
 * that fall on unmaterialised pages are hydrated on demand from the newest
 * claiming segment (manifest v4 page sets, fail-closed SQLITE_IOERR).
 *
 * (frame-level range GETs): when the claiming segment's
 * manifest records "frame_offsets" and the store supports RANGE_GET, the
 * hydrator fetches exactly the needed frame blocks with byte-range GETs
 * (consecutive pages coalesced into one request). Segments without offsets
 * (v1-v3, or v4 published without them) and stores without RANGE_GET keep
 * the whole-object fetch path with full SHA-256 + size verification.
 *
 * Integrity: a byte-range slice cannot be verified against the manifest's
 * whole-object SHA-256. Sliced fetches inherit trust from the
 * chain-authenticated manifest (immutable, content-addressed objects) and
 * are additionally validated structurally: exact fetched range size, block
 * header fields, decompressed frame size == 8 + page_size, per-block CRC-32
 * of the decompressed frame, and the frame's page number must equal the
 * manifest page. SQLite's own header/page-1 checks cross-check the first
 * page where available. Full-object SHA-256 verification remains in force
 * for every whole-object download (fallback paths). Any failure fails
 * closed (SQLITE_IOERR); a wrong page is never served silently.
 *
 * The hydrator is created by otsardb_lazy_restore_store and is owned by the
 * caller (one per S3 session). All hydrate/mark calls happen on the SQLite
 * calling thread; the lease-thread takeover path re-uses the same struct via
 * the in-place rebuild form of otsardb_lazy_restore_store.
 *
 * (sequential read-ahead): in the miss path, when recent
 * demand misses are consecutive page numbers (the sequential-scan
 * signature), the prefetch pool fetches the NEXT OTSARDB_LAZY_PREFETCH pages
 * (default 8, clamp 1..64, 0 = off) of the same segment in the background,
 * one coalesced byte-range GET per contiguous run, restricted to pages that
 * exist in the claiming segment's page set and are not yet materialised.
 * Whole-object (non-frame-offset) segments are already eagerly fetched by
 * the pool in ordinal order, so prefetch jobs target frame-offset segments
 * only. Random access never triggers a prefetch and demand-fetch semantics
 * are unchanged. The demand path never issues a redundant fetch for a page
 * an in-flight window already covers: it waits for the window (same wire
 * cost as its own GET would be) and re-checks; a dropped window falls back
 * to the demand fetch. Prefetch failures are dropped silently — the next
 * explicit miss re-fetches; only demand-fetch failures are sticky (fail
 * closed).
 *
 * (coalescing refinement + per-read-path counters):
 * the demand path merges same-segment runs whose byte distance is at most
 * OTSARDB_S3_COALESCE_MAX (default 4096, 0 = strict 0059 consecutive-page
 * behavior) into ONE byte-range GET covering both runs (the gap bytes are
 * fetched and structurally validated, but only applied to pages that are
 * still unmaterialised and still claimed by that segment — newest-claimant
 * and bitmap rules are unchanged). Coalesced GETs of one hydrate call are
 * issued in parallel, up to OTSARDB_LAZY_PARALLEL workers. Per-session
 * counters are kept for every read path (see
 * otsardb_lazy_hydration_stats); with OTSARDB_S3_READ_STATS=1 one canonical
 * "otsardb: read-stats ..." line is printed to stderr at session close
 * (otsardb_lazy_destroy) — no s3_database.cpp change is required for the
 * env-gated print; callers that want the line emitted elsewhere use
 * otsardb_lazy_hydration_stats_get + otsardb_lazy_hydration_stats_print
 * (the documented s3_database.cpp wiring point for the coordinator).
 *
 * (RAM page cache): a
 * session-scoped in-memory page cache (page_no -> page_size bytes) with
 * strict LRU eviction under OTSARDB_S3_RAM_CACHE_MB (default 64, 0 = off).
 * The demand path resolves every page against the RAM tier FIRST (before
 * the bitmap): a cache hit skips the S3 fetch AND the decode, and the page
 * is only written into the local image when the bitmap says it is not yet
 * materialised. Prefetch jobs fill the cache too, and pages applied by the
 * whole-object paths (v1-v3 / v4-without-offsets / no-RANGE_GET stores)
 * are cached at apply time.
 *
 * Integrity: the cache holds ONLY verified bytes — every page was
 * SHA-256/CRC-32 validated at fetch time (chain-authenticated manifest +
 * per-block structural validation + CRC-32 on the frame path; exact size +
 * SHA-256 on the whole-object path) before it was applied and cached.
 * Invalidation rules keep cached bytes equal to the local image's newest
 * state: otsardb_lazy_mark_range removes the marked page (SQLite wrote the
 * image), otsardb_lazy_mark_all clears the tier, and lazy_release_state
 * (lease-takeover rebuild / destroy) clears it — cached bytes are only
 * valid for the hydrator's restore-time head state, and the newest-claimant
 * rule is preserved because the hydrator's segment index is immutable
 * between rebuilds.
 *
 * VFS boundary (documented per R3): the cache serves the HYDRATION layer's
 * internal page resolution only. Pages are still materialised into the
 * local image before the VFS reads them — the local image remains the
 * source of truth for SQLite xRead, and local_vfs behavior is unchanged.
 * The VFS hook API (otsardb_lazy_hydrate_bytes -> otsardb_lazy_hydrate_range)
 * has no page-bytes-out channel, so the RAM tier cannot be consumed
 * directly by the VFS; it only makes the hydrate path cheaper.
 *
 * Stats (OTSARDB_S3_READ_STATS=1): ram_hits / ram_misses are session
 * counters resolved by the RAM tier (a hit = the page was found in the
 * cache and the read was served without a store GET + decode; a miss = the
 * cache was consulted and did not hold the page). They are printed on a
 * SECOND "otsardb: read-stats ram_hits=N ram_misses=N" line at session close
 * (the canonical 0082 line is byte-identical for backwards compatibility —
 * the 0082 format test pins it).
 */

typedef struct otsardb_lazy_hydrator otsardb_lazy_hydrator;

/*
 * Validate the manifest chain (same checks as recovery) and prepare a lazy
 * database image at database_path:
 * - HEAD/db.json/CHECKPOINT.json are read (the caller may pass a head it
 * already read via `head_in` to avoid the extra GET);
 * - the chain is validated exactly like recovery (hash links, generation
 * continuity, page_size/database_id/commit_id checks, genesis);
 * - when a valid checkpoint covers the head, its page groups are
 * materialised (pages 1..covered_size are NOT bitmap-marked: pages
 * claimed by segments newer than the covered generation must still
 * resolve to those segments; unclaimed pages are marked on demand).
 * Completeness accounting counts the base-covered unclaimed pages
 * explicitly (LH-2);
 * - the file is created/truncated at head database_size_pages * page_size;
 * - all other segments are kept LAZY in the hydrator's index.
 *
 * *out_hydrator may be NULL (a new hydrator is created and returned) or an
 * existing hydrator (in-place rebuild for lease takeover). Returns 1 on
 * success; on failure nothing usable is left behind (a created hydrator is
 * destroyed, a rebuilt one is left poisoned so later hydrates fail closed)
 * and 0 is returned.
 *
 * Note: the head manifest's post_apply_checksum is NOT verified at open in
 * lazy mode (that would require materialising the whole image); every
 * lazily-fetched segment is hash+size verified against its manifest instead.
 */
/*
 * otsardb_lazy_restore_store_ex additionally decrypts the
 * segments the chain-authenticated manifest marks encrypted (top-level
 * "segment_enc" array) with the given cipher — both on the whole-object
 * fetch path and, defensively, before any frame-range read (encrypted
 * segments are never recorded with frame_offsets, so lazy hydration
 * whole-fetches them; the cipher is still required and tag-verified). A
 * missing or wrong cipher fails closed (SQLITE_IOERR on hydrate, failed
 * restore on open). The legacy otsardb_lazy_restore_store falls back to the
 * thread-local session cipher (NULL = plaintext, byte-identical pre-0079
 * behavior).
 */
int otsardb_lazy_restore_store_ex(otsardb_object_store *store,
                                const char *database_root,
                                const char *database_path,
                                const otsardb_head *head_in,
                                otsardb_lazy_hydrator **out_hydrator,
                                const otsardb_cipher *cipher);

int otsardb_lazy_restore_store(otsardb_object_store *store,
                             const char *database_root,
                             const char *database_path,
                             const otsardb_head *head_in,
                             otsardb_lazy_hydrator **out_hydrator);

/*
 * Ensure every page in [first_page, last_page] (1-based, inclusive) is
 * materialised in the local image, fetching/verifying/decoding/applying
 * owning segments on demand. Returns 1 on success, 0 on ANY failure (network,
 * hash/size mismatch, unresolvable page, decode error) — the caller must
 * surface SQLITE_IOERR, never a partial page.
 */
int otsardb_lazy_hydrate_range(otsardb_lazy_hydrator *hydrator,
                             uint32_t first_page,
                             uint32_t last_page);

/*
 * Byte-range variant used by the VFS read hook: hydrates every page
 * intersecting [offset, offset+amount).
 */
int otsardb_lazy_hydrate_bytes(otsardb_lazy_hydrator *hydrator,
                             long long offset,
                             int amount);

/* True when every page <= head database_size_pages is materialised (the
 * local image is then a complete copy of the head state). Used to gate the
 * local-cache save so a partial image is never cached as authoritative.
 *
 * (LH-2): pages covered by a checkpoint base that are claimed
 * by NO segment newer than the checkpoint count as materialised without a
 * bitmap mark (the base image bytes are their newest state). Pages claimed by
 * a newer segment, and every page beyond the covered range, still require a
 * mark — so the gate never accepts a partial image. */
int otsardb_lazy_is_complete(const otsardb_lazy_hydrator *hydrator);

/* SQLite-side marking: pages written by SQLite itself (main-db xWrite) and
 * full rewrites (checkpoint build) must be marked so the hydrator never
 * overwrites them with older segment data. */
void otsardb_lazy_mark_range(otsardb_lazy_hydrator *hydrator,
                           uint32_t first_page,
                           uint32_t last_page);
void otsardb_lazy_mark_bytes(otsardb_lazy_hydrator *hydrator,
                           long long offset,
                           int amount);
void otsardb_lazy_mark_all(otsardb_lazy_hydrator *hydrator);
void otsardb_lazy_clear_beyond(otsardb_lazy_hydrator *hydrator,
                             uint32_t page_count);
void otsardb_lazy_clear_beyond_bytes(otsardb_lazy_hydrator *hydrator,
                                   long long bytes);

void otsardb_lazy_destroy(otsardb_lazy_hydrator *hydrator);

/* Gated stderr diagnostics ("otsardb: timing ..." when OTSARDB_S3_TIMING=1). */
int otsardb_lazy_timing_enabled(void);
void otsardb_lazy_timing_note(const char *label, double milliseconds);

/*
 * Per-read-path counters. One snapshot per
 * session, owned by the hydrator, accumulated atomically from every thread
 * (demand thread, prefetch pool workers, coalesced-GET workers). They cover
 * the hydration/fetch paths only — restore-time chain reads (HEAD, db.json,
 * checkpoint, manifests) are not counted.
 *
 * total_gets every segment-fetch GET (byte-range + whole object)
 * range_gets byte-range GETs (demand + prefetch)
 * bytes_fetched bytes received on successful GETs (0 on failures)
 * demand_misses pages materialised by the demand path (inline frames,
 * whole-object task applies, unknown-segment applies,
 * checkpoint-base coverage marks)
 * prefetch_fetches pages materialised by prefetch jobs
 * prefetch_hits demand reads served by a page a completed prefetch
 * job materialised (provenance approximated via the
 * job's DONE window coverage, not tracked per page)
 * coalesced_runs range GETs covering more than one page (0059
 * consecutive-page runs + 0082 byte-window merges)
 * sequential_streaks times the sequential read-ahead detector triggered a
 * prefetch schedule (miss streak >= 2; includes
 * schedules dropped by window dedup)
 * hydration_ms wall time spent in otsardb_lazy_hydrate_range
 * ram_hits pages whose hydrate resolution was served by the RAM
 * page cache (counted only when
 * OTSARDB_S3_RAM_CACHE_MB > 0)
 * ram_misses pages the RAM page cache was consulted for and did
 * not hold (counted only when OTSARDB_S3_RAM_CACHE_MB > 0)
 *
 * OTSARDB_S3_READ_STATS=1 prints one canonical line to stderr at session
 * close (otsardb_lazy_destroy):
 * otsardb: read-stats total_gets=N range_gets=N bytes_fetched=N
 * demand_misses=N prefetch_fetches=N prefetch_hits=N coalesced_runs=N
 * sequential_streaks=N hydration_ms=N
 * followed by the RAM tier line:
 * otsardb: read-stats ram_hits=N ram_misses=N
 */
typedef struct otsardb_lazy_hydration_stats {
    uint64_t total_gets;
    uint64_t range_gets;
    uint64_t bytes_fetched;
    uint64_t demand_misses;
    uint64_t prefetch_fetches;
    uint64_t prefetch_hits;
    uint64_t coalesced_runs;
    uint64_t sequential_streaks;
    uint64_t hydration_ms;
    uint64_t ram_hits;
    uint64_t ram_misses;
} otsardb_lazy_hydration_stats;

/* Snapshot the session counters. Returns 1 on success (best-effort copy,
 * safe from any thread while the hydrator is alive). */
int otsardb_lazy_hydration_stats_get(const otsardb_lazy_hydrator *h,
                                   otsardb_lazy_hydration_stats *out);

/* Render the canonical one-line "otsardb: read-stats ..." record (with
 * trailing newline). Returns the number of characters that would have been
 * written (excluding the terminator), like snprintf. */
int otsardb_lazy_hydration_stats_format(const otsardb_lazy_hydration_stats *stats,
                                      char *buf,
                                      size_t buf_size);

/* Print the canonical line to stderr. */
void otsardb_lazy_hydration_stats_print(const otsardb_lazy_hydration_stats *stats);

/* Render the RAM tier line (one "otsardb: read-stats
 * ram_hits=N ram_misses=N" record with trailing newline). The canonical
 * 0082 line is intentionally NOT extended (the 0082 format test pins its
 * exact bytes); this second line carries the RAM cache counters and is
 * printed by otsardb_lazy_destroy right after the canonical line when
 * OTSARDB_S3_READ_STATS=1. Returns the number of characters that would have
 * been written (excluding the terminator), like snprintf. */
int otsardb_lazy_hydration_stats_format_ram(const otsardb_lazy_hydration_stats *stats,
                                          char *buf,
                                          size_t buf_size);

#ifdef __cplusplus
}
#endif

#endif
