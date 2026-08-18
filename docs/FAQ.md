# OtsarDB — FAQ

Short, honest answers to the questions every evaluator asks. Every answer
links to the deep document that carries the evidence. OtsarDB classifies
claims as OBSERVED / PROJECTED / UNTESTED / INCONCLUSIVE / NOT_APPLICABLE
(see the project's evidence rules); this page follows that discipline and
will not state more than the recorded validation supports.

## 1. What is OtsarDB, in one sentence?

A SQLite-derived database engine whose **authoritative state lives in
S3-compatible object storage** and whose compute (process/container) is
disposable — a local cache accelerates reads, but it is never the source of
truth. See `./ARCHITECTURE.md`.

## 2. Does OtsarDB hit S3 on every SELECT?

**No.** A session materializes a consistent snapshot at open; hot reads are
served from the local RAM/NVMe cache. S3 is contacted on open/reopen, on
write (commit publication), and on cache misses during lazy hydration — not
once per `SELECT`. See `./WAL_PIPELINE.md` (read/recovery path) and
`./S3_STORAGE.md` (cache tiering).

## 3. How does one instance see another instance's write?

By **reopening** (or a write attempt on a stale image), never invisibly. A
session holds a snapshot of the generation it opened; another instance's
commit is only observed after this session reopens and recovers to the newer
HEAD (full recovery, cache-reuse, or delta materialization). A stale session
that tries to write is fenced — the write returns `SQLITE_BUSY` rather than
silently overwriting the newer state. See `./CONCURRENCY.md`.

## 4. Can I read stale data?

**Yes, by design.** A session reads from the committed snapshot it opened.
If another writer commits while your session is open, your session keeps
seeing the older snapshot until you reopen. This is documented session-snapshot
semantics, not a bug; it also means your read-your-writes within a session is
guaranteed, while cross-instance freshness requires a reopen. See
`./CONCURRENCY.md` (stale-snapshot scenario) and
`./S3_STORAGE.md`.

## 5. What if S3 is down but my cache is hot?

Reads that hit the local cache **continue to work** — the cached image is
already materialized. Writes **fail**: a commit is only acknowledged after it
is durably published to object storage, so with S3 unreachable the sync fails
and the commit is not acknowledged (fail-closed, no silent loss). See
`./WAL_PIPELINE.md` (durability sync path) and `./S3_STORAGE.md`.

## 6. Do I still need backups?

**Yes — for a different reason than durability.** OtsarDB's durability
guarantee (an acknowledged COMMIT is durably published to S3 and survives
machine death) is not a backup. A backup protects against what durability
does not: accidental deletion or out-of-band edits to the bucket, provider
account loss, and point-in-time recovery to an arbitrary past state (OtsarDB
recovers to *committed* generations, not to arbitrary wall-clock points, and
has no snapshot-retention history). Keep provider-side backup/versioning for
those. See `./AVAILABILITY_MODEL.md` (Level A vs. recovery) and
`./guides/KNOWN_LIMITATIONS.md`.

## 7. Is it PostgreSQL-compatible?

**No.** OtsarDB speaks the **SQLite 3.53.4 dialect** (FTS5, RTREE, JSON,
math functions, CTEs, window functions, UPSERT, RETURNING). It is not a
PostgreSQL or MySQL wire-protocol/dialect target, and has no PL/pgSQL,
extensions, stored procedures, RLS, or LISTEN/NOTIFY. See
`./guides/KNOWN_LIMITATIONS.md` and `./WHEN_NOT_TO_USE.md`.

## 8. Is it a drop-in SQLite replacement?

**Dialect yes, storage model no.** The SQL surface is SQLite 3.53.4 and the
local `otsardb demo.db` path behaves like SQLite. But the S3 path changes the
storage model: S3 is authoritative, local files are disposable, commits are
acknowledged at remote-durability time, and concurrent writers surface
`SQLITE_BUSY` / `SQLITE_BUSY_SNAPSHOT` instead of OS file locking. Applications
that assume a local file on a POSIX filesystem need adaptation. See
`./ARCHITECTURE.md` and `./S3_STORAGE.md`.

## 9. Where do my credentials go?

Into the environment or the provider credential chain — never into the
database, manifests, or committed config. OtsarDB resolves, in order: explicit
`OTSARDB_S3_*` variables, standard AWS environment variables, the AWS
credentials file, then AWS IAM instance/container (workload) identities. See
`./guides/QUICKSTART.md` (credentials) and `./S3_STORAGE.md` (security baseline).

## 10. What's the license?

**Unlicense (public domain).** (See `LICENSE`.) The engine is derived from
the SQLite amalgamation; see `THIRD_PARTY_NOTICES` for the S3 client library
and LZ4 dependencies.

## 11. Is encryption on by default?

**No — opt-in.** Set `OTSARDB_S3_ENC_KEY` (64 hex chars) to enable
**AES-256-GCM** envelope encryption, and it covers **segment payloads only**:
the manifest, checkpoint objects and `HEAD` stay plaintext (use provider-side
encryption for those). Wrong/missing keys fail closed. See
`./guides/KNOWN_LIMITATIONS.md` (section 1).

## 12. What happens if the company disappears — am I locked in?

**No.** OtsarDB is open source (public domain) and the on-disk format is a
SQLite image; the durable state is plain S3 objects (`db.json`, `HEAD.json`,
immutable segments/manifests). With the source and the object keys you can
reconstruct a
real SQLite database image, so your data is not trapped behind a proprietary
runtime. See `./S3_STORAGE.md` (object layout) and `LICENSE`.

---

### Quick evidence numbers (for reference)

| Claim | Value | Classification |
|---|---|---|
| Single-node Swarm failover RTO | P50 **6.0 s** (kill → first valid write) | OBSERVED |
| Two-node (Docker-in-Docker) machine-loss RTO | P50 **19.1 s** (reconnect P50 13.5 s) | OBSERVED |
| RPO kill matrix | **RPO-OK** (no confirmed commit lost; corrupt/deleted segments fail closed) | OBSERVED |
| Lost-commit-response RPO | **INCONCLUSIVE** (deterministic coverage exists) | INCONCLUSIVE |
| Scale tested | 1,000,000-row local load | OBSERVED (test suite S8, local MinIO) |

Full record: `./performance/BENCHMARKS.md` and the test-suite report.
