# Migrating from SQLite to OtsarDB

> Guide. Honesty markers per the project's evidence rules apply. The
> compatibility contract is the pinned engine version: the SQL dialect is
> the pinned SQLite 3.53.4 dialect, so the *SQL* is a drop-in; the
> *storage and deployment* are not. Read
> `docs/reference/SQL_COMPATIBILITY.md` for the full matrix and
> `docs/FAQ.md` for the short answers.

## 1. What is a near drop-in

- **Queries, schemas, triggers, views, indexes, PRAGMAs**: unchanged — the
  dialect is exactly the SQLite 3.53.4 dialect (`OBSERVED`).
- **Your data's exit path**: the materialized local image is a real SQLite
  file any SQLite tool can read — no lock-in (`OBSERVED`).

What is **not** a drop-in:

- **The connection**: OtsarDB exposes a C API (`include/otsardb.h`) and a
  CLI today. There is **no ORM driver bridge** (FUTURE FEATURE, not shipped —
  see `SQL_COMPATIBILITY.md` §3). SQLite-dialect ORMs emit compatible SQL but
  cannot connect without such a driver.
- **The storage target**: for the S3-backed model, the database is a
  directory prefix in object storage, not a shared file path.
- **COMMIT semantics**: a successful COMMIT is acknowledged only after
  durable publication to object storage — commit latency now includes
  provider round trips (`OBSERVED`, `docs/architecture/DURABILITY.md`).

## 2. Setup: environment

```bash
# explicit OtsarDB variables (highest priority)
export OTSARDB_S3_ACCESS_KEY="<your-access-key-id>"
export OTSARDB_S3_SECRET_KEY="..."
export OTSARDB_S3_REGION="us-east-1"

# non-AWS S3-compatible providers need the endpoint
export OTSARDB_S3_ENDPOINT="https://s3.example.com"

# Core-only providers (no CAS — e.g. Wasabi, Magalu) require the
# explicit single-writer assertion for write-capable opens:
export OTSARDB_S3_SINGLE_WRITER=1
```

Credential resolution order (explicit `OTSARDB_S3_*` → standard `AWS_*`
environment → `~/.aws/credentials` → IAM instance profile) is documented in
`docs/guides/QUICKSTART.md` §3.1-3.2.

## 3. Create the S3 target

```bash
# same SQL you already have, new target
otsardb s3://my-bucket/company/database \
  "CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT, email TEXT UNIQUE);"
otsardb s3://my-bucket/company/database \
  "INSERT INTO users(name, email) VALUES('João', 'joao@example.com');"
otsardb s3://my-bucket/company/database "SELECT * FROM users;"
```

Local-file mode still works unchanged (`otsardb app.db "..."`) — useful as an
interim step while you switch the deployment target.

## 4. Verify with a test database before switching

1. Export your existing database with standard SQLite tooling:
   `sqlite3 old.db .dump > schema.sql`.
2. Replay the dump's SQL against the S3 target statement by statement via the
   CLI (the OtsarDB CLI accepts SQL as a command-line argument and has an
   interactive REPL; its meta-command set is the one listed in
   `SQL_COMPATIBILITY.md` §3 — there is no `.read`/`.import` equivalent in the
   OtsarDB CLI).
3. Verify counts on both sides: `otsardb s3://my-bucket/company/database
   "SELECT count(*) FROM orders;"` must match `sqlite3 old.db "SELECT
   count(*) FROM orders;"`.
4. Prove the storage model works: reopen the S3 database with an empty local
   cache on the same or another machine (zero-local reopen) and re-check the
   counts. This is the behavior the 7/7 provider validation exercised on
   MinIO local/BR, R2, Wasabi and Magalu (`OBSERVED`; see
   `docs/S3_PROVIDERS.md`).
5. Run your own smoke workload (the queries your application really issues)
   against the S3 target before changing any application code.

## 5. Zero-code-change caveats (read all of them)

- **Queries stay the same; connection code does not.** If your application
  uses a SQLite driver/ORM, it cannot reach OtsarDB until a driver bridge
  exists (FUTURE FEATURE). Plan the C API/CLI integration explicitly.
- **Commit latency moves to the wire.** Recorded provider benchmark classes
  (`OBSERVED`, see `docs/performance/BENCHMARKS.md` §3): MinIO local bench P50 a/b/c
  211.2/212.2/226.5 ms; MinIO BR 1101.0/1275.8/1960.3 ms; R2
  5404.7/5070.0/8084.8 ms; Wasabi 2985.5/3387.7/5896.2 ms; Magalu
  3725.9/2003.8/7032.5 ms. A write-heavy workload that tolerated local-file
  latency may not tolerate WAN publication latency (`WHEN_NOT_TO_USE.md` §1).
- **Provider CAS decides writer topology.** CAS-capable providers (MinIO,
  Cloudflare R2) support multi-writer; Core-only providers (Wasabi, Magalu)
  fail closed without `OTSARDB_S3_SINGLE_WRITER=1` — an assertion, not a
  lock (`docs/reference/S3_COMPATIBILITY.md`).
- **Reads are session-snapshot.** A long-lived session does not see commits
  made by other instances until it reopens (`OBSERVED`; see
  `docs/architecture/CONSISTENCY_MODEL.md`).
  SQLite applications that shared one file across processes saw each other's
  commits; OtsarDB applications see them on reopen.
- **PRAGMA defaults are the engine's.** e.g. `PRAGMA foreign_keys` follows
  the SQLite default (OFF) until the application enables it — no OtsarDB
  override is applied.
- **Loadable extensions** (`.so`/`.dll`) are inherited engine behavior,
  `UNTESTED` in the OtsarDB binary.
- **`ATTACH`** across databases inside an S3 session is `UNTESTED`
  (`SQL_COMPATIBILITY.md` §2).
- **Durability is not backup.** S3 durability protects against process loss;
  it does not replace point-in-time recovery, accidental-delete protection or
  object-versioning policy (`docs/FAQ.md`, `WHEN_NOT_TO_USE.md` §6).

## 6. After migrating

- Keep the exit path alive: the local materialized image is a standard SQLite
  file — periodically verify an external SQLite tool can read a closed
  session's image (the documented exit strategy).
- Watch `docs/guides/KNOWN_LIMITATIONS.md` — it is the honest, current list
  of what beta OtsarDB does and does not do.
