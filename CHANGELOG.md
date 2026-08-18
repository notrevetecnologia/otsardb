# Changelog

All notable changes to OtsarDB are recorded here, grouped by theme. This file
is the public summary of the project's change history. Evidence classes
(OBSERVED / UNTESTED / PROJECTED) follow the project's evidence rules — see
`docs/performance/BENCHMARKS.md` and `docs/guides/KNOWN_LIMITATIONS.md` for
recorded evidence and honest limits.

## Unreleased

### Engineering audit and handoff cleanup

- Validation harnesses now resolve their repository and temporary paths from
  the checkout instead of depending on one developer machine.
- Multi-attempt concurrency runs use a fresh cache per attempt and preserve
  per-session diagnostics, so retries cannot hide stale local state.
- Storage, security and release documentation now use the manifest-v4 and
  opt-in segment-encryption contracts.
- The engineering handoff and public-site publishing boundary were organized
  around current evidence. The corrected professional CAS stress now records
  100/100 writer sessions and 100 fresh rows; a separate aggressive diagnostic
  run with an 8-attempt budget defined a retry/liveness boundary at 97/100
  sessions without acknowledged-row loss; the release S8 gate now uses a
  lease-aware 60-attempt budget and passed 100/100.

The thematic entries below are historical summaries. For the current state,
see the architecture and storage documentation.

### Finalization audit

- Studio v0.7 responsive embedded UI: modern clean layout, narrow-screen
  single-column behavior, wrapped controls, wide-grid scrolling and
  preserved dark/light mode without external assets.
- Studio server now bounds incomplete HTTP reads at 5 seconds so a silent
  client cannot occupy the single worker indefinitely.
- Cross-platform contract clarified: browser mode is the portable path;
  the optional WebView2 desktop shell is Windows-only and falls back on
  Linux/macOS or when its loader is unavailable.
- Evidence docs synchronized, including two-node
  Docker-in-Docker RTO, three-destination quorum, C-API 517 and the
  release-tag mismatch found during audit.

### Browser-first platform contract

- Studio browser mode is now the official compatibility path for Windows,
  Linux, macOS and modern mobile browser viewports.
- The Windows/WebView2 `--window` shell is explicitly optional and does not
  define release acceptance.
- Documentation records the important boundary: responsive mobile UI is
  not a shipped Android/iOS binary and loopback-only Studio cannot remotely
  administer another computer from a phone.
- Audit corrected stale architecture and CMake/embedder statements and
  recorded remaining platform, security and test gaps honestly.

### External tester readiness

- Corrected beta/release packaging paths that referenced the old `docs/`
  layout instead of `docs/guides/`; packages now include the Studio guide.
- Updated the beta kit so Studio browser testing is part of onboarding, while
  native mobile binaries and remote phone administration remain out of scope.
- Added a sanitized `docs/beta/triage-log.md` so external findings have a
  recorded destination before invitations are sent.

Seed from the project's early engineering history; later workstreams are
recorded here as they land.

### Engine (SQLite-dialect core)

- OtsarDB engine bootstrap: one binary, embedded SQL engine, public C API,
  interactive shell and one-shot SQL.
- Compatibility and migration position (v0.1): SQLite 3.53.4 dialect in;
  materialized local image is a real SQLite file readable by standard
  SQLite tooling with the session closed.

### S3-authoritative storage

- S3-authoritative storage contract, backend-neutral object-store
  interface, capability probe instead of provider-name allowlist.
- Native `s3://bucket/database-root` target, stable logical identity,
  per-database VFS/publisher ownership, credential provider chain.
- Immutable content-addressed `.otsseg` segments, hash-linked commit
  manifest chain to genesis, HEAD as the authoritative publication point;
  LZ4 segment compression (~10x on WAL-page commits); reusable local cache
  with atomic metadata and bounded eviction.
- Durability contract: a commit is confirmed to the client only after
  remote durability succeeds (WAL `xSync`), with bounded publish retry
  and deterministic fault injection.

### Recovery, RPO/RTO

- Crash recovery using only S3-authoritative state; zero-local-state
  reopen; rolling checksums and verified-parent-state gates.
- RPO/RTO suite: kill-point matrix RPO-OK on local MinIO, RTO
  percentiles, two-instance takeover via expiry-proven CAS;
  container-level takeover with two real Docker containers; WAN RPO run
  on Cloudflare R2.

### Availability / HA

- Availability model levels A-F and exact claim wording; evidence rules
  and CI-optional policy.
- Writer lease phases 1+2 (CAS claim/renew, generation fence, candidate
  polling), HTTP write forwarding, health/readiness contract.

### Multi-S3

- Multi-S3 replication design record (models A/B/C) and Model B
  synchronous dual publish: ack only after COMMITTED on both
  destinations, deterministic commit identity.
- Forwarding/replication load test. Promotion/roll-forward
  remains out of v0 scope (design records).

### Performance

- Windowed parallel multipart upload (up to 3x throughput) and parallel
  segment pre-upload.
- Lazy hydration: manifest-validated cold opens, frame-level range-GET
  hydration, sequential read-ahead prefetch (R2 reopen 16.4s -> 3.5s,
  then -58% on sequential scans).
- Checkpoints (auto, threshold-based), GC wiring, bounded segment
  streaming, benchmark tooling and remote-bench harness.

### Providers

- Multi-provider validation harness and consolidated record:
  MinIO local, MinIO BR, Cloudflare R2, Wasabi, Magalu — 5 providers,
  validation 7/7 each, two-writer CAS concurrency 5/5 on the three CAS
  providers. AWS/B2 optional.

### Studio

- Studio vision (M6) and v0.1: `--json` exec mode and loopback-only web
  server; v0.2: `--json-schema`, connection profiles,
  schema explorer, paged result grid with CSV export.

### Portability, packaging, reliability hardening

- Windows MSVC build enablement and miniocpp select-patch; Linux port
  fixes and first Linux package.
- Root-caused and fixed the early-stage 0xC0000005 multipart/probe flake
  (libcurl shared connection pool serialized).
- Release artifact workflows and standard multi-OS release layout; beta
  packaging and launch kit.

## 0.1.0-dev — unreleased beta

Beta package `otsardb-0.1.0-dev (engine base 3.53.4)` for Windows x64;
Linux/macOS artifacts are UNTESTED until a release run
is observed. Full honest limitation list:
`docs/guides/KNOWN_LIMITATIONS.md`.
