# OtsarDB Durability Model

> Public architecture reference. Grounded in `src/storage/wal_publisher.c`,
> `src/storage/commit_publish.c`, `src/storage/recovery.c`, `src/storage/replica.c`.
> Evidence discipline (the project's evidence rules) applies throughout.

## The commit protocol

A SQL COMMIT in WAL mode reaches `xSync`; the VFS WAL-sync hook publishes it to
object storage in this strict order, and the commit is acknowledged **only
after every step is durable**:

```text
1. refresh HEAD                (generation fence; 1 remote GET per commit)
2. encode WAL frames → segments (.otsseg, content-addressed, idempotent PUT)
   └─ (optional) AES-256-GCM envelope per segment
3. PUT the commit manifest     (commits/<commit_id>.json, idempotent)
4. HEAD CAS                     (the single authoritative publication point)
5. (if configured) replica phase: replicate objects + write COMMITTED ledger
   on every destination
6. xSync returns success        → SQLite acks the COMMIT
```

`OBSERVED` in `src/storage/wal_publisher.c` (`publish_one_commit` +
`publish_retry_cycle`) and `src/storage/commit_publish.c`
(`otsardb_publish_commit`, segments → manifest → HEAD CAS). Steps 1–4 map
exactly onto "segments + manifest + HEAD durably on S3 before ack".

**Failure propagates, it is never swallowed:** a non-OK publication maps to an
error through `journal_result_to_sqlite` (transport/ambiguous → `SQLITE_IOERR`
(10) family; conflict → `SQLITE_BUSY` (5); invalid → `SQLITE_CORRUPT`), the
sync fails, and the COMMIT is **not acknowledged**
(`src/storage/wal_publisher.c:355`).

## ACID, property by property

### Durability — remote ack, not local fsync

Durability is defined by the object store: the commit is durable when HEAD and
the objects it points at are readable back from S3. There is **one HEAD write
per commit** (the single mutable point); segments and the manifest are immutable
and idempotent, so re-attempting the same commit can only advance HEAD once —
no duplicate commit. `OBSERVED` (`commit_publish.c` `immutable_put`).

### Atomicity — HEAD CAS + chain validation

Atomicity is a *pointer atomicity*: either HEAD points at the new manifest
(commit visible, all its segments already durable) or it still points at the
parent (prepared objects are orphans, never half-applied). Recovery rebuilds a
database by walking the hash-linked chain from HEAD and re-applying each
manifest's segments; a broken hash link or a generation/commit-id mismatch
fails the restore, never silently heals. `OBSERVED` in
`src/storage/recovery.c` and `src/storage/commit_publish.c:729`
(`otsardb_resolve_commit`).

### Isolation — session snapshot

Reads are session-snapshot reads: a session that opened against generation N
keeps its local image until it re-materialises. The generation fence
refuses *writes* from a stale image, but stale *reads* are the documented
snapshot semantics — isolation is snapshot isolation for the single-writer
owner, and the two-writer case is surfaced as a 5/517 conflict, never a
merged state. `OBSERVED`; see `docs/architecture/CONCURRENCY.md`.

### Consistency — rolling checksum + verified restore

Every commit may carry a `post_apply_checksum` (rolling XOR checksum over the
post-commit page state, `page_no || page`). Recovery verifies the head
manifest's checksum against the reconstructed image; a mismatch fails closed.
The checksum is computed **once per commit before** the retry cycle, so a
retried commit publishes the identical checksum. When the local image
is unverifiable, the publisher refuses to publish a checksum rather than
publish a wrong one. `OBSERVED`.

## Failure model

| Failure | Behavior | Source |
|---|---|---|
| Transport error before HEAD | retried with bounded exponential backoff (default 3 attempts, `OTSARDB_PUBLISH_RETRIES`, base `OTSARDB_PUBLISH_RETRY_BASE_MS`=250 ms doubling); budget exhausted → sync fails, commit not acked | `wal_publisher.c:453` |
| Timeout, commit identity unknown (`AMBIGUOUS`) | resolved **by commit identity** (`otsardb_resolve_commit`); already-durable → reported COMMITTED with no retry; still absent → retried | `wal_publisher.c:511` |
| Lost response after HEAD delegate | identity resolution finds the commit's own HEAD → success | — |
| Partial segment upload | segment PUT timeout resolved by read-back; durable object → commit completes; missing object → `AMBIGUOUS`/abort | `commit_publish.c:46` |
| HEAD CAS conflict (`CONFLICT`) | **never retried**; classified 517 (overlap) vs 5 (disjoint) | — |
| Crash before HEAD | prepared segments/manifests are orphans; recovery ignores them, GC sweeps them | `gc.c` |
| Replica-phase failure (dual publish) | primary stays durable but the sync fails (commit NOT acked, `INCONCLUSIVE`); best-effort `ABORTED` ledger, rollback, retry re-resolves by identity | `wal_publisher.c:1119` |

Retry is **bounded and transport-only**: only `OTSARDB_JOURNAL_ERROR` and
`OTSARDB_JOURNAL_AMBIGUOUS` are retried. `CONFLICT` (the CAS fencing result),
`NOT_FOUND`, `INVALID`, and `UNSUPPORTED` are never retried. `OBSERVED`.

### At-rest encryption (opt-in)

With `OTSARDB_S3_ENC_KEY` set, every segment is stored as an AES-256-GCM
envelope `[iv(12)][ciphertext][tag(16)]` with AAD = `<absolute object key>\n<commit_id>`;
the manifest stays plaintext and its `sha256`/`size` cover the stored envelope,
so the chain remains integrity-binding. Encryption is a confidentiality
control, **not** a substitute for endpoint security or credential hygiene.
`OBSERVED`. Checkpoint page objects (`.otscp`) and the local
cache stay plaintext.

## What is proven vs unproven

**Proven (OBSERVED, with recorded artifacts in the project's change
record):**

- Empty-cache reopen and durable recovery on the recorded provider paths
  (MinIO local, MinIO BR, R2, Wasabi, Magalu).
- CAS fencing and two-writer behavior on MinIO and Cloudflare R2 test paths.
- Fault injection across every PUT of a multi-segment commit, plus
  segment/manifest/HEAD timeouts and their identity resolution
  (`tests/commit_fault_test.c`).
- Bounded retry semantics T1–T7 (`otsardb_wal_publisher_retry`).
- Multi-S3 replication/quorum within the recorded test scope.

**Not proven here (`UNTESTED`):**

- Retry/liveness under arbitrary 10×10 schedules, WAN conditions and
  provider-specific throttling.
- Swarm failover on two physical hosts, and a real destination kill during a
  live Model C quorum run.
- End-to-end propagation of `SQLITE_BUSY_SNAPSHOT` (517) through a live WAL
  sync on a real provider.
- "Zero RPO" / "high availability" / "multi-region" claims: these are not
  asserted here and are only valid if the matching recorded evidence
  supports them.
