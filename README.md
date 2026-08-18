# OtsarDB

**Store. Preserve. Endure.**

**Your database lives in object storage. Compute is disposable.**

OtsarDB is a SQLite-derived SQL engine whose authoritative durable state lives
in S3-compatible object storage. There is no server to install: the local
process is disposable runtime and cache, and any other process can recover the
database from remote state alone.

[![License: Unlicense](https://img.shields.io/badge/license-Unlicense-blue)](LICENSE)
[![Version](https://img.shields.io/badge/version-0.1.1-blue)](src/core/otsardb.c)

**Website:** [otsardb.org](https://otsardb.org)

## Why

Traditional databases make a server or a local disk the system of record.
OtsarDB inverts that:

- **No server to run.** Data is published as immutable objects; the storage
  account is the database.
- **Compute is disposable.** Any process — a laptop, a container, an ephemeral
  worker — opens the same database and recovers from object storage with an
  empty cache.
- **Provider-aware.** S3 Core and S3 CAS capabilities are probed, not inferred
  from the provider name, so the engine adapts to what the storage actually
  supports.

OtsarDB is not "SQLite making backups to S3": the object store is the durable
source of truth, and the local files are cache.

## Quick start

```powershell
$env:OTSARDB_S3_ENDPOINT = "http://127.0.0.1:9000"
$env:OTSARDB_S3_BUCKET    = "otsardb-ci"
$env:OTSARDB_S3_ACCESS_KEY = "<key>"
$env:OTSARDB_S3_SECRET_KEY = "<secret>"
$env:OTSARDB_S3_REGION    = "us-east-1"

otsardb "s3://otsardb-ci/example" "CREATE TABLE notes(id INTEGER PRIMARY KEY, body TEXT);"
otsardb "s3://otsardb-ci/example" "INSERT INTO notes(body) VALUES ('hello');"
otsardb "s3://otsardb-ci/example" "SELECT * FROM notes;"
```

Zero-local recovery: delete the cache directory and the next process rebuilds
the database from remote state. See
[docs/guides/QUICKSTART_BETA.md](docs/guides/QUICKSTART_BETA.md) for the full
walkthrough.

## Downloads

Official binaries are published on the [GitHub Releases](https://github.com/notrevetecnologia/otsardb/releases)
page. Current release `0.1.1` ships packages for:

- **Linux** (`x64`, tar.gz)
- **Windows** (`x64`, standalone zip with runtime libraries)
- **WebAssembly** (`wasm32`, emscripten artifact for WASM-compatible hosts)

Each package includes the binary, `otsardb.h`, the license, and a SHA256SUMS
manifest. Downloads are listed automatically on the website from the GitHub
Releases API.

## Providers

Validated against MinIO (local and remote), Cloudflare R2, Wasabi and Magalu
Cloud. Providers that support conditional writes (`If-None-Match`,
`If-Match`) get the fenced multi-writer path; providers without it require an
explicit single-writer assertion (`OTSARDB_S3_SINGLE_WRITER=1`). Full matrix in
[docs/S3_PROVIDERS.md](docs/S3_PROVIDERS.md).

## Studio

`otsardb studio` serves a local administration UI — connection profiles, SQL
editor, schema explorer, result grid, monitor and admin actions. Loopback-only,
no TLS/authentication: do not expose it beyond the local machine.
[docs/guides/STUDIO.md](docs/guides/STUDIO.md).

## Build from source

```powershell
cmake -S . -B build-s3 -G Ninja -DOTSARDB_ENABLE_S3=ON
cmake --build build-s3 --config Release
ctest --test-dir build-s3 -C Release --output-on-failure
```

See [CONTRIBUTING.md](CONTRIBUTING.md) for the development flow.

## Documentation

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — engine boundaries
- [docs/S3_STORAGE.md](docs/S3_STORAGE.md) — object layout and manifest v4
- [docs/AVAILABILITY_MODEL.md](docs/AVAILABILITY_MODEL.md) — durability, recovery
  and orchestration levels
- [docs/performance/BENCHMARKS.md](docs/performance/BENCHMARKS.md) — recorded performance evidence
- [docs/guides/KNOWN_LIMITATIONS.md](docs/guides/KNOWN_LIMITATIONS.md) — limitations
- [docs/FAQ.md](docs/FAQ.md) — common questions and short answers

## Community

- Website: [otsardb.org](https://otsardb.org)
- WhatsApp (official group): [join](https://chat.whatsapp.com/Jgje8lDk6YtJiMXfqhqqY6)

## License

OtsarDB is released under the **Unlicense** (public domain). OtsarDB is
derived from SQLite, which is also in the public domain. Third-party
components keep their own licenses. See [LICENSE](LICENSE) and
[THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES).
