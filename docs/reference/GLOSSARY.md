# OtsarDB — Glossary

Terminology used across the OtsarDB documentation. Definitions are grounded in
the current implementation and its evidence record; they are not aspirational.
Primary sources: `../S3_STORAGE.md`, `../WAL_PIPELINE.md`, `../CONCURRENCY.md`,
`../AVAILABILITY_MODEL.md`, and
`../architecture/STORAGE_MODEL.md` (exact object keys and fields).

## Core storage model

### authoritative storage
The object store (S3-compatible) is the **durable source of truth** for the
database. A commit is only acknowledged after its objects are durably
published there; local files and caches are never the source of truth.

### disposable compute
The OtsarDB process/container holds only operational state and a disposable
local cache. It can be killed and recreated on another machine, which then
recovers the database from authoritative storage with no prior local state.

### generation
A monotonically increasing integer naming one committed version of the
database. Each acknowledged commit advances `HEAD` from generation N to N+1.
A stale writer publishing from an older generation is fenced (see fencing).

### HEAD
The small `HEAD.json` object that points at the current committed generation
(`generation`, `commit_id`, `commit_sha256`). It is published atomically — with
CAS (`If-Match`/`If-None-Match`) on CAS-capable providers — and is the normal
entry point for readers. HEAD advancement is the **durable publication point**:
a commit is acknowledged only after it succeeds.

### manifest
An immutable JSON object (`commits/<commit-id>.json`) describing one committed
generation: parent hash links, an ordered `segments` array, page size, and
database-size metadata. Format v4 (hash-linked); v1–v3 remain decodable. The
chain of manifests is the recovery/audit history.

### segment
An immutable, content-addressed `.otsseg` object holding the durable page/frame
changes of one commit (or a bounded chunk of a very large transaction). LZ4
compressed, SHA-256 checksummed, referenced by the manifest.

### checkpoint
An immutable `.otscp` object that materializes the full database state at a
specific commit, so recovery can start from it and replay only the later chain
— bounding recovery time. Checkpoints do not replace the commit journal, and
can be produced automatically after N commits (`OTSARDB_AUTO_CHECKPOINT_COMMITS`).

## Coordination and concurrency

### CAS (compare-and-swap)
Conditional object writes (`If-None-Match: *` for create, `If-Match: <etag>`
for update). A provider returning **412** on a failed condition enforces CAS,
which OtsarDB uses for optimistic writer fencing. Providers without CAS
(Wasabi, Magalu) run single-writer mode. See `../S3_PROVIDERS.md`.

### lease
A time-bounded writer-ownership record (`<root>/lease.json`) with a TTL
(`OTSARDB_LEASE_TTL`), renewed by a live leader. It prevents two live
publishers; a candidate takes over only after provable expiry (Last-Modified
vs. TTL), via CAS.

### fencing
The set of mechanisms that stop a stale or deposed writer from committing:
the **lease**, the **generation fence** (stale local image), and the **HEAD
CAS** (physical barrier). A fenced write returns `SQLITE_BUSY`/`SQLITE_BUSY_SNAPSHOT`
— it never silently overwrites newer state.

### BUSY vs BUSY_SNAPSHOT
The two conflict classes a committing statement surfaces:
- `SQLITE_BUSY` (5) — stale writer / not yet eligible: candidate mode (a live
  lease exists), generation fence, or a disjoint-page CAS conflict. Action:
  re-open and retry.
- `SQLITE_BUSY_SNAPSHOT` (517) — genuine page overlap: the winner's commit
  touched at least one page yours also wrote. Action: re-query, rebase,
  re-execute.

## Caching and reads

### cold cache / warm cache
A **cold** cache is an empty local cache: reopening pays full recovery
(zero-local reopen). A **warm** cache has a saved image: if the remote HEAD is
unchanged the image is reused directly; if it changed, the delta path applies
only the newer commits.

### hydration
Materializing the local execution image from authoritative storage.
- **eager** — replay the full history at open;
- **lazy** — restore from the newest checkpoint base and fetch pages on demand
  (Range GET) as SQLite touches them;
- **delta** — apply only the segments of generations G+1..HEAD on top of a
  cached image at generation G.

### session snapshot
A session reads from a consistent committed snapshot materialized at open.
Other instances' commits are not visible to it until it reopens (or a write
attempt is fenced with BUSY). This is documented, intended behavior.

### read-your-writes
Within a session, your own committed writes are visible. Cross-instance
visibility requires a reopen; the two are distinct guarantees.

## Multi-S3 / availability

### read replica
A read-only session that opens the database against the replicated destination
(`OTSARDB_S3_READ_FROM_REPLICA=1`). It never claims the lease, never publishes,
and all DML fails with `SQLITE_READONLY`; it sees the newest commit that is
COMMITTED on that replica.

### quorum
Model C N-of-M replication: a commit is durably written to a majority/quorum
of the configured destinations before it is acknowledged, reducing dependence
on a single provider.

### promotion
Making a replica store the new primary (an offline / disaster-recovery operator
contract). Refused while the old primary's lease is provably live; gated by a
CAS epoch raise so two concurrent promotions cannot both win.

### reconciliation
Replaying the gap between a lagging replica HEAD and the primary chain
(bounded, e.g. 64 commits), re-publishing the missing objects and advancing the
replica HEAD, so a replica that fell behind catches up instead of failing
closed forever.
