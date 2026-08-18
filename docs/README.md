# OtsarDB Documentation

Index of the public documentation. One topic per file; the docs follow
UPPER_SNAKE.md naming. For the project's engineering process (evidence
rules, change-record protocol) read `CONTRIBUTING.md` at the repository
root.

## Overview

- [ARCHITECTURE.md](./ARCHITECTURE.md) — system architecture: execution vs durable state, VFS boundary, pipeline.
- [AVAILABILITY_MODEL.md](./AVAILABILITY_MODEL.md) — availability levels A-F, exact claim wording, `otsardb --health` contract.
- [CONCURRENCY.md](./CONCURRENCY.md) — lease + CAS + generation fencing, multi-writer semantics.
- [WAL_PIPELINE.md](./WAL_PIPELINE.md) — WAL read, segment encode, durability sync path.
- [RELEASES.md](./RELEASES.md) — release and packaging layout.

## Architecture

- [architecture/STORAGE_MODEL.md](./architecture/STORAGE_MODEL.md) — object layout, immutable segments, hash-linked chain.
- [architecture/CONCURRENCY.md](./architecture/CONCURRENCY.md) — writer coordination, two-writer race, 517-vs-5.
- [architecture/DURABILITY.md](./architecture/DURABILITY.md) — commit protocol, ACID, failure model.
- [architecture/CONSISTENCY_MODEL.md](./architecture/CONSISTENCY_MODEL.md) — read contract: session snapshot, read-your-writes.
- [architecture/CACHE_COHERENCE.md](./architecture/CACHE_COHERENCE.md) — data-cache validity vs global freshness.
- [architecture/PERFORMANCE_MODEL.md](./architecture/PERFORMANCE_MODEL.md) — hot/warm/cold paths, where HTTP enters.

## Storage & S3

- [S3_STORAGE.md](./S3_STORAGE.md) — object layout, commit manifests, recovery semantics.
- [S3_PROVIDERS.md](./S3_PROVIDERS.md) — provider configuration, capability and CAS notes.

## Reference

- [FAQ.md](./FAQ.md) — common questions, short and sourced.
- [WHEN_TO_USE.md](./WHEN_TO_USE.md) — where OtsarDB fits.
- [WHEN_NOT_TO_USE.md](./WHEN_NOT_TO_USE.md) — where it does not (read this too).
- [reference/GLOSSARY.md](./reference/GLOSSARY.md) — terms: generation, HEAD, segment, CAS, lease, snapshot.
- [reference/LIMITS.md](./reference/LIMITS.md) — tested scale, engine bounds, explicit unknowns.
- [reference/S3_COMPATIBILITY.md](./reference/S3_COMPATIBILITY.md) — provider matrix and capability reference.
- [reference/SQL_COMPATIBILITY.md](./reference/SQL_COMPATIBILITY.md) — SQL dialect contract and cross-dialect matrix.

## Performance

- [performance/BENCHMARKS.md](./performance/BENCHMARKS.md) — recorded benchmark results with hardware/commit/percentiles.
- [performance/METHODOLOGY.md](./performance/METHODOLOGY.md) — how benchmarks must be run and reported.

## Guides

- [guides/QUICKSTART.md](./guides/QUICKSTART.md) — full product guide: CLI, credentials, S3 operations.
- [guides/QUICKSTART_BETA.md](./guides/QUICKSTART_BETA.md) — the 5-minute quickstart (PT-BR / EN).
- [guides/KNOWN_LIMITATIONS.md](./guides/KNOWN_LIMITATIONS.md) — honest, one-page limitation list (beta edition).
- [guides/BETA_LAUNCH_KIT.md](./guides/BETA_LAUNCH_KIT.md) — beta tester playbook.
- [guides/STUDIO.md](./guides/STUDIO.md) — Studio user guide.
- [guides/TEST_SUITE.md](./guides/TEST_SUITE.md) — the one-command validation suite and its report.
- [guides/MIGRATING_FROM_SQLITE.md](./guides/MIGRATING_FROM_SQLITE.md) — moving from SQLite to OtsarDB.
- [guides/MIGRATING_FROM_POSTGRESQL.md](./guides/MIGRATING_FROM_POSTGRESQL.md) — honest PostgreSQL migration assessment.
