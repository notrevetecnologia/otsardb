# OtsarDB — When NOT to Use

Mandatory honesty page. If you are in one of the cases below, OtsarDB is not
the right tool today. This is a design boundary, not a sales objection to be
worked around.

## 1. Ultra-low-latency, write-heavy OLTP

Every acknowledged COMMIT requires durable publication to object storage
(segment PUT + manifest PUT + HEAD advance) before SQL reports success. That
network round trip makes OtsarDB unsuitable for workloads that need
sub-millisecond write commits under heavy write concurrency.

**Use PostgreSQL (or a similarly purpose-built client-server OLTP database)
instead — it wins here.** Its local write path and mature write-heavy
concurrency are the correct fit; OtsarDB explicitly does not compete on that
axis. See `./WAL_PIPELINE.md` (durability sync path) and
`./S3_PROVIDERS.md` (observed bench latency classes).

## 2. Deep PostgreSQL/MySQL feature dependence

OtsarDB is the **SQLite 3.53.4 dialect** — not PostgreSQL/MySQL compatible. It
does not provide, and is not a migration target for:

- PL/pgSQL or stored procedures/functions;
- extensions / custom types / operators;
- LISTEN/NOTIFY or other client-server notification primitives;
- row-level security (RLS) or PostgreSQL-style GRANT hierarchies;
- server-managed roles and connection pools.

If your application or existing schema depends on these, stay on your current
database. See `./guides/KNOWN_LIMITATIONS.md` and `./FAQ.md`.

## 3. Providers without read-after-write / CAS — unless single-writer

OtsarDB needs S3 Core (authenticated object ops with usable read-after-write)
and, for independent concurrent writers, S3 CAS (conditional writes). Wasabi
and Magalu lack CAS, so they run **single-writer** only — an explicit
deployment assertion, not a lock. If you cannot guarantee one writer and your
provider lacks CAS, OtsarDB refuses writes (fail-closed) rather than risking
lost updates. That is correct behavior, but it means it is not the tool for
that topology. See `./S3_PROVIDERS.md` and
`./guides/KNOWN_LIMITATIONS.md` (section 3).

## 4. Poor or intermittent connectivity

Every commit — and every cold reopen — depends on reaching object storage.
With an unreliable link, commits fail (fail-closed, no silent loss, but no
progress) and cold recovery stalls. A hot cache lets reads continue, but
writes and opens cannot. If your environment cannot reach S3 reliably, do not
adopt OtsarDB. See `./FAQ.md` (S3-down behavior).

## 5. High-contention many-writer topologies (unvalidated)

The fenced multi-writer path is validated on **two-writer CAS races** (MinIO,
R2) and a recorded **local** high-volume CAS envelope (100/100 sessions with a
lease-aware 60-attempt retry budget). Arbitrary >10-writer contention, WAN
degradation, provider throttling, and alternate retry budgets are **not
proven**. Do not assume it scales to many concurrent writers; retry liveness
outside the recorded budget is unproven. See `./CONCURRENCY.md` for the
fencing and retry semantics.

## 6. Arbitrary out-of-band bucket edits

The database's integrity assumes the S3 objects are managed through OtsarDB.
Recovery detects corruption/deletion and fails closed (OBSERVED: corrupt or
deleted segments fail closed), but out-of-band edits, accidental deletion, or
renaming objects can destroy committed state or force manual repair. If your
workflow requires editing bucket objects directly (or you cannot protect the
bucket from deletion), OtsarDB is not a safe fit. See `./S3_STORAGE.md`
(reachability/GC) and `./FAQ.md` (backups).

---

**If none of these apply**, see `./WHEN_TO_USE.md`. If several do, the honest
conclusion is that OtsarDB is not your database — and that is fine.
