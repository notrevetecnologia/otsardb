# OtsarDB — When to Use

Honest fit guide. OtsarDB is not a general-purpose replacement for a
traditional client-server database; it is a SQLite-derived engine with
**authoritative state in S3 and disposable compute**. Use it where that model
is an advantage, not where you are trying to force-fit it.

## Strong fits

### 1. Ephemeral / disposable compute

Serverless functions, edge workers, containers, CI jobs — anything where the
machine/process can disappear at any time. State lives in S3, so a replacement
instance recovers the database from object storage with **no prior local
state** (zero-cache reopen, OBSERVED on 5 providers). Kill the process; the
database survives. See `./AVAILABILITY_MODEL.md` (Levels A/B/C).

### 2. Read-heavy workloads with a hot cache

Reads are served from a local RAM/NVMe cache after a one-time snapshot
materialization at open — S3 is not hit per `SELECT`. If your access pattern is
read-mostly with periodic writes, you pay S3 latency only on open/reopen and on
commit. See `./FAQ.md` and `./WAL_PIPELINE.md`.

### 3. Decoupled storage — compute and data live apart

You want your database durable in object storage (buckets, lifecycle, provider
replication) instead of on a specific machine's disk. OtsarDB treats local
disk as disposable cache and object storage as the source of truth. See
`./S3_STORAGE.md`.

### 4. BYOS — bring your own S3, multi-provider

You want provider independence or replication across providers. OtsarDB is
capability-based (not brand-based): MinIO (local + datacenter) and Cloudflare R2
run CAS multi-writer; Wasabi and Magalu run single-writer; AWS and Backblaze B2
are untested. Multi-S3 dual publish and quorum replication are implemented
within the recorded scope. See `./S3_PROVIDERS.md` and
`./AVAILABILITY_MODEL.md` (Level F).

### 5. Remote durability matters more than minimal commit latency

Every acknowledged COMMIT means the data is **already durably in S3** — commit
latency includes the network round trip, in exchange for "a commit that
returned success is never lost to machine death." If you prefer strong
durability semantics over sub-millisecond local commits, this is the trade-off
you want. See `./WAL_PIPELINE.md` and `./S3_STORAGE.md`.

### 6. Zero-ops self-host

One binary, no server daemon, no config files, no init step. `otsardb demo.db`
behaves like SQLite; `otsardb s3://bucket/db` switches the storage target. See
`./guides/QUICKSTART.md`.

## It can work, within the recorded boundary

- **Two-instance / orchestrator failover** — leader + candidate takeover is
  OBSERVED (process, container, single-node Swarm, two-node Docker-in-Docker);
  physical-host failover is untested. See `./AVAILABILITY_MODEL.md`.
- **Multi-writer on CAS providers** — fenced optimistic writers with
  `SQLITE_BUSY` / `SQLITE_BUSY_SNAPSHOT` conflict classes; validated on MinIO
  and R2, with a recorded local high-volume CAS envelope (100/100 sessions).
  See `./CONCURRENCY.md`.

## Before you commit to OtsarDB

Read `./WHEN_NOT_TO_USE.md` and `./guides/KNOWN_LIMITATIONS.md`. If you are in
one of those cases (ultra-low-latency write-heavy OLTP, deep PostgreSQL/MySQL
feature dependence, no CAS and no single-writer, poor connectivity, or
high-contention many-writer topologies), the fit is not there today.
