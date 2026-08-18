# OtsarDB Native S3 Storage Contract

Status: design baseline for `otsardb-s3`.

This document defines the first correctness contract for OtsarDB when S3-compatible object storage is authoritative persistence. Local RAM/NVMe may accelerate execution but is disposable.

## Design basis

OtsarDB derives its storage model from SQLite's existing mechanisms and does not invent a second transaction log from zero.

- **SQLite VFS/WAL** supplies the existing SQL transaction ordering and the storage interception boundary.
- Immutable S3 objects, manifest-driven page indirection, page/cache tiering and later lazy materialization/prefetch are implemented as OtsarDB-owned mechanisms.
- Immutable transaction state, orphan-safe publication, optimistic concurrency and conflict handling are implemented as OtsarDB-owned mechanisms.

The first implementation prioritizes correctness and crash recovery before advanced page grouping or prefetch.

## Non-negotiable invariants

1. A committed transaction must be recoverable using only S3-authoritative state.
2. Local database files, WAL files and caches must be disposable.
3. Data objects already published as committed are immutable.
4. A reader observes an entire committed generation, never a partially published transaction.
5. A failed commit may leave orphan objects, but orphans are unreachable and therefore harmless.
6. The commit acknowledgement point is remote durability, not a later backup/checkpoint timer.
7. S3 write mode is enabled only when the configured provider satisfies OtsarDB's required consistency primitives.

## Database target and identity

A target such as:

```text
s3://my-bucket/tenant-a/orders
```

maps to one OtsarDB database root:

```text
s3://my-bucket/tenant-a/orders/
```

`db.json` is created once and establishes database identity and on-disk format compatibility.

Example:

```json
{
  "otsardb_format": 1,
  "database_id": "019c...uuid",
  "created_at": "2026-08-09T00:00:00Z",
  "page_size": 4096,
  "features": []
}
```

The page size remains an engine setting. OtsarDB must not force a larger-page optimization until benchmarks justify it.

## Object layout

```text
<db-root>/
  db.json
  HEAD.json

  commits/
    <commit-id>.json

  segments/
    sha256/<aa>/<sha256>.otsseg

  checkpoints/
    sha256/<aa>/<sha256>.otscp

  leases/              # reserved for HA/coordination; not v0.1
  gc/                  # optional future GC bookkeeping
```

### `db.json`

Stable database metadata. It is not changed on every commit.

### `HEAD.json`

Small current-generation pointer. This is the normal authoritative entry point for readers.

Example:

```json
{
  "otsardb_format": 1,
  "database_id": "019c...uuid",
  "generation": 42,
  "commit_id": "01J...ulid",
  "commit_key": "commits/01J...ulid.json",
  "commit_sha256": "..."
}
```

The object must be published atomically. For creation OtsarDB uses conditional create (`If-None-Match: *`) when supported. For updates OtsarDB uses the ETag read with the previous HEAD and publishes with `If-Match`.

A conditional-update failure means the writer no longer owns the version it read and must not silently overwrite newer state.

### Commit manifests

Every committed generation has an immutable commit manifest.

Published format is v4 (hash-linked, like v3, plus page-set metadata and an ordered `segments`
array so one commit can span multiple bounded `.otsseg` chunks):

```json
{
  "otsardb_format": 3,
  "database_id": "019c...uuid",
  "generation": 42,
  "commit_id": "01J...ulid",
  "parent_commit_id": "01J...previous",
  "parent_generation": 41,
  "parent_manifest_sha256": "...",
  "segments": [
    {
      "key": "segments/sha256/ab/abcdef....otsseg",
      "sha256": "abcdef...",
      "size": 123456,
      "kind": "wal_frames"
    }
  ],
  "checkpoint": null,
  "page_size": 4096,
  "database_size_pages": 1234,
  "created_at": "2026-08-09T00:00:00Z"
}
```

Segment keys inside v3 are relative to the database root. Legacy v1/v2
manifests (single flat `segment_key`/`segment_sha256`/`segment_size`) remain
decodable; recovery accepts the whole chain.

The manifest is uploaded before HEAD is advanced. Until HEAD references it, the manifest is not committed state.

### Segments

The first OtsarDB S3 implementation uses immutable commit segments derived from SQLite's existing transaction/WAL ordering rather than inventing a second transaction engine.

An `.otsseg` contains the durable page/frame changes required for one OtsarDB commit (or a bounded chunk of one very large transaction), plus:

- format version;
- commit id;
- page/frame identifiers;
- payload lengths;
- checksum(s);
- optional compression metadata.

Segment payloads use LZ4 compression at format v2, which is transparent to
consumers — the header declares the format and the decoder selects the
correct decompressor. v1 uncompressed segments remain decodable.
Compression benchmarks show typical WAL-page commits are
~10× smaller on S3 with a ~25 % latency improvement.

The exact binary encoding is an implementation detail behind the OtsarDB storage API. The logical contract is immutable and checksummed.

### Checkpoints

A checkpoint is an immutable materialized database/page-map state used to bound recovery time. It does not replace the commit journal.

A future checkpoint may use page grouping, seekable compression and Range GET-friendly layouts. Correctness must not depend on those optimizations.

## Commit protocol

For transaction T based on generation N:

```text
1. SQLite/OtsarDB completes the logical transaction locally enough to know its durable frame/page set.
2. OtsarDB builds immutable segment object(s) for T.
3. PUT every segment to its content-addressed key.
4. Verify upload result/checksum metadata required by the backend.
5. Build and PUT immutable commits/<commit-id>.json referencing those segments.
6. Publish HEAD.json -> generation N+1.
   - bootstrap: If-None-Match: *
   - existing database: If-Match: <etag-of-head-N>
7. Only after HEAD publication succeeds may OtsarDB acknowledge COMMIT.
```

**Step 6 is the durable publication point.**

Objects uploaded in steps 3-5 are prepared state. They become committed only when reachable from the successfully published HEAD chain.

## Why HEAD is authoritative

Normal open/reopen must require O(1) metadata discovery instead of depending on listing every manifest.

Immutable commits remain a recovery/audit chain. If HEAD is lost or administratively damaged, a repair tool may scan immutable commit objects, validate their parent chain/checksums and rebuild HEAD. Normal database operation does not use LIST as its only source of truth.

## Crash and timeout behavior

| Failure point | Visible result |
|---|---|
| before segment PUT | generation N remains committed |
| partial/failed segment PUT | generation N remains committed; incomplete upload is garbage |
| after segment PUT, before manifest | generation N remains committed; segment is orphan |
| after manifest PUT, before HEAD | generation N remains committed; manifest/segments are orphan prepared state |
| conditional HEAD fails | generation N or a competing newer generation remains authoritative; writer must refresh |
| HEAD succeeds, process crashes before response | generation N+1 is committed |
| stale/corrupt local cache | discard cache and rebuild/fetch from S3-authoritative state |

### Ambiguous client timeout after HEAD publication

Every transaction has a unique `commit_id`.

If the client receives a timeout while OtsarDB cannot tell whether HEAD publication completed, OtsarDB re-reads HEAD:

- HEAD references our `commit_id`: return success;
- HEAD still references our parent generation: publication did not commit and may be retried;
- HEAD references a different/newer chain: enter concurrency/conflict handling rather than claiming success.

## Read/recovery path

Initial correctness-first reopen:

```text
GET db.json
GET HEAD.json
GET referenced commit manifest(s)
find nearest checkpoint if present
fetch required immutable segment/checkpoint objects
verify checksums
reconstruct/materialize current database state
open OtsarDB SQL core
```

After this is proven, OtsarDB may replace eager materialization with lazy page fetch:

```text
SQLite page request
   -> OtsarDB RAM cache
   -> OtsarDB NVMe cache
   -> S3 Range GET/object fetch
```

That optimization must preserve the same commit/HEAD contract.

## Concurrency compatibility

### First S3 milestone

- many readers may independently open committed generations;
- one authoritative write source is the supported topology;
- HEAD conditional publication acts as a safety fence against accidental stale publication.

### Later optimistic writers

Multiple independent writers will use the same HEAD ETag as a compare-and-swap generation fence:

```text
Writer A reads HEAD etag E
Writer B reads HEAD etag E
A publishes new HEAD with If-Match E -> success
B publishes with If-Match E -> precondition failure
B refreshes, checks conflicts, rebases/retries or returns conflict
```

Conflict detection/rebase is a separate milestone. A CAS failure is never permission to overwrite.

### HA serialized writers

Lease, forwarding and fencing may be layered above this same storage contract. HA changes who is allowed to commit; it does not change what a committed OtsarDB generation means.

## S3 provider capability contract

Write mode requires a tested provider profile. At minimum OtsarDB must verify the primitives it relies on, including:

- whole-object atomic publication semantics;
- read-after-write behavior for GET/HEAD;
- conditional create/update (`If-None-Match` / `If-Match`) for safe concurrent-process fencing;
- Range GET for the optimized read path;
- predictable multipart completion semantics for larger objects.

AWS S3 is the reference behavior. MinIO is the first self-hosted compatibility target. Other providers are enabled only after capability tests.

## Garbage collection

GC is reachability-based.

An object may be deleted only when OtsarDB can prove it is not reachable from:

- current HEAD;
- retained commit history/checkpoints;
- an active retention/snapshot policy.

A minimum grace period is required before deleting newly discovered orphans so a slow/in-flight writer cannot have prepared data removed prematurely.

## Security/integrity baseline

- Content objects use SHA-256 identity/checksums at the OtsarDB layer; do not treat provider ETag as OtsarDB's content hash.
- Credentials never live in manifests, database metadata or committed repository config.
- Encryption/compression are storage-format features that may be added while preserving immutable-object and HEAD-publication semantics.

## Implementation order

1. S3 backend interface and provider capability probe.
2. `db.json` + `HEAD.json` models.
3. immutable `.otsseg` encoder/decoder with checksum.
4. immutable commit manifest.
5. conditional HEAD publication.
6. crash-injection tests for every commit step.
7. zero-local-state recovery.
8. immutable checkpoints (`.otscp`) that materialise full database state to bound recovery time.
9. integrate with SQLite WAL/VFS transaction durability boundary.
10. adapt lazy page/cache/page-group optimizations.
11. add higher concurrency stages.

## Definition of success

The defining test remains:

1. open `s3://bucket/database`;
2. create schema and commit rows;
3. stop OtsarDB;
4. delete every local database/WAL/cache file;
5. start a fresh OtsarDB process;
6. open the same S3 target;
7. recover exactly the committed schema/data;
8. prove injected crashes never expose partial transactions.
