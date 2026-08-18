# OtsarDB — Public Benchmarks

> Honesty contract: every number on this page is a **real recorded value**
> with its date, binary and endpoint, quoted from the project's recorded
> evidence. Cells that were
> never measured say **NEEDS BENCHMARK** — there is no number for them
> anywhere in the validation record, and none is invented here. Methodology
> (how these numbers must be produced) lives in
> `docs/performance/METHODOLOGY.md`.

## 1. How to read the tables

Each recorded result answers: **what**, **hardware/OS**, **provider/region**,
**exact commit**, and **percentiles**. Where the original artifact did not
record one of those fields, the cell says `NOT RECORDED` — an honest gap, not
a blank to be filled by guessing.

| Field | Meaning |
|---|---|
| Hardware / OS | machine, OS, compiler, build config of the recording |
| Provider / region | endpoint + region + bucket used |
| Commit | exact repo commit (for reproducibility) |
| p50 / p95 / p99 | nearest-rank percentiles, with N, as recorded |

## 2. Read path — real numbers

| Benchmark | Value (recorded) | Hardware / OS | Provider / region | Commit | p50 / p95 / p99 | Evidence |
|---|---|---|---|---|---|---|
| Hot point lookups (10,000 indexed SELECTs, warm session) | **0 added S3 GETs** (control=13, hot=13); P50 0.0917 ms, P95 0.1574 ms, P99 0.9323 ms | Windows 11, MSVC x64 Release | MinIO local, `127.0.0.1:9000`, bucket `otsardb-ci` | `7c404d54ac17ed31acf7bac2479cbe33bb3ba9c2` (binary SHA-256 `E1978D1E...A3A7D5A`) | p50 0.0917 / p95 0.1574 / p99 0.9323 (N=10000) | 2026-08-13 |
| Fresh-reopen visibility delta (single-writer commit → fresh session sees it) | ~180.7 ms (3 rounds: 195.6 / 182.6 / 180.7 ms) | same | same | same | single-round values above | 2026-08-13 |
| Zero-local reopen (local) | 100.5 ms (before lazy hydration: 205 ms) | NOT RECORDED | MinIO local | 0043-era binary | NOT RECORDED (point value) | 2026-08-10 |
| Cold zero-cache lazy reopen, 20,000-row DB | P50 **4784.1 ms** (N=5) | Windows 11, MSVC x64 Release | Cloudflare R2 (ENAM), region `auto`, bucket `otsardb-test` | tree `4133930e` (binary SHA-256 `2e3d5177...8cfa5`) | p50 4784.1 / p95 5456.7 / p99 5456.7 (N=5) | 2026-08-12 |
| Warm delta reopen (same DB, stale cache) | P50 **2645.7 ms** (N=5) — **1.81x faster** than cold | same | same | same | p50 2645.7 / p95 4968.0 / p99 4968.0 (N=5) | 2026-08-12 |

> The R2 N=5 runs resolve P95 only at N=5 (nearest-rank); P99 equals P95
> there. A larger-N tail is a NEEDS BENCHMARK row (§5).

## 3. Write path — real numbers

| Benchmark | Value (recorded) | Hardware / OS | Provider / region | Commit | p50 / p95 / p99 | Evidence |
|---|---|---|---|---|---|---|
| Commit request model, small tx | 1 HEAD GET + **3 PUTs** (segment + manifest + HEAD) — `PROJECTED` from code, not re-measured live | n/a | n/a | inspected at `58c55f79...` | n/a | `architecture/PERFORMANCE_MODEL.md` §2 |
| Bench P50 a/b/c (open+create+insert / zero-local reopen+SELECT / big tx 20k rows) | MinIO local 211.2 / 212.2 / 226.5 ms | Windows dev host | MinIO local | 2026-08-10 run | recorded as phase P50s (per-run artifact has P95/P99) | 2026-08-10 |
| same | MinIO BR 1101.0 / 1275.8 / 1960.3 ms | same | MinIO BR datacenter | same | same | same |
| same | R2 5404.7 / 5070.0 / 8084.8 ms | same | Cloudflare R2 (ENAM) | same | same | same |
| same | Wasabi 2985.5 / 3387.7 / 5896.2 ms | same | Wasabi us-east-1 | same | same | same |
| same | Magalu 3725.9 / 2003.8 / 7032.5 ms | same | Magalu Cloud br-se1 | same | same | same |
| 1,000,000-row INSERT (100 tx × 10,000 rows) | **36.4 s** (Quick run) / **22.9 s** (Full run) | Windows 11, MSVC x64 | MinIO local | `e8594df772cf33f6f089cbbc8b06521e7551183d` (binary SHA-256 `28B1696B...EFD907`) | NOT RECORDED (single run per mode) | 2026-08-12 |
| 1,000,000-row SELECT | **0.3 s** (Quick) / **0.2 s** (Full), count verified | same | same | same | NOT RECORDED | 2026-08-12 |
| 100,000-row single transaction | 0.6 s (Quick) / 0.3 s (Full) | same | same | same | NOT RECORDED | 2026-08-12 |
| 10-writer × 10-session CAS storm (local envelope) | **100/100 sessions, 100 fresh rows** (lease-aware 60-attempt budget) | same host class | MinIO local | 2026-08-13 corrected professional S8 run | n/a (count-based) | 2026-08-13 |
| Multipart parallel upload | 13.2 → **38.5 MB/s** at 16 parts (relay ~110 ms/req) | NOT RECORDED | local relay/MinIO | 0039-era run | NOT RECORDED | 2026-08-10 |

## 4. Availability / RTO — real numbers

| Benchmark | Value (recorded) | Hardware / OS | Provider / region | Commit | p50 / p95 / p99 | Evidence |
|---|---|---|---|---|---|---|
| Single-node Swarm RTO (kill → first valid write) | P50 **6027 ms** (6027/6046/5981); detection P50 547 ms | WSL2 Docker Swarm | containerized MinIO | recorded in the change record | per-iteration values listed | 2026-08-11 |
| Two-node Docker-in-Docker Swarm failover (machine loss) | RTO P50 **19.1 s**, client reconnect P50 **13.5 s** (6 losses, zero loss/split-brain) | WSL2 Docker-in-Docker | containerized MinIO | recorded in the change record | P50s as listed | 2026-08-12 |
| Two-instance takeover, MinIO local (TTL 10 s) | RTO P50 **4455 ms**, P95/P99 4584 ms | Windows host | MinIO local | 0048-era binary | p50 4455 / p95 4584 / p99 4584 | 2026-08-10 |
| Two-instance takeover, R2 (TTL 30 s) | 15807 ms (**N=1**; fired earlier than theoretical expiry — clock offset, recorded INCONCLUSIVE) | Windows host | Cloudflare R2 | 0048-era binary | single sample | 2026-08-10 |
| Container-level takeover (3 iterations) | mean ~6153 ms (6901 / 7082 / 4477 ms) | WSL2 Docker | containerized MinIO | recorded in the change record | per-iteration values listed | 2026-08-11 |

## 5. NEEDS BENCHMARK (no number exists — do not invent)

| Missing cell | What is missing | Current status |
|---|---|---|
| Hot-read throughput at scale (ops/s) | per-query P50 exists (2026-08-13); sustained throughput has never been measured | NEEDS BENCHMARK |
| Write transaction latency p50/p95/p99 per provider, N ≥ 50 | only phase P50s (a/b/c) at small N and single load runs exist | NEEDS BENCHMARK |
| R2 cold reopen tail at N ≥ 20 | N=5 resolves P95 only (p99 == p95 at N=5) | NEEDS BENCHMARK |
| 1M-row load on a WAN provider (R2 / Wasabi) | 1M numbers are local-MinIO only | NEEDS BENCHMARK |
| Per-provider multipart write amplification | multipart throughput measured once on a local relay; per-provider large-object costs never measured | NEEDS BENCHMARK |
| WAN-degraded delta request accounting | explicitly open item | NEEDS BENCHMARK |
| Multi-writer contention beyond the recorded envelope | >10-writer, WAN, provider throttling, alternate retry budgets — unproven | NEEDS BENCHMARK |
| Physical-host Swarm machine loss | only Docker-in-Docker evidence exists; physical hosts UNTESTED | NEEDS BENCHMARK |
| S3-down hot read latency | harness case is `SKIP`/`NEEDS ENV` (no local S3 proxy hook) | NEEDS ENV |
| macOS / Linux release-binary performance | Windows-only evidence | NEEDS BENCHMARK |

## 6. Where the raw artifacts live

`scripts/validation-results/` (git-ignored, no credentials): dated provider
matrices, delta-WAN runs, read-coherence runs, RPO/RTO runs. The
narrative record is this page plus the project changelog — if a
number is not here, it does not exist as evidence.
