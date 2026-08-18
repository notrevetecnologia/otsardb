# OtsarDB — Limits Reference

> Public reference. Honest separation of three kinds of bound:
> **theoretical engine bounds** (inherited from the pinned SQLite 3.53.4
> amalgamation — never validated at OtsarDB scale), **tested scale**
> (`OBSERVED` — recorded with date and artifact), and **explicit unknowns**
> (`UNTESTED`). No number below converts a projection into a result
> (per the project's evidence rules).

## 1. Theoretical engine size bounds (inherited, NOT tested scale)

These are the official limits of the pinned SQLite 3.53.4 engine, inherited
by OtsarDB because the amalgamation is compiled in unchanged. They are
**engine documentation, not OtsarDB validation**: nothing in the validation
record exercises any of these bounds (`UNTESTED` at this scale).

| Bound | Value (engine theoretical) | OtsarDB-tested scale |
|---|---|---|
| Maximum database size | ~281 TB (2^48 bytes) | **UNTESTED** (see §2) |
| Maximum pages per database | 2,147,483,646 | **UNTESTED** |
| Maximum rows per table | 2^64 (bounded by database size) | **UNTESTED** |
| Maximum columns per table | 2000 | **UNTESTED** |
| Maximum SQL statement length | 1,000,000,000 bytes | **UNTESTED** |
| Maximum bound parameters | 32,766 | **UNTESTED** |
| Maximum TEXT/BLOB length | 1,000,000,000 bytes | **UNTESTED** |
| Attached databases | 10 default (compile max 125) | ATTACH through the S3 session **UNTESTED** |
| Expression tree depth / trigger recursion | 1000 | **UNTESTED** |
| Tables in a single join | 64 | **UNTESTED** |

Do not cite the left column as "OtsarDB scale". The tested scale is the
right column's citation and §2.

## 2. Tested scale (OBSERVED, with artifacts)

| What | Real result | Evidence |
|---|---|---|
| 1,000,000-row load, local MinIO | 1M INSERT in 36.4 s (Quick run) / 22.9 s (Full run); 1M SELECT 0.3 s / 0.2 s; storm 100/100; 100k-row tx 0.6 s / 0.3 s; footprint 204 objects / 108,544 bytes / 0.11 bytes/row | 2026-08-12, `otsardb-test-suite.ps1` |
| 20,000-row benchmark database | cold zero-cache lazy reopen P50 4784 ms vs warm delta reopen P50 2646 ms on R2 (N=5) | 2026-08-12, `delta-wan-bench.ps1` |
| 100,000-row single transaction | 0.3–0.6 s (local MinIO, machine-dependent) | 2026-08-12 |
| 10,000-row footprint | 82 KiB ≈ 122k rows/MiB ≈ 8.4 B/row | see `docs/performance/BENCHMARKS.md` §2 |
| 500,000-row footprint | 4.17 MiB compressed segments (LZ4) vs 9.98 MiB raw SQLite | see `docs/performance/BENCHMARKS.md` §2 |
| Hot point lookups | 10,000 lookups: 0 added S3 GETs; P50 0.0917 ms, P95 0.1574 ms, P99 0.9323 ms | 2026-08-13, local MinIO |

## 3. OtsarDB-specific bounds (OBSERVED in code)

| Bound | Value | Where |
|---|---|---|
| Segment payload limit | **16 MiB default** (`otsardb_wal_publisher_set_segment_payload_limit`) | `docs/architecture/PERFORMANCE_MODEL.md` §3 |
| Segments per commit | max **128** (`OTSARDB_MANIFEST_MAX_SEGMENTS`); a transaction exceeding it fails at `xSync` (never commits partially) | `docs/architecture/PERFORMANCE_MODEL.md` §2 |
| Publication points per commit | exactly **1 HEAD write** (segments/manifest are immutable + idempotent) | `docs/architecture/DURABILITY.md` |
| Write request model | small tx ≈ 1 HEAD GET + 3 PUTs (segment + manifest + HEAD) — `PROJECTED` from code, not re-measured live | `docs/architecture/PERFORMANCE_MODEL.md` §2 |

## 4. Concurrency limits

| Scenario | Validated bound | Status |
|---|---|---|
| Two-writer CAS races | 5/5 PASS on MinIO local, MinIO BR, Cloudflare R2 | `OBSERVED` 2026-08-10 |
| High-volume multi-writer, local CAS envelope | 10 concurrent writers × 10 sessions: **100/100 sessions, 100 fresh rows** with a lease-aware 60-attempt retry budget | `OBSERVED` 2026-08-13 (corrected professional S8) |
| Reduced retry budget | 8-attempt diagnostic run: 97/100 sessions, 97 fresh rows, **no acknowledged rows lost** | `OBSERVED` 2026-08-13 |
| Arbitrary >10-writer contention, WAN, provider throttling, other retry budgets | — | **UNTESTED** (explicitly not proven; `WHEN_NOT_TO_USE.md` §5) |
| Multi-writer on Core-only providers (Wasabi, Magalu) | **not available** — single-writer assertion required (fail-closed) | `OBSERVED` |
| AWS S3 / Backblaze B2 (any mode) | — | **UNTESTED** (no accounts) |

## 5. Read freshness limits

- A session reads its **snapshot pinned at open**: commits from other
  instances become visible only on reopen (delta path) or on a fenced write
  attempt returning BUSY. There is **no auto-refresh / "FRESH" mode today**;
  TTL/poll/Coordinator freshness modes are design ideas only, not implemented
  (`OBSERVED`).
- S3-down: hot-cache reads continue locally; writes and cold opens fail
  closed. The S3-down hot-read case is `SKIP`/`NEEDS ENV` in the measurement
  harness — `UNTESTED` end-to-end.
- RTO/RPO evidence and its scope: `docs/AVAILABILITY_MODEL.md` and
  `docs/performance/BENCHMARKS.md` §4.

## 6. Tested vs untested, one table

| Claim you may make | Claim you may NOT make |
|---|---|
| 1M-row load on local MinIO with recorded timings | any database-size bound (281 TB etc.) as OtsarDB-proven |
| 10-writer × 10-session local CAS envelope, 100/100 with the 60-attempt budget | arbitrary writer counts / WAN / retry policies |
| CAS multi-writer on MinIO (local/BR) and R2 | CAS multi-writer on AWS or B2 |
| single-writer 7/7 on Wasabi and Magalu | multi-writer on Wasabi/Magalu |
| 16 MiB segment payload default, 128 segments/commit max | per-page object amplification (there is none; amplification is per segment — `PROJECTED`) |
| session-snapshot reads, visibility via reopen | any auto-refresh / FRESH mode |
