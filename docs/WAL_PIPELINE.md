# OtsarDB WAL-to-S3 Pipeline

This document defines how OtsarDB turns SQLite-derived transactional page changes into OtsarDB storage objects and how those objects reconstruct a database.

The rule: OtsarDB does not invent a second transaction log when SQLite's
own interception, batching and recovery mechanisms suffice.

## Design basis

### SQLite WAL

SQLite WAL is the source transaction format inherited at the SQL-engine boundary. A WAL contains a 32-byte header followed by frames. Each frame contains a database page number, an optional database-size commit marker, salts, cumulative checksums and the full page image.

OtsarDB therefore starts with physical page changes rather than creating a new logical SQL log.

### VFS boundary and interception point

A SQLite VFS can separate the main database from WAL/journal handling while
making object storage the authoritative store. The lessons OtsarDB adopts:

- dirty page tracking belongs below SQL;
- speculative writes must never be published as committed data;
- an intervening VFS `xSync` is a meaningful durability boundary;
- immutable remote objects + a manifest pointer are a practical object-storage commit model;
- local cache/staging can be disposable while remote objects remain authoritative.

A virtual WAL interface (a set of hooks around the SQLite WAL — begin/end
write transaction, `xFrames` for writing page frames, undo,
savepoint/savepoint undo, frame count, checkpoint, snapshot operations) is
the strongest candidate for OtsarDB's eventual deep/native interception point,
because it sits inside the SQLite-derived WAL transaction path instead of
observing commits afterward.

**Decision:** OtsarDB first proves the storage/recovery contract through VFS
`xSync`. If that path cannot provide exact failure semantics or sufficient
performance, port/adapt a virtual-WAL interface into the OtsarDB core rather
than creating a new WAL abstraction from zero.

### Transaction-file and rolling-checksum patterns

Per-transaction files that add transaction identity/position, page frames,
file checksums and a rolling checksum representing the database state after a
transaction are a strong basis for:

- transaction identity/position;
- `.otsseg` evolution;
- rolling whole-database integrity checks;
- compaction;
- split-brain/divergence detection.

Immutable transaction files containing page frames, transaction
identity/position, checksums, indexing, compaction and recovery inform the
OtsarDB `.otsseg` evolution. These reference models observe/replicate an
existing SQLite WAL externally, so they are recovery/format references, not
OtsarDB's final synchronous COMMIT interception point.

The broader model of a SQLite VFS backed by a local working log plus a remote
transactional log/object-storage source of truth (lazy/partial replication and
transactional snapshots) is the architectural basis for later lazy page
hydration and partial recovery.

VFS-level change tracking, crash-safe spooling and snapshot reconstruction for
S3-compatible stores remain useful references for spooling and recovery
failure handling, though OtsarDB keeps remote state authoritative rather than
treating the local database as the source of truth.

Cloud-backed VFS/block/cache behavior is the primary precedent for
cloud-backed storage; OtsarDB continues to compare its block/cache/manifest
implementation before introducing equivalent machinery.

## Current OtsarDB pipeline

```text
SQL transaction
      |
      v
SQLite-derived WAL writes
      |
      v
database-owned OtsarDB VFS wrapper
  - delegates xWrite/xTruncate
  - tracks this database's WAL state
  - intercepts WAL xSync
      |
      v
underlying WAL fsync
      |
      v
incremental WAL reader
  - read 32-byte header
  - validate checksum + salts
  - identify WAL generation
  - resume from durable frame cursor
  - read only new complete frames
  - leave uncommitted tail outside cursor
      |
      v
stateful wal_publisher
  - restore cursor + HEAD state once after restart
  - publish only newly committed transactions
  - carry current manifest hash into the child commit
  - encode committed page batch
      |
      v
.otsseg
      |
      v
hash-linked commit manifest v4
      |
      v
HEAD publication
  - CAS when supported
  - explicit SINGLE_WRITER otherwise
      |
      v
xSync returns SQLITE_OK
      |
      v
SQL COMMIT succeeds
```

If publication fails, OtsarDB returns an SQLite error from the WAL `xSync` path instead of reporting a successful SQL COMMIT.

## Per-database VFS ownership

The initial bootstrap used a process-global durability callback. Production S3-backed opens no longer depend on that design.

Each OtsarDB database can register a dedicated VFS instance whose `sqlite3_vfs.pAppData` owns the WAL durability callback/context. Every wrapped `sqlite3_file` retains the exact VFS instance that opened it. That gives each database independent:

- object store;
- database root/id;
- WAL publisher;
- WAL cursor;
- commit `HEAD` state;
- close/lifetime ordering.

The default global hook remains only as compatibility/test tooling while the native S3 path uses database-owned VFS state.

## Incremental WAL cursor

The first correctness implementation read the complete WAL on every sync. That is no longer the normal path.

The current VFS provides the publisher with a random-access reader over the already-fsynced WAL file. The publisher maintains:

- WAL salts/generation;
- number of committed frames represented by the cursor;
- cumulative WAL frame checksum state;
- number of commits from that WAL generation already published;
- remote `HEAD` generation/commit identity;
- SHA-256 of the current manifest.

Normal same-generation sync:

```text
xSync
  |
  +--> read WAL header (32 bytes)
  |
  +--> salts unchanged
  |
  +--> seek to cursor frame
  |
  +--> read new frames only
  |
  +--> advance cursor only through successful COMMIT frames
```

An uncommitted/speculative tail does **not** advance the durable cursor. Those frames are reread on the next sync because they may later become part of a committed SQLite transaction.

### Publisher/process restart

A fresh publisher has no in-memory frame checksum cursor. OtsarDB therefore:

1. reads remote `HEAD` and its manifest SHA-256;
2. recognizes deterministic WAL commit IDs of the form `w<salt1><salt2>-<commit-index>`;
3. if `HEAD` belongs to the currently present WAL generation, scans once up to that commit index to reconstruct checksum/frame state;
4. continues incrementally from the next frame;
5. uses the `HEAD` manifest SHA as the parent hash of the next committed manifest.

This restart rebuild is intentionally a one-time cost for that local WAL generation, not a cost paid by every transaction.

### WAL checkpoint/reset

`PRAGMA wal_checkpoint(TRUNCATE)` or an equivalent WAL reset may produce a zero-length WAL and later a new header/salts. OtsarDB treats this as a new local WAL generation, not database-history reset. The next committed transaction still chains onto the existing remote OtsarDB `HEAD` and its manifest hash.

## Hash-linked manifest v4

New OtsarDB commits use manifest format v4: the v2 hash links plus an ordered
`segments` array so one commit can span multiple bounded immutable objects.

```text
HEAD
  commit_sha256 = SHA(manifest N)
        |
        v
manifest N
  parent_manifest_sha256 = SHA(manifest N-1)
  segments = [ {key, sha256, size}, ... ]   (applied oldest -> newest)
        |
        v
manifest N-1
  parent_manifest_sha256 = SHA(manifest N-2)
        |
        v
...
        |
        v
genesis manifest
  parent_generation = 0
  parent_commit_id = ""
  parent_manifest_sha256 = ""
```

Each manifest also contains the SHA-256 and exact byte length of every
immutable `.otsseg` it references.

This gives recovery two integrity dimensions:

1. **history integrity:** `HEAD` authenticates the newest manifest and every v2 manifest authenticates its parent;
2. **transaction-object integrity:** every manifest authenticates the exact segment bytes it references.

Manifest v1 remains readable for development/backward compatibility. Once recovery encounters a legacy v1 segment of history, no parent hash exists to validate earlier manifests, but the `HEAD` target and every segment are still content-verified. New publication always emits v4 (with v1-v3 still decodable).

## Bounded/streaming segment construction (manifest v4)

A single very large transaction is no longer accumulated in a
transaction-sized memory buffer. The incremental reader keeps only frame references
(page number + WAL byte offset, 12 bytes per frame) in memory while scanning.
At the commit frame, the publisher re-reads the already-fsynced page images
from the WAL file and encodes the commit as **one or more bounded segments**:

```text
xSync
  |
  v
scan WAL (validate salts + checksums, record page_no + offset only)
  |
  v
commit frame reached
  |
  v
split frames into chunks of at most segment_payload_limit encoded bytes
  |
  v
per chunk: re-read pages from the WAL -> wal_batch -> .otsseg
  |
  v
manifest v4 with ordered "segments": [{key, sha256, size, pages}, ...]
  |
  v
HEAD publication (CAS when supported)
```

`otsardb_wal_publisher_set_segment_payload_limit()` bounds the default 16 MiB
budget; a transaction larger than the budget becomes several immutable
segments of one commit. Recovery applies all segments of each manifest
oldest-first. Peak publisher memory is bounded by the chunk budget plus the
12-byte-per-frame reference array, independent of transaction size.

In manifest v4 each segment key is stored relative to the database root so
the manifest stays small; legacy v1/v2 flat `segment_key` remains the
absolute key and both formats remain decodable. New publication always emits
v4. A commit may span at most `OTSARDB_MANIFEST_MAX_SEGMENTS` (128) segments;
a transaction exceeding that bound fails at xSync instead of committing
partially.

## Current recovery path

Recovery is the inverse operation:

```text
db.json + HEAD
      |
      v
verify SHA(HEAD manifest)
      |
      v
walk manifest chain backwards
  - generation/id continuity
  - v2 parent-manifest SHA link
      |
      v
fetch .otsseg objects
  - exact stored size
  - SHA-256
  - commit id
      |
      v
apply WAL page batches oldest -> newest
      |
      v
reconstructed local execution image
      |
      v
OtsarDB opens image and executes SQL
```

A deterministic recovery test additionally mutates an older parent manifest by appending harmless whitespace without changing its semantic fields. Recovery must still reject the history because the child's recorded parent SHA no longer matches the object bytes.

## Checkpoints

A checkpoint (`.otscp`) materialises the full database state at a specific
commit, producing a repeatable page-for-page image that recovery can use to
skip all pre-checkpoint history. When a checkpoint exists, recovery first
applies the checkpoint image, then replays only the segment/commit chain
that follows it — bounding recovery time regardless of how many earlier
segments exist. Checkpoints do not replace the commit journal; they are a
performance optimisation layered on top of the same immutable-object
contract.

Checkpoints can also be produced automatically. When a native S3 session
closes, OtsarDB compares the remote HEAD generation against the generation
covered by the last published checkpoint; if the delta reaches
`OTSARDB_AUTO_CHECKPOINT_COMMITS` (default 1000), the checkpoint builder runs
before the session tears down. The build is best-effort: a failure only logs
to stderr and never breaks the close, and `OTSARDB_AUTO_CHECKPOINT=0` disables
it entirely. Setting `OTSARDB_AUTO_CHECKPOINT_COMMITS=1` forces a checkpoint at
every close, which doubles as an operations tool for explicitly collapsing
history.

## Why `sqlite3_wal_hook()` is not the durability hook

`sqlite3_wal_hook()` is an observation callback invoked after SQLite has committed the transaction. Returning an error from that callback does not make it an appropriate point to promise that the local transaction never committed.

OtsarDB's target guarantee is stronger:

> A successful OtsarDB COMMIT means all state necessary to recover that transaction is durably published to authoritative object storage.

Therefore remote publication participates in the WAL sync path before SQLite receives sync success.

## Correctness evidence implemented

The repository contains deterministic tests for:

- a real SQLite WAL being parsed, batched into `.otsseg` and reapplied to an empty database image;
- an injected WAL `xSync` failure causing SQL to return an error and the failed row remaining absent after restart;
- ROLLBACK not advancing remote `HEAD`;
- object-store failure after a segment but before `HEAD` causing SQL failure while the previous `HEAD` remains authoritative;
- ambiguous `HEAD` timeout after the remote PUT actually succeeded being resolved by `commit_id` before SQL receives success;
- publisher restart against the same WAL resuming from remote `HEAD` without duplicate generations;
- `PRAGMA wal_checkpoint(TRUNCATE)` resetting the WAL and the next WAL generation chaining onto the existing remote history;
- CAS-capable storage and S3 Core/SINGLE_WRITER storage both publishing real WAL commits;
- zero-local-state recovery after deleting the original `.db`, `-wal` and `-shm` files;
- incremental same-generation WAL publication reading less than the complete WAL;
- an idle repeated publisher sync with no new frames reading only the 32-byte WAL header;
- failure, ambiguous-timeout and S3 Core tests running through the incremental reader hook;
- two database-owned VFS instances isolating independent databases in one process;
- manifest v2 publication requiring the exact parent manifest hash;
- tampering with an older parent manifest causing zero-local recovery rejection;
- a live generic-S3 integration executable using the incremental WAL reader for SQL -> S3 -> zero-local recovery;
- one commit spanning multiple bounded segments (manifest v4) with forced tiny chunk budgets, verified end-to-end through zero-local-state recovery.

The S3 GitHub Actions workflow is configured to build the S3 adapter, run deterministic CTest cases, start a disposable S3-compatible endpoint, run the capability probe and then run the live WAL/S3 recovery/native-URI scenarios. A successful latest remote Actions result must still be observed before the native S3 milestone is marked complete.

## Current limitations

The storage implementation is still being hardened for production workloads:

1. ~~A single huge transaction is currently accumulated in a transaction-sized memory buffer before `.otsseg` encoding.~~ **Resolved by manifest v4 bounded chunking** (see above); remaining bound is the 128-segment-per-commit cap and the chunk-budget metadata array.
2. A publisher process restart against a surviving WAL may perform a one-time prefix scan to rebuild the cumulative SQLite WAL checksum cursor from remote `HEAD`.
3. Synchronous object-storage publication adds network latency to SQL COMMIT. Batching/pipelining must preserve the durability contract.
4. Large transactions/WALs, multipart objects and long checkpoint cycles still need stress testing.
5. Rolling whole-database checksum/TXID metadata is not yet part of OtsarDB's format; it remains a candidate for stronger divergence checks and compaction.
6. Native S3 credential-provider chains and reusable manifest-aware local cache are not yet productionized.

## Escalation criterion: port a virtual WAL interface

If VFS `xSync` cannot provide acceptable semantics/performance as these limitations are removed, OtsarDB should port/adapt a virtual WAL interface into its SQLite-derived core.

That path gives OtsarDB direct access to `xFrames`, `xUndo`, savepoints and checkpoints and is preferred over inventing an unrelated OtsarDB transaction engine.

## Longer-term optimizations

Only after correctness:

- direct `xFrames` interception if selected;
- TXID, rolling DB checksum, page indexing and compaction;
- page groups, large pages, compression and Range GET;
- lazy/partial hydration;
- manifest-aware RAM/NVMe cache;
- checkpoints that collapse long segment chains;
- object garbage collection after an authoritative manifest transition.
