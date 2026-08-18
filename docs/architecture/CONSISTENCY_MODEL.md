# OtsarDB — Consistency Model (Read Contract)

> This document describes the consistency model **as it is implemented today**,
> including both consistency modes. It does not propose a
> change. The snapshot-mode sections were written against commit
> `58c55f79ec64169515c4f6de56bae17332e35480` (`desenvolvimento`), verified by
> **source inspection on 2026-08-13**; the FRESH-mode sections were added with
> the FRESH consistency mode and verified by **execution on 2026-08-13**
> (local MinIO, `build-s3`; deterministic memory-store suite F1–F8; live
> cross-process e2e).
> Each claim carries an evidence classification per the project's evidence
> rules: `OBSERVED` here means "code path confirmed by inspection" (the exact
> function and line are cited) or "executed and recorded" for the FRESH-mode
> sections, `PROJECTED` means "derived count", `UNTESTED` means "not yet
> observed". Measured numbers are quoted from the record that captured them,
> together with its date/binary.

## 0. Two modes, one engine

`OTSARDB_CONSISTENCY` selects the read-freshness contract at open
(`OBSERVED` `s3_database.cpp` `open_session_common`):

| Mode | Env value | Contract | Default |
|---|---|---|---|
| **SNAPSHOT** | absent or `snapshot` | session snapshot pinned at open (this document's §1–§10, byte-identical pre-FRESH) | **yes** |
| **FRESH** | `fresh` | one remote HEAD GET at the start of every `otsardb_exec` statement; a generation advance re-materialises the session image before the statement executes | no |

Any other value **fails the open** (`OBSERVED`: CLI exit 1 with
`OTSARDB_CONSISTENCY must be 'snapshot' or 'fresh' ...`).

## 1. The model, named as-is

The default (SNAPSHOT) read model is:

> **session snapshot pinned at open.**

A session opens against a specific S3 generation (the remote `HEAD` read once
at open), materialises a local execution image at that generation, and then
serves every read from that local image until the session ends. The session
does **not** re-check the remote `HEAD` on reads and has **no** background
HEAD watcher. The snapshot can only advance via a reopen, or via a write
attempt (which re-reads `HEAD` and either adopts the new generation or fails
with `SQLITE_BUSY`).

This is a *snapshot-read* model with *last-writer-wins-on-reopen* visibility,
not an always-fresh read model. No fancier name is warranted by the code.

## 2. The invariant in one picture

```text
                      S3 (authoritative)
      HEAD (gen G)  ── commit manifests ── immutable segments
           │                    (hash-linked chain)
           │  read ONCE at open (and on write attempts)
           ▼
   local execution image (materialized SQLite db + WAL)   ← ALL reads
                                                             served here
```

After open, the local image is the *only* thing the SQL engine touches for
reads (`OBSERVED` — `src/storage/local_vfs.c:94` `otsardb_io_read` delegates to
the operating-system VFS `xRead`; the only remote-capable hook is the optional
lazy-hydration page hook, which fires only for pages that are not yet
materialised, `local_vfs.c:100`).

## 3. Per-operation read/write contract

SNAPSHOT mode (the default):

| Operation | Remote HEAD reads | Remote data reads (GET) | Remote writes (PUT) | Notes |
|---|---:|---:|---|---|
| **Hot read** (page already in local image / RAM) | 0 | 0 | 0 | `OBSERVED` `local_vfs.c:94-108`. Fully local. |
| **Warm read** (reopen, unchanged HEAD, cache reuse) | 1 (HEAD) | 0 segments/manifests | 0 | `OBSERVED` `s3_database.cpp` open path + cache probe. |
| **Cold read** (reopen, empty/advanced cache) | 1 (HEAD) | manifest chain + checkpoint + segments | 0 | `OBSERVED` open path, `recovery.c`, `lazy_hydration.c`. |
| **Cold page miss** (lazy hydration) | 0 | 1 coalesced Range GET | 0 | `OBSERVED` `lazy_hydration.c`; coalescing enabled by default. |
| **Read-your-writes** | 0 | 0 | on commit only | `OBSERVED` `wal_publisher.c:1095-1097`. See §4. |
| **Cross-instance visibility** | on reopen or write attempt | — | — | `OBSERVED` generation fence `wal_publisher.c:1530-1539`. See §5. |

FRESH mode (`OTSARDB_CONSISTENCY=fresh`; `OBSERVED` by the
deterministic counting test F3 and the live e2e):

| Operation | Remote HEAD reads | Notes |
|---|---:|---|
| **Every statement** (`otsardb_exec`) | exactly 1 | `OBSERVED` F3 (counting store): steady-state cost is ONE HEAD GET per statement, unconditionally — "1 HEAD per statement, period". Transient HEAD failures retry per the standard retry policy; final failure fails the statement (fail-closed). |
| **Statement inside an explicit transaction** | 0 | The check is deferred while `sqlite3_get_autocommit()==0` — the snapshot stays pinned inside the transaction (ACID); the first statement after COMMIT re-checks (`OBSERVED` F8). |
| **Generation unchanged** | 1 (the check) | Nothing else happens; hot reads below stay local. |
| **Generation advanced** | 1 + ladder | The session re-materialises at the new HEAD with the open-path ladder (exact-match cache probe → delta → lazy/full fallback), swaps the local image (takeover pattern: discard files, restore into the same path, reset SQLite's page cache, re-anchor the publisher fence), then executes the statement (`OBSERVED` live e2e: `fresh mode: rematerialized at generation 3 (complete image)`). |
| **Own commit (read-your-writes)** | 1 (the check) | The publisher already adopted the generation (`current_generation == local_image_generation == head`): the anchor advances WITHOUT re-materialising (`OBSERVED` F7 — no swap diagnostic). |
| **HEAD read fails after retries** | retries then fail | The statement fails with the mapped error — fresh mode promises the latest state, so it fails closed instead of serving the stale snapshot (`OBSERVED` F6). |

### 3.1 Hot read — zero remote work

Once the local image is materialised, `SELECT`/`UPDATE` page reads go straight
to the OS VFS. `otsardb_io_read` (`local_vfs.c:94`) does **zero** store calls
and **zero** HEAD calls. The `page_hydrate_hook` (`local_vfs.c:100-105`) is the
only path that can reach S3 from a read, and it is installed only in lazy
sessions and only for the `SQLITE_OPEN_MAIN_DB` file — a page that is already
present is served locally.

### 3.2 Warm read — cache reuse (unchanged HEAD)

At open the session reads the remote `HEAD` once
(`otsardb_journal_read_head`, `s3_database.cpp:1301`) and, when cache reuse is
enabled (`OTSARDB_CACHE_REUSE`, on by default — `s3_database.cpp:1318-1320`),
probes the saved local image (`otsardb_local_cache_probe`,
`s3_database.cpp:1333`). The probe requires an **exact** match of
`generation` + `commit_id` + `commit_sha256` (`local_cache.h:31`). On a match the
saved image is copied into the session and the manifest-chain recovery is
skipped entirely — the warm open does not fetch a single segment or manifest.

> Naming note: the environment variable is `OTSARDB_CACHE_REUSE`. The
> identifier family was renamed early in development; no legacy spelling
> remains in `src/`.

### 3.3 Cold read — open + recovery / lazy hydration

When there is no cache or the HEAD moved, the session reconstructs state:

- **Full recovery** (`recovery.c:773`) — reads `db.json`, `HEAD`, walks the
  manifest chain, applies every segment oldest-first into a new image.
- **Lazy hydration** (`lazy_hydration.c:3276`) — validates the chain, materialises
  only the newest checkpoint base, and fetches the remaining segments on page
  misses (coalesced Range GETs).
- **Delta materialization** (`recovery.c:912`, wired at `s3_database.cpp:1379`) —
  when a saved image exists at generation `G < HEAD`, applies only segments
  `G+1..HEAD` on top of it.

## 4. Read-your-writes

**Yes.** A session that commits sees its own writes without a remote round
trip. On a successful publish the publisher advances both its cached generation
**and** its local-image anchor to the newly published generation
(`OBSERVED` — `wal_publisher.c:1095-1097`):

```c
publisher->current_generation = result.head.generation;
if (publisher->local_image_generation_known) {
    publisher->local_image_generation = result.head.generation;
}
```

The committed pages already live in the session's local WAL/image, so the next
read returns them locally. The generation fence therefore does *not* trip on
the session's own commits — the local image and the cached HEAD stay in lockstep
for the writing session.

## 5. Cross-instance visibility

A session does **not** notice a commit made by another instance on read. It
notices it only in two places:

1. **Reopen** — the open path re-reads `HEAD` (`s3_database.cpp:1301`), so a new
   session (or the same process opening again) sees the newest generation.
2. **Write attempt** — every WAL sync calls `refresh_head`
   (`wal_publisher.c:1522`), which re-reads `HEAD`; if the remote generation
   advanced beyond the session's local-image anchor, the write is refused with
   `SQLITE_BUSY` (generation fence, `wal_publisher.c:1530-1539`). A write that
   races a competing writer is additionally fenced by the HEAD compare-and-swap
   (`If-Match` on the previous ETag, `journal.c:180`), which fails with
   `CONFLICT`.

There is **no HEAD polling and no watcher** in SNAPSHOT mode (`OBSERVED` —
the only background sleep loop is the writer-lease thread; it renews/claims
the writer lease and does not read for read-freshness).
`otsardb_wal_publisher_refresh_head` has no call site outside the
write/retry paths and a unit test. FRESH mode performs a
**synchronous** HEAD check per statement — it deliberately has **no
background watcher either** (an optional `OTSARDB_FRESH_POLL_MS` watcher was
considered and not built; the per-statement check is the contract).

## 5.1 Lease clock-skew envelope (failure-mode audit F2)

The M2 writer lease (`lease.h/lease.c`, design doc 0014) uses two different
clocks to judge the same lease:

- the **holder** judges its own liveness from its local client clock
  (`claimed_at`, re-anchored by every renew), expiring at `TTL - skew`;
- a **takeover candidate** judges the holder's lease expired from the
  store's `Last-Modified` (`now - last_modified >= TTL - skew`) using the
  candidate's OWN clock.

A candidate whose clock is ahead by up to `OTSARDB_LEASE_SKEW_MARGIN_SECS`
can therefore take over while the holder still believes it is live; the
holder notices at the earliest of its own clock reaching `TTL - skew` or
its next renewal CAS-conflicting (renewals run every `TTL/3`). The
lease-level takeover (liveness) window is therefore bounded by:

```text
OTSARDB_LEASE_SKEW_MARGIN_SECS + TTL/3
```

with the default TTL (30 s) giving `5 + 10 = 15 s` and the operational
minimum TTL (10 s) giving `5 + 3 = 8 s` (PROJECTED; the margin is the
designed worst-case clock skew and TTL/3 the worst-case renewal notice
latency). This is a *liveness* window only: inside it the old holder may
still believe it is the writer and the store has not yet objected.

**The data-loss window is zero, regardless of clock skew**, because every
publish is fenced by the generation anchor in
addition to the lease: a sync first re-reads HEAD
(`otsardb_wal_publisher_refresh_head`, `wal_publisher.c:1522`) and is
refused with `SQLITE_BUSY` when the remote generation is beyond the local
execution image's generation (`wal_publisher.c:1530-1539`) — before any
object upload. A fenced-out holder's stale-base commit is therefore refused
even while its own lease still reports live (`OBSERVED`, deterministic
regression fence-skew test: the same holder-lease
state that CONTROL proves permits a stale publish when HEAD has not advanced
is refused by MAIN as soon as the winner has advanced HEAD).

## 6. Snapshot scope

- **Within a transaction** — ordinary SQLite ACID on the local image/WAL: the
  snapshot is the generation pinned at open; a single transaction reads a
  consistent view of it. In FRESH mode the per-statement check is deferred
  while a transaction is open, so a FRESH transaction is likewise pinned from
  BEGIN to COMMIT (`OBSERVED`, F8).
- **Within a session** — SNAPSHOT mode: the snapshot is fixed at open for the
  session's lifetime. Reads never observe a newer remote generation; writes
  either extend the pinned snapshot (single-writer) or fail with
  `SQLITE_BUSY` (another writer advanced HEAD). FRESH mode: the session's
  image advances at statement granularity (each statement may adopt a newer
  generation before executing).

## 7. The staleness window

SNAPSHOT mode: a session reads a generation that may lag the remote `HEAD`
for the entire time between its open and the next **reopen or write
attempt**. This is the staleness window: bounded by the session's own
activity (every write re-reads HEAD) but otherwise open-ended for a
read-only, long-lived session.

FRESH mode: the staleness window is bounded by one statement — a session
that is about to execute a statement first re-reads `HEAD` and adopts the
newest generation (or fails closed if the HEAD cannot be read). Between two
statements the session may still be stale (it is not a live view), which is
exactly the poll-on-read contract.

## 8. Data-cache validity vs global freshness

Two properties are deliberately separated (detailed in
`CACHE_COHERENCE.md`):

- **Data-cache validity** — the local image is a byte-correct materialisation of
  *its* generation (guaranteed by recovery's checksum/chain validation).
- **Global freshness** — whether that generation is still the latest `HEAD`.

A session can hold a *valid but stale* image: valid against its generation,
not fresh against S3. OtsarDB does not attempt to keep the two equal without a
reopen/write.

## 9. What if S3 is down but the cache is hot?

**Reads continue locally.** Hot reads perform zero remote work
(`OBSERVED` `local_vfs.c:94-108`), so an already-open session keeps serving
reads from its materialised image when S3 is unreachable. The lease thread may
self-demote a leader after repeated renewal transport errors
(`s3_database.cpp:388-397`), which fences *writes*; reads are unaffected. A
reopen of a session whose cache is hot also requires a HEAD read to confirm the
cache is still current (`s3_database.cpp:1301`) — if S3 is down, that open
fails rather than serving an unverifiably-fresh cache.

## 10. FAQ

- **Can a read observe a half-published transaction?** No. Commits are visible
  only through the hash-linked HEAD chain; a reader materialises a whole
  generation, never a partial publication (`S3_STORAGE.md` invariant 4).
- **When does another instance's commit become visible?** In SNAPSHOT mode:
  on reopen, or on a write attempt (which then either adopts it or returns
  `SQLITE_BUSY`). In FRESH mode: at the START of the next `otsardb_exec`
  statement of the open session.
- **Is there an eventually-consistent read path?** SNAPSHOT mode is
  snapshot-at-open (no TTL/poll/coordinator path). FRESH mode is the
  poll-on-read path: a session that has been idle sees the newest committed
  generation on its next statement. The remaining FUTURE modes are listed in
  `CACHE_COHERENCE.md` §7.
