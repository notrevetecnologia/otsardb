# OtsarDB Test Suite — one-command validation & report

`scripts/otsardb-test-suite.ps1` runs the OtsarDB validation battery with **one
command** and produces a **professional, shareable report** (self-contained
HTML + raw JSON) that the owner can produce ANYTIME — for a senior engineer
walkthrough, a release gate, or a repeatability demo.

The suite is rerunnable: every section cleans its own state (`mc rm` of its
own prefixes, fresh cache dirs), and nothing in the report or artifact
contains credentials — providers are recorded by endpoint/region/
bucket only.

---

## How to run

```powershell
# Quick mode — local only (needs: build-s3\otsardb.exe, build\otsardb.exe,
# local MinIO on 127.0.0.1:9000, mc.exe on PATH):
powershell -ExecutionPolicy Bypass -File .\scripts\otsardb-test-suite.ps1 -Quick

# Full mode — adds every provider in scripts/providers.json, the aggregate
# validate-all-providers.ps1 battery and the RPO/RTO harness:
powershell -ExecutionPolicy Bypass -File .\scripts\otsardb-test-suite.ps1

# Repeat the high-volume load section N times (repeatability demo):
powershell -ExecutionPolicy Bypass -File .\scripts\otsardb-test-suite.ps1 -Quick -Iterations 3

# Override the CAS-storm retry budget when testing a different lease envelope:
powershell -ExecutionPolicy Bypass -File .\scripts\otsardb-test-suite.ps1 -Quick -StormRetries 60
```

Preconditions:

- `build\otsardb.exe` (core build) and `build-s3\otsardb.exe` (S3 build) exist
  (driver: `configure-core/build-core` and `configure-s3/build-s3`).
- Local MinIO is running:
  `minio server <data-dir> --address 127.0.0.1:9000`.
- `mc.exe` is on PATH with the `local` alias set.
- `scripts/providers.json` (git-ignored) exists; providers with empty
  credentials are listed as SKIPPED and never fail the run.

Exit code: **0 only when every required section passed** (SKIP sections are
listed as SKIP and do not fail the run).

Output (both git-ignored under `scripts/validation-results/`):

- `otsardb-report-<utc>.html` — the self-contained report (open in any browser).
- `<utc>.json` — the raw machine-readable record of the same run.
- `<utc>-load.json` — the high-volume load section's per-iteration detail.

---

## What each section proves

| # | Section | Proves |
|---|---------|--------|
| S1 | Header | Reproducibility anchor: version, commit SHA, binary SHA-256, UTC timestamp, OS, compiler. |
| S2 | `ctest --test-dir build-s3` (full) | The whole deterministic C/C++ test battery (45 tests) is green on this binary. |
| S3 | Core smoke | The core CLI works on `:memory:` and `SELECT 6*7` returns 42. |
| S4 | Soak | Sustained writes: 41 sessions x 100 rows = 4100 rows, per-batch remote verification, auto-checkpoint at close, GC object accounting. |
| S5 | Concurrency 5/5 | Two-process CAS races: exactly one winner per race, no lost rows, no ghosts, zero-cache reopen consistent. |
| S6 | Per-provider | For every credential-bearing provider in `providers.json`: the 7-step acceptance (`s3-validation.ps1` — probe, write, zero-local reopen, durability guards, checkpoint recovery, bench) plus `remote-bench.ps1` P50/P95/P99 per phase (nearest-rank). Quick mode: local MinIO only; full mode: all providers. Single-writer providers run with `OTSARDB_S3_SINGLE_WRITER=1` (fail-closed mode). |
| S6b | `validate-all-providers.ps1` | The aggregate one-command battery over all providers, including CAS concurrency 5/5 for CAS providers. Full mode only. |
| S7 | RPO/RTO harness | Kill-point matrix (no confirmed commit lost), corruption/restore fail-closed, RTO percentiles. Full mode only. |
| S8 | HIGH-VOLUME LOAD | 1,000,000-row INSERT (100 transactions x 10,000 rows), 1M-row zero-local SELECT timing, canonical 10-writer storm (10 concurrent writers x 10 sessions, client-side rebase on BUSY), one 100,000-row transaction, and a footprint report (objects, total bytes, bytes/row) over the 1M database. The professional default is a lease-aware 60-attempt retry budget; repeated `-Iterations` use fresh prefixes. |
| S9 | Artifact linkage | Every section's JSON artifact is listed next to the report. |
| S10 | Summary + verdict | PASS/FAIL/SKIP table with durations; exit code decision. |

The C-API extended-code test (`tests/extended_error_test.c`,
wired by the coordinator into CMakeLists as `otsardb_extended_error`) is part
of section S2 once the coordinator lands the target; until then it runs
standalone:

```powershell
.\build-s3\extended_error_test.exe
# S1: race sync rc=517 ... S4: ... extended error codes ... passed
```

---

## How to read the report

- **Overall verdict banner** at the top: green PASS only when every required
  section passed.
- **Sections table**: status color-coded (green PASS / red FAIL / yellow
  SKIP), duration per section, and the detail tail (or artifact filename).
- **Per-provider table**: provider, endpoint, region, bucket (never keys),
  7/7 validation step counts and the bench P50 per phase.
- **SKIP is never PASS**: Quick mode lists the full-mode sections
  (S6b, S7) as SKIP; a provider without credentials is SKIP; a harness that
  crashes is SKIP with the documented reason (see S7 note below).
- Raw `JSON` carries the same data for tooling/re-citation.

### Known honest gaps in the report

- S7 (RPO/RTO): the harness's async-drain process pattern can crash
  Windows PowerShell on this host (WER `PSInvalidOperation`, exit 2 —
  reproduced and documented). When the crash occurs the
  section is listed as **SKIP with the documented reason**; the RPO/RTO
  evidence itself lives in the recorded artifacts
  (`20260810-rpo-rto.json`, `20260811-rpo-wan.json`).
- Bench P50s are per-run truth, not a performance guarantee: WAN providers
  (R2/Wasabi/Magalu/minio-br) vary with internet conditions; the report
  records what was observed.
- The 1M-row load runs against the local MinIO in both modes.

---

## Expected durations (this machine, 2026-08-12)

| Mode | Typical duration |
|------|------------------|
| `-Quick` | ~3 min (ctest ~35-65 s, soak ~11-18 s, concurrency ~7-10 s, providers-local ~15 s, load ~50-80 s) |
| full | ~30-35 min (per-provider battery ~13 min, aggregate battery ~16 min, RPO/RTO 20-50 min when it does not crash, load ~1 min) |

The suite is safe to rerun immediately: every prefix it creates is removed
at the end of the run (and stale prefixes from a crashed previous run are
removed at the start of the next).
