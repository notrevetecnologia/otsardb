# OtsarDB — SQL Compatibility Reference

> Public reference. Evidence discipline (the project's evidence rules)
> applies throughout:
> `OBSERVED` = read from the cited source or recorded in the cited
> validation artifact; `PROJECTED` = derived, never presented as a result;
> `UNTESTED` = not yet observed; `FUTURE FEATURE` = design intent, not shipped.
> The SQL dialect contract is pinned by the engine base version; this file is
> the consolidated matrix. PostgreSQL/MySQL/MariaDB/SQL Server/Oracle appear
> here **only as compatibility boundaries** (not supported).

## 1. The compatibility position

OtsarDB speaks **exactly the SQLite 3.53.4 dialect**. The official SQLite
3.53.4 amalgamation is fetched at build time, checksum-verified and statically
compiled into the engine (`OBSERVED`, `docs/ARCHITECTURE.md`).
There is no dialect translation layer and no PostgreSQL/MySQL wire-protocol
adapter (`OBSERVED`).

Consequences, stated plainly:

- SQL written for SQLite 3.53.4 runs against OtsarDB **without query changes**.
- SQL written for PostgreSQL, MySQL/MariaDB, SQL Server or Oracle generally
  does **not** run unchanged; see the mapping tables and the "NOT compatible"
  list in sections 5–7.
- The storage model is not the SQLite file format: for an S3 database the
  authoritative state is the OtsarDB object layout (`docs/architecture/STORAGE_MODEL.md`),
  while the materialized local image remains a standard SQLite file readable by
  any SQLite tool — the documented exit path (`OBSERVED`).

## 2. SQLite 3.53.4 dialect support (inherited feature catalog)

Everything below is part of the pinned engine and is inherited by OtsarDB.
The catalog was verified against the build flags and code on 2026-08-10
(`OBSERVED`); feature-by-feature execution through the S3
path is covered by the engine test suites and the recorded end-to-end runs,
not by a per-feature compatibility matrix of our own.

| Area | Support in the OtsarDB dialect (SQLite 3.53.4) |
|---|---|
| Types | INTEGER, REAL, TEXT, BLOB, NUMERIC affinity, NULL; type affinity rules (flexible typing); `STRICT` tables (3.37+) |
| DDL | `CREATE/ALTER/DROP TABLE`, views, `CREATE INDEX`, `REINDEX`, `VACUUM` |
| DML/DQL | full SELECT/INSERT/UPDATE/DELETE; `LIMIT/OFFSET`; `GROUP BY`/`HAVING`; subqueries; `UNION/INTERSECT/EXCEPT` |
| TCL | `BEGIN/COMMIT/ROLLBACK`, `SAVEPOINT`, nested transactions |
| CTE | `WITH`, recursive CTEs, `MATERIALIZED`/`NOT MATERIALIZED` hints (3.35+) |
| Window functions | full set (3.25+): `row_number`, `rank`, `dense_rank`, `lag`, `lead`, `ntile`, `first_value`, `last_value`, aggregates over windows |
| UPSERT | `INSERT ... ON CONFLICT DO UPDATE / DO NOTHING` (3.24+) |
| RETURNING | `DELETE/INSERT/UPDATE ... RETURNING` (3.35+) |
| Generated columns | `GENERATED ALWAYS AS (...) STORED/VIRTUAL` (3.31+) |
| JSON | JSON1 functions core since 3.38: `json_extract`, `json_array`, `json_object`, `json_each`, `json_set`, `json_patch`, JSON operators `->`/`->>`; JSON stored as TEXT (no binary JSONB type) |
| FTS5 | full-text search (`SQLITE_ENABLE_FTS5` build flag) |
| RTREE | R-tree spatial indexes (`SQLITE_ENABLE_RTREE`) |
| Math functions | `SQLITE_ENABLE_MATH_FUNCTIONS`: `sin`, `cos`, `log`, `sqrt`, `pi`, etc. |
| Indexes | B-tree indexes, **partial indexes** (`WHERE` clause), **expression indexes** (index on expressions) |
| Triggers | `BEFORE/AFTER/INSTEAD OF`, `FOR EACH ROW`, `WHEN` clauses; trigger bodies are SQL, not procedural code |
| Foreign keys | declared and enforceable via `PRAGMA foreign_keys`; enforcement follows the engine default (`OFF`) unless the application enables it, exactly as in SQLite |
| CHECK constraints | `CHECK` on columns and tables |
| ATTACH | `ATTACH DATABASE` is part of the dialect; see the honesty note below |
| PRAGMA | SQLite `PRAGMA` surface (`journal_mode`, `foreign_keys`, `table_info`, `database_list`, ...) |
| Hooks/introspection | session extensions (`SQLITE_ENABLE_SESSION`), pre-update hooks (`SQLITE_ENABLE_PREUPDATE_HOOK`), `dbstat` virtual table (`SQLITE_ENABLE_DBSTAT_VTAB`), column metadata (`SQLITE_ENABLE_COLUMN_METADATA`) |

Honesty notes:

- **ATTACH**: supported by the engine, but attaching other databases inside an
  OtsarDB S3 session is `UNTESTED` in the validation record (the authoritative
  S3 model covers one database per S3 prefix; a second local/attached database
  is not part of the recorded durability envelope).
- **Loadable extensions** (`.so`/`.dll`): inherited engine behavior, `UNTESTED`
  in the OtsarDB binary (`OBSERVED` risk note).
- **Compile flags** (`OBSERVED`, `CMakeLists.txt`):
  `SQLITE_THREADSAFE=1`, `SQLITE_ENABLE_COLUMN_METADATA`,
  `SQLITE_ENABLE_DBSTAT_VTAB`, `SQLITE_ENABLE_FTS5`,
  `SQLITE_ENABLE_MATH_FUNCTIONS`, `SQLITE_ENABLE_RTREE`,
  `SQLITE_ENABLE_SESSION`, `SQLITE_ENABLE_PREUPDATE_HOOK`.

## 3. Connection surface today: C API + CLI

The two supported ways to talk to OtsarDB today are:

- **C API** — `include/otsardb.h`: `otsardb_initialize`, `otsardb_open`,
  `otsardb_exec`, `otsardb_close` (`OBSERVED`, header in tree).
- **CLI** — `otsardb <target> "<SQL>"` (multi-argument join) and an
  interactive REPL; meta-commands: `.help`, `.tables`, `.schema`, `.status`,
  `.version`, `.clear`, `.quit`, `.exit` (`OBSERVED`).

### ORM note (stated honestly)

SQLite-dialect ORMs generate SQL that matches the OtsarDB dialect, but **no
ORM driver, bridge, wire protocol or language binding exists today**. An
application's connection layer must be the C API or the CLI. A driver bridge
that exposes the C API to language ecosystems is a **FUTURE FEATURE**, not
shipped, not tested (`UNTESTED`). Do not plan a migration on the assumption
that an ORM will connect transparently: it will not, today.

## 4. Type mapping vs other dialects

Representative mapping (not exhaustive). "Not supported" means the OtsarDB
dialect has no such type; the "workaround" column is SQL-level guidance, not
an OtsarDB feature.

| OtsarDB (SQLite) | PostgreSQL | MySQL/MariaDB | SQL Server | Oracle | Notes / workaround |
|---|---|---|---|---|---|
| INTEGER | integer / bigint / smallint | INT / BIGINT / SMALLINT | INT / BIGINT | NUMBER(19) | 64-bit signed |
| REAL | double precision | DOUBLE | FLOAT | BINARY_DOUBLE | 8-byte float |
| NUMERIC | numeric(p,s) | DECIMAL(p,s) | DECIMAL(p,s) | NUMBER(p,s) | numeric affinity, no enforced scale |
| TEXT | text / varchar | VARCHAR / TEXT | NVARCHAR | VARCHAR2 | unlimited length by engine bounds (see LIMITS) |
| BLOB | bytea | BLOB / VARBINARY | VARBINARY | BLOB / RAW | raw bytes |
| (none) | boolean | BOOLEAN | BIT | NUMBER(1) | SQLite has no boolean type: `TRUE`/`FALSE` literals are INTEGER 1/0; use `INTEGER CHECK (x IN (0,1))` |
| (none) | date / time / timestamp(tz) | DATETIME / TIMESTAMP | DATETIME2 | DATE / TIMESTAMP | store as TEXT (ISO-8601) or INTEGER (unixepoch); see function table |
| (none) | uuid | (none; use CHAR(36)) | UNIQUEIDENTIFIER | (none) | TEXT 36-char convention; no native UUID type |
| (none) | jsonb (binary) | JSON | (none) | (none) | TEXT + JSON1 functions; no binary JSON, no JSONB index ops |
| (none) | array / int[] | (none) | (none) | (none) | **not supported**; normalize or store JSON in TEXT |
| (none) | enum | ENUM | (none) | (none) | **not supported**; `CHECK (col IN (...))` |
| (none) | serial / identity / sequence | AUTO_INCREMENT | IDENTITY | SEQUENCE + trigger | `INTEGER PRIMARY KEY AUTOINCREMENT` (per-table; no shared sequences) |
| NULL | NULL | NULL | NULL | NULL | same |

## 5. DDL mapping vs other dialects

| Concept | OtsarDB (SQLite) | PostgreSQL | MySQL/MariaDB | SQL Server | Oracle |
|---|---|---|---|---|---|
| Auto-increment PK | `INTEGER PRIMARY KEY AUTOINCREMENT` | `SERIAL` / `GENERATED ... AS IDENTITY` | `AUTO_INCREMENT` | `IDENTITY(1,1)` | `GENERATED ... AS IDENTITY` or sequence+trigger |
| Sequence object | **not supported** | `CREATE SEQUENCE` | `CREATE SEQUENCE` (MariaDB 10.3+) | `CREATE SEQUENCE` | `CREATE SEQUENCE` |
| UPSERT | `ON CONFLICT DO UPDATE` | `ON CONFLICT` (same shape) | `ON DUPLICATE KEY UPDATE` | `MERGE` | `MERGE` |
| RETURNING | `... RETURNING` | `... RETURNING` | MariaDB `RETURNING`; MySQL: not available | `OUTPUT` clause | `RETURNING ... INTO` |
| Partial index | `CREATE INDEX ... WHERE <pred>` | same (`WHERE`) | **not supported** | filtered index (`WHERE`) | not supported (function-based only) |
| Expression index | `CREATE INDEX ... ON t(expr)` | same | functional indexes (8.0+) | computed-column index | function-based index |
| Generated column | `GENERATED ALWAYS AS (...) STORED/VIRTUAL` | `GENERATED ALWAYS AS (...) STORED` | `GENERATED ALWAYS AS ... STORED` | computed column | virtual column |
| CHECK constraint | `CHECK (...)` | same | enforced 8.0.16+ | same | same |
| Foreign keys | declared; enforced when `PRAGMA foreign_keys=ON` (default OFF) | enforced by default | enforced by default | enforced by default | enforced by default |
| Trigger body | SQL statements only | PL/pgSQL (procedural) | procedural SQL | procedural T-SQL | procedural PL/SQL |
| Materialized view | **not supported** | `CREATE MATERIALIZED VIEW` | not supported (MariaDB: no) | indexed view | materialized view |
| Schema/namespace | one main DB + `ATTACH` | `CREATE SCHEMA` / search_path | database name | schema | schema |
| Session settings | `PRAGMA` | `SET` / `SHOW` | `SET SESSION` | hints / `SET` | `ALTER SESSION` |
| Statement introspection | `PRAGMA table_info`, `sqlite_master` | `information_schema`, `pg_catalog` | `information_schema` | `sys.*` | `user_tables` etc. |

## 6. Function mapping vs other dialects (representative)

| OtsarDB (SQLite) | PostgreSQL | MySQL/MariaDB | SQL Server | Oracle |
|---|---|---|---|---|
| `length(x)` | `length(x)` | `LENGTH(x)` | `LEN(x)` | `LENGTH(x)` |
| `substr(x,a,b)` | `substring(x,a,b)` | `SUBSTRING(x,a,b)` | `SUBSTRING(x,a,b)` | `SUBSTR(x,a,b)` |
| `coalesce(a,b)` | same | same | same | same |
| `nullif(a,b)` | same | same | same | same |
| `ifnull(a,b)` | `coalesce(a,b)` | `IFNULL(a,b)` | `ISNULL(a,b)` | `NVL(a,b)` |
| `random()` | `random()` | `RAND()` | `RAND()` | `DBMS_RANDOM.VALUE` |
| `printf('fmt',...)` | `format(...)` | `FORMAT(...)` (different semantics) | `FORMAT(...)` | not direct |
| `instr(haystack,needle)` | `strpos(...)` | `INSTR` / `LOCATE` | `CHARINDEX` | `INSTR` |
| `datetime('now')` | `now()` | `NOW()` | `GETDATE()` | `SYSDATE` |
| `datetime('now','-7 day')` | `now() - interval '7 days'` | `DATE_SUB(NOW(), INTERVAL 7 DAY)` | `DATEADD(day,-7,GETDATE())` | `SYSDATE - 7` |
| `strftime('%Y-%m-%d',x)` | `to_char(x,'YYYY-MM-DD')` | `DATE_FORMAT(x,'%Y-%m-%d')` | `FORMAT(x,'yyyy-MM-dd')` | `TO_CHAR(x,'YYYY-MM-DD')` |
| `group_concat(x)` | `string_agg(x,',')` | `GROUP_CONCAT(x)` | `STRING_AGG(x,',')` | `LISTAGG(x,',')` |
| `json_extract(j,'$.a')` / `j->>'$.a'` | `j->>'a'` | `JSON_EXTRACT(j,'$.a')` | `JSON_VALUE(j,'$.a')` | `JSON_VALUE(j,'$.a')` |
| `json_each(j)` | `jsonb_array_elements(...)` | `JSON_TABLE` | `OPENJSON` | `JSON_TABLE` |
| `CAST(x AS t)` | same | same | same | same |
| `EXISTS/IN` subqueries | same | same | same | same |

The tables above are representative, not exhaustive. For the authoritative
function reference use the SQLite 3.53.4 documentation of the pinned engine.

## 7. Explicitly NOT compatible

These PostgreSQL-family features have **no OtsarDB counterpart today** and no
translation layer:

| Feature | OtsarDB status |
|---|---|
| PL/pgSQL | **not supported** — no procedural language |
| Stored procedures / stored functions | **not supported** (SQL only) |
| Native arrays (`int[]`, `text[]`) | **not supported** (normalize or JSON TEXT) |
| JSONB (binary JSON, GIN index ops) | **not supported** (TEXT + JSON1 functions only) |
| Enums (`CREATE TYPE ... AS ENUM`) | **not supported** (`CHECK` constraint instead) |
| Sequences (`CREATE SEQUENCE`, `nextval`) | **not supported** (`AUTOINCREMENT` per table only) |
| LISTEN/NOTIFY | **not supported** (no client-server channel) |
| Row-level security (RLS) | **not supported** |
| Advisory locks (`pg_advisory_lock`) | **not supported** (writer fencing is CAS/lease-based, not user-lock-based) |
| Materialized views | **not supported** (plain views only) |
| `COPY` (bulk protocol) | **not supported** (bulk load = INSERT statements via CLI/C API) |
| `INFORMATION_SCHEMA` / catalog queries | **not supported** (use `PRAGMA` + `sqlite_master`) |
| PostgreSQL wire protocol | **out of scope** (explicitly not a v0.1 surface) |
| MySQL/MariaDB wire protocol | **out of scope** |
| Extensions (`CREATE EXTENSION`) | **not supported** |

For the practical consequences, read
`docs/guides/MIGRATING_FROM_POSTGRESQL.md` (honest matrix-based guide, no
drop-in claims) and `docs/WHEN_NOT_TO_USE.md` (fit boundary).
