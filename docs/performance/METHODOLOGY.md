# OtsarDB — Benchmark Methodology

> How benchmarks must be run and reported in this repository, so that any
> recorded number is reproducible or clearly marked otherwise. This page
> codifies the harness conventions already in use and the project's
> evidence/reproducibility rules. Numbers that do not meet this bar
> are quoted only as `NOT RECORDED` / `NEEDS BENCHMARK` on
> `docs/performance/BENCHMARKS.md`.

## 1. Mandatory recorded fields

Every benchmark artifact must record, per run:

| Field | Example |
|---|---|
| Reproducible script + exact command | `powershell -ExecutionPolicy Bypass -File scripts/delta-wan-bench.ps1` with every env override spelled out |
| Hardware / OS / compiler / build config | Windows 11, MSVC x64 Release + Ninja; CPU/RAM if reported |
| Provider, region, endpoint, bucket, prefix | Cloudflare R2 (ENAM), region `auto`, bucket `otsardb-test` (credentials masked) |
| Dataset, schema, indexes, value distribution | 20,000-row single table `t(id INTEGER PRIMARY KEY, v TEXT)`, no secondary indexes |
| Transaction size | 100 tx × 10,000 rows (1M load); 500-row delta commit |
| Concurrency | writers × sessions; readers; retry budget |
| Cache state | cold (fresh empty cache dir), warm (reused stale cache), `OTSARDB_CACHE_REUSE` value, `OTSARDB_AUTO_CHECKPOINT`/`_COMMITS` |
| Exact commit + binary hash | commit `7c404d54...`, binary SHA-256 `E1978D1E...` |
| UTC timestamp + harness version | 2026-08-13T19:03:00Z, harness vN |
| Raw per-run results + logs | full sample list, never just the summary (no secrets) |
| Percentiles | method (nearest-rank convention), N, min / max / mean / p50 / p95 / p99 |

Artifacts go to git-ignored `scripts/validation-results/YYYYMMDD-<name>.json`;
credentials never enter code, commit history, logs, artifacts or docs.

## 2. Run rules

1. **Never best-run-only.** Report all runs, including the first. First-run
   effects (TCP/TLS warmup) are real data: the recorded R2 warm run 1
   (4968 ms) vs runs 2-5 (~2572-2646 ms) is the example — hiding it
   would misrepresent the distribution.
2. **One configuration per table.** Do not mix providers, hardware, cache
   states or binary versions in one percentile column.
3. **A SKIP is not a PASS.** A scenario that cannot run (missing
   credentials, missing env hook, harness crash) is recorded as SKIP with the
   reason — e.g. a `SKIP`/`NEEDS ENV` case.
4. **Measure a working system.** Every sample must verify its own correctness
   (e.g. `SELECT count(*) == expected` per run, the standard gate) — a
   benchmark run that silently failed is not a sample.
5. **Fixed percentile method.** Nearest-rank for
   comparability; N ≥ 5 minimum, and N large enough to resolve the tail you
   claim (at N=5, p99 == p95 by nearest-rank — the recorded data notes this
   limitation instead of hiding it).
6. **Cold/warm separation.** Cold samples use a fresh empty cache dir per
   run; warm samples reuse a verified-stale cache and must prove the delta
   path fired.
7. **Cleanup contract.** Harnesses clean only their own bucket prefix and
   must be idempotent (run → verify prefix empty).
8. **Projections are labeled.** Request counts derived from code structure
   are `PROJECTED` and never presented as measured results
   (`architecture/PERFORMANCE_MODEL.md` §4).

## 3. Reporting template

```markdown
| Benchmark | Value (recorded) | Hardware / OS | Provider / region | Commit | p50 / p95 / p99 | Evidence |
|---|---|---|---|---|---|---|
| <scenario> | <number + unit + N> | <machine> | <endpoint> | <sha> (binary <sha>) | <p50 / p95 / p99 (N)> | <artifact + date> |
```

Unmeasured cells say `NEEDS BENCHMARK` or `NOT RECORDED` — never a guessed
value. A new benchmark becomes citable evidence only after its artifact lands
and is quoted on `docs/performance/BENCHMARKS.md` (the page that records
it).

## 4. Existing harnesses (reference implementations)

| Harness | What it measures | Convention source |
|---|---|---|
| `scripts/validate-all-providers.ps1` | per-provider 7/7 capability+functional battery, bench P50 a/b/c | standard harness |
| `scripts/s3-concurrency-test.ps1` | two-writer CAS race, zero-loss reopen | standard harness |
| `scripts/remote-bench.ps1` | zero-local reopen percentiles | standard harness |
| `scripts/delta-wan-bench.ps1` | cold vs warm delta reopen on a real WAN provider | standard harness |
| `scripts/read-coherence-bench.ps1` | hot-read 0-GET proof + visibility latency | standard harness |
| `scripts/rpo-rto-test.ps1` / `rpo-wan-test.ps1` | RPO/RTO kill matrix | standard harness |
| `scripts/otsardb-test-suite.ps1` (`-Quick`/full) | engine tests, load (S8), providers, soak | standard harness |

## 5. Acceptance stance

CI (GitHub Actions) is OPTIONAL manual validation (`workflow_dispatch`
only). The acceptance gate is observed behavior on real
endpoints via the harnesses above and the local gates (`ci-local`, soak,
concurrency, `validate-all-providers`). A missing green CI badge is not a
validation gap and must not be listed as a project failure.
