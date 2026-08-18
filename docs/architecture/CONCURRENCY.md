# OtsarDB Concurrency Model

> Public architecture reference. Grounded in `src/storage/wal_publisher.c`,
> `src/storage/commit_publish.c`, `src/ha/lease.c`, `src/storage/replica.c`.
> Evidence discipline (the project's evidence rules) applies throughout.

OtsarDB coordinates concurrent writers with **three layered fences**. They are
ordered weakest-first (cheap local check) to strongest (physical CAS barrier):

1. **Writer lease** (`lease.json`) — elects a *single* preferred publisher and
   lets a crashed writer be replaced automatically.
2. **Generation fence** — refuses a writer whose local execution image is older
   than the remote HEAD (lost-update guard).
3. **HEAD CAS** — the physical linearization point: exactly one writer's
   conditional HEAD write wins.

## 1. Writer lease (claim / renew / takeover)

The lease object is `<root>/lease.json` =
`{"owner":"<instance-id>","generation":N,"address":"…"}`. It is *not* part of
the database image and is invisible to recovery. `OBSERVED` in `src/ha/lease.c`.

- **Claim (absent)** → create at generation 1 with `If-None-Match:*`; a
  concurrent create loses the CAS and re-reads.
- **Claim (same owner, reconnect/adopt)** → CAS `If-Match` re-stamp at the same
  generation.
- **Claim (different owner, takeover)** → CAS `If-Match` overwrite at
  `generation + 1`, allowed **only when expiry is provable**: the store's
  `Last-Modified` must satisfy `age >= TTL - skew` (5 s skew margin). A store
  that exposes no `Last-Modified` makes the lease "presumed live" — never a
  blind CAS. `OBSERVED` in `src/ha/lease.c:328`.
- **Renew** → CAS `If-Match` at the same generation; a `412` marks the lease
  permanently lost (demotion). Transport errors demote after 3 consecutive
  failures. `OBSERVED` in `src/ha/lease.c:378`.
- **Liveness** is judged from the owner's local clock (`claimed_at` re-anchored
  by claim/adopt/renew) plus the store `Last-Modified` for takeover; the CAS
  generation is the true fence, so two writers can never both believe they hold
  the same generation. `OBSERVED` in the lease module comment.

The background thread (`s3_database.cpp`) renews every
`TTL/3`; a session that loses the lease becomes a **candidate** that polls and
re-claims only after proven expiry, then re-runs recovery to HEAD before its
publish fence re-opens.

## 2. Generation fence (local image staleness)

Every WAL sync refreshes the remote HEAD first. If another writer advanced
HEAD past the generation the local execution image was materialised from, the
sync refuses with `SQLITE_BUSY` — it never silently overwrites an acknowledged
commit. `OBSERVED` in `src/storage/wal_publisher.c:1519-1539`.

- The fence is seeded from recovery HEAD at open (`s3_database.cpp`), so a
  stale writer's later HEAD CAS also fails on the etag precondition.
- Cost: **one extra remote HEAD GET per commit** (per sync). This is the
  documented `request-per-commit` overhead of correctness.

## 3. HEAD CAS and the two-writer race

Publication ends with a single conditional HEAD write:

- `create_head_if_missing` (genesis) → `If-None-Match:*`;
- otherwise → `If-Match` the cached HEAD etag.

Two writers racing from the same parent produce **one winner** (HEAD advances)
and **one loser** (CAS `412` → `OTSARDB_JOURNAL_CONFLICT`). `OBSERVED` in
`src/storage/commit_publish.c:672-684`.

### Conflict classification: 517 vs 5

A CAS failure alone does not tell the loser *why*. On `CONFLICT`, the publisher
walks the interleaved committed chain between the loser's `parent_generation`
and the remote HEAD and intersects per-segment page sets
(`otsardb_publish_commit_detect_conflict`):

- **Page overlap** (the interleaved winner touched pages this commit also
  touches) → the extended result code **`SQLITE_BUSY_SNAPSHOT` (517)**: the
  application must re-run against the new HEAD image (its local snapshot is
  stale). `OBSERVED` in `src/storage/wal_publisher.c:1291-1295`; the constant is
  `SQLITE_BUSY (5) | (2<<8) = 517` (`vendor/sqlite/…/sqlite3.h:545`).
- **Disjoint pages** → plain **`SQLITE_BUSY` (5)**: a stale-writer retry that
  may simply be re-attempted.

`sqlite3_extended_result_codes` is enabled at open
(`src/core/otsardb.c:62`), so 517 reaches the application verbatim.

**Rebase/retry contract:** on `517`, re-materialise from the new HEAD and
re-execute. On `5`, the same operation may be retried; a later attempt whose
parent has moved on will classify again. There is **no automatic merge** of
conflicting writes — OtsarDB never combines two writers' page changes; the loser
always re-executes from the winner's state. `OBSERVED` (this classification is
made explicit in the deterministic conflict test).

## Single-writer for CAS-less providers

Providers without conditional writes cannot support independent concurrent
writers. OtsarDB capability-probes `CONDITIONAL_CREATE`/`CONDITIONAL_UPDATE`
(`src/storage/object_store.h`) and falls back to
`OTSARDB_WRITE_STRATEGY_SINGLE_WRITER`, which requires the operator assertion
**`OTSARDB_S3_SINGLE_WRITER=1`**; otherwise the native open fails closed.
In this mode the parent is validated by read-back before HEAD is written
unconditionally (`validate_single_writer_parent`,
`src/storage/commit_publish.c:90`), and no lease/thread is started.
`OBSERVED` in `src/storage/s3_database.cpp:975`, `src/storage/wal_publisher.c:174`.

## Explicitly out of scope

- **No magic merge / no auto-conflict-resolution.** Conflicting writes are
  surfaced (5 or 517), never silently combined.
- **No distributed transaction / 2PC** across writers. Multi-S3 is replication
  (`replica.c`), not a multi-writer consistency protocol; the replica path is a
  separate ledger-based dual-publish.

## Evidence status

- Lease claim/renew/takeover and CAS generation fencing: `OBSERVED` via
  deterministic `otsardb_lease` tests and live MinIO failover runs.
- Two-writer 517-vs-5 classification: `OBSERVED` at the detect→busy code level
  (`tests/conflict_test.c`); the end-to-end propagation of
  `SQLITE_BUSY_SNAPSHOT` through a live WAL sync on a real provider remains
  `UNTESTED` (a CAS failure cannot be produced deterministically in a
  single-threaded test).
- Retry liveness under arbitrary 10×10 schedules/WAN throttling: `UNTESTED`
  (see the project's current-state notes).
