# Migrating from PostgreSQL to OtsarDB

> Guide. **No drop-in claims**: OtsarDB speaks the SQLite 3.53.4 dialect and
> has no PostgreSQL wire protocol and no dialect translation layer
> (`OBSERVED`). This page is an honest matrix-based
> assessment: what maps, what does not, and how to size the work before you
> start. The full feature matrix is `docs/reference/SQL_COMPATIBILITY.md`;
> the fit boundary is `docs/WHEN_NOT_TO_USE.md`.

## 1. Start with the assessment checklist

Answer before writing any code. Each "yes" is work, each row names the cost.

| Do you use... | Verdict | Migration work |
|---|---|---|
| PL/pgSQL functions / triggers / stored procedures | **not supported** | rewrite as application logic or SQL-only triggers (SQLite triggers have no procedural body) |
| `CREATE TYPE ... AS ENUM` | **not supported** | `TEXT` + `CHECK (col IN (...))` |
| Sequences (`CREATE SEQUENCE`, `nextval`) | **not supported** | `INTEGER PRIMARY KEY AUTOINCREMENT` per table (no shared sequences) |
| Native arrays (`int[]`, `text[]`) | **not supported** | normalize into tables, or store JSON in TEXT |
| `jsonb` columns with index/GIN operations | **not supported** | TEXT + JSON1 functions (`json_extract`, `json_each`); JSONB-specific indexes must be reworked |
| `LISTEN/NOTIFY` | **not supported** | application-level polling or a message queue |
| Row-level security (RLS) | **not supported** | enforce in the application layer |
| Advisory locks | **not supported** | writer fencing is OtsarDB-internal (CAS/lease); user-level locks are app logic |
| Materialized views | **not supported** | plain views, or recompute into a table |
| `COPY ... FROM/TO` | **not supported** | bulk load = INSERT statements (CLI/C API) |
| `information_schema` / `pg_catalog` queries | **not supported** | `PRAGMA table_info`, `sqlite_master` |
| Schemas / `search_path` | **not supported** | one database per S3 prefix (or `ATTACH`, `UNTESTED` through the S3 session) |
| Extensions (PostGIS etc.) | **not supported** | remove or replace |
| Plain SQL (DML, CTEs, window functions, UPSERT, RETURNING) | supported | mostly syntax-level rewrites (below) |

## 2. Type mapping

| PostgreSQL | OtsarDB (SQLite dialect) | Notes |
|---|---|---|
| `integer` / `bigint` / `smallint` | `INTEGER` | 64-bit signed |
| `double precision` | `REAL` | |
| `numeric(p,s)` | `NUMERIC` | affinity, not enforced scale |
| `text` / `varchar` | `TEXT` | |
| `bytea` | `BLOB` | |
| `boolean` | `INTEGER CHECK (x IN (0,1))` | `TRUE`/`FALSE` are INTEGER 1/0 |
| `date` / `timestamp` / `timestamptz` | `TEXT` (ISO-8601) or `INTEGER` (unixepoch) | pair with `datetime(...)`/`strftime(...)` |
| `uuid` | `TEXT` (36-char convention) | no native type |
| `json` | `TEXT` + JSON1 functions | |
| `jsonb` | **not supported** (TEXT + JSON1 is the closest) | |
| arrays | **not supported** | |
| enum | **not supported** (`CHECK`) | |
| `serial` / identity | `INTEGER PRIMARY KEY AUTOINCREMENT` | |

## 3. DDL mapping

| PostgreSQL | OtsarDB |
|---|---|
| `CREATE TABLE ... (id SERIAL PRIMARY KEY)` | `CREATE TABLE ... (id INTEGER PRIMARY KEY AUTOINCREMENT)` |
| `CREATE SEQUENCE` / `nextval(...)` | not supported — `AUTOINCREMENT` only |
| `CREATE INDEX ... WHERE <pred>` (partial) | identical syntax |
| `CREATE INDEX ... ON t(expr)` (expression) | identical syntax |
| `ALTER TABLE ... ADD COLUMN` / `RENAME` | supported (engine-dialect) |
| `GENERATED ALWAYS AS (...) STORED` | supported (3.31+) |
| PL/pgSQL trigger body | SQL-only trigger body (`BEFORE/AFTER ... FOR EACH ROW`) |
| `CREATE SCHEMA` | not supported — separate S3 prefixes |
| `VACUUM` / `ANALYZE` | `VACUUM` / `ANALYZE` (engine pragma surface) |
| `SET` / `SHOW` session settings | `PRAGMA` |

## 4. Query-level mapping (worked before → after)

```sql
-- PostgreSQL: UPSERT
INSERT INTO users (email, name) VALUES ('a@x.com', 'Ana')
ON CONFLICT (email) DO UPDATE SET name = excluded.name;

-- OtsarDB: same UPSERT shape (SQLite ON CONFLICT)
INSERT INTO users (email, name) VALUES ('a@x.com', 'Ana')
ON CONFLICT (email) DO UPDATE SET name = excluded.name;
```

```sql
-- PostgreSQL: window function
SELECT user_id, rank() OVER (PARTITION BY user_id ORDER BY created_at DESC)
FROM orders;

-- OtsarDB: identical (window functions since 3.25)
SELECT user_id, rank() OVER (PARTITION BY user_id ORDER BY created_at DESC)
FROM orders;
```

```sql
-- PostgreSQL: JSON access
SELECT items->>'sku' FROM orders;

-- OtsarDB: JSON1 equivalent
SELECT json_extract(items, '$.sku') FROM orders;   -- or items->>'$.sku'
```

```sql
-- PostgreSQL: date arithmetic
SELECT created_at FROM orders WHERE created_at > now() - interval '7 days';

-- OtsarDB: datetime modifiers
SELECT created_at FROM orders WHERE created_at > datetime('now', '-7 days');
```

## 5. Worked schema example (before → after)

PostgreSQL:

```sql
CREATE TYPE order_status AS ENUM ('new', 'paid', 'shipped');

CREATE TABLE users (
  id SERIAL PRIMARY KEY,
  email TEXT UNIQUE NOT NULL,
  created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE orders (
  id SERIAL PRIMARY KEY,
  user_id INTEGER NOT NULL REFERENCES users(id),
  status order_status NOT NULL DEFAULT 'new',
  items JSONB NOT NULL,
  total NUMERIC(10,2) NOT NULL,
  created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX idx_orders_status ON orders(status) WHERE status <> 'shipped';
```

OtsarDB (same intent, dialect-legal SQL):

```sql
CREATE TABLE users (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  email TEXT UNIQUE NOT NULL,
  created_at TEXT NOT NULL DEFAULT (datetime('now'))
);

CREATE TABLE orders (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  user_id INTEGER NOT NULL REFERENCES users(id),
  status TEXT NOT NULL DEFAULT 'new'
    CHECK (status IN ('new', 'paid', 'shipped')),   -- replaces the enum
  items TEXT NOT NULL,                              -- JSON via JSON1 functions
  total NUMERIC NOT NULL,
  created_at TEXT NOT NULL DEFAULT (datetime('now'))
);

PRAGMA foreign_keys = ON;                           -- engine default is OFF

CREATE INDEX idx_orders_status ON orders(status) WHERE status <> 'shipped';
```

Representative queries after migration:

```sql
-- insert an order (defaults fire the same way)
INSERT INTO orders (user_id, items, total)
VALUES (1, '{"sku":"X-1","qty":2}', 99.90) RETURNING id;

-- reporting with a window function (unchanged semantics)
SELECT user_id, total,
       sum(total) OVER (PARTITION BY user_id) AS lifetime_total
FROM orders;

-- the partial index still serves the same predicate
SELECT count(*) FROM orders WHERE status <> 'shipped';
```

## 6. What this guide does not claim

- No claim of wire-protocol or driver compatibility (none exists).
- No claim of feature parity: the "not supported" rows above are hard
  boundaries of the dialect (`SQL_COMPATIBILITY.md` §7).
- No claim about performance parity: commit latency is bounded by object-store
  publication (provider bench classes in `docs/performance/BENCHMARKS.md` §3); a
  write-heavy OLTP system that depends on local write commits is in the
  "when not to use" territory (`WHEN_NOT_TO_USE.md` §1).

If the assessment checklist has many "yes" rows, the honest conclusion is to
stay on PostgreSQL — it is the right tool for that workload, and OtsarDB
explicitly does not compete on that axis.
