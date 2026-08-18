# Contributing to OtsarDB

OtsarDB is an independent database engine whose authoritative state lives on
S3-compatible object storage. This file is the public contributor guide;
read it before making changes. Every change must carry evidence (see
§5) and be reproducible.

## 1. Build for local development

Prerequisites (Windows/MSVC, the reference development platform):

- Visual Studio 2022 Build Tools (MSVC x64, CMake, Ninja — enable the
  "Desktop development with C++" workload);
- vcpkg (`C:\vcpkg`) with the `x64-windows` triplet packages installed;
- the SQLite 3.53.4 upstream pin (`bash scripts/fetch-sqlite.sh`, or the
  vendored copy under `vendor/sqlite/` if already fetched).

Core engine build + tests (Windows PowerShell or cmd, from a Visual Studio
developer prompt — `vcvars64.bat` must be loaded):

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Native S3 build (the product's main path) adds `-DOTSARDB_ENABLE_S3=ON` and
uses a separate build directory so core and S3 test suites stay
independent:

```bat
cmake -S . -B build-s3 -G Ninja -DCMAKE_BUILD_TYPE=Release -DOTSARDB_ENABLE_S3=ON ^
  -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build-s3 --parallel
ctest --test-dir build-s3 --output-on-failure
```

Release builds keep test assertions active: CMake undefines `NDEBUG` for
test targets — never regress this.

Linux (WSL2 / native, from `scripts/package-linux.sh`):

```bash
sudo apt-get install -y cmake build-essential make curl unzip python3 zip tar
git clone https://github.com/microsoft/vcpkg "$HOME/vcpkg"
"$HOME/vcpkg/bootstrap-vcpkg.sh" -disableMetrics
"$HOME/vcpkg/vcpkg" install --triplet x64-linux
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DOTSARDB_ENABLE_S3=ON \
  -DCMAKE_TOOLCHAIN_FILE="$HOME/vcpkg/scripts/buildsystems/vcpkg.cmake"
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Release packaging: `scripts/package-windows.ps1` (Windows),
`scripts/package-linux.sh`, `scripts/package-macos.sh` (standard release
layout), plus `scripts/package-beta.ps1` for the beta zip.

## 2. Test gates — what a change must pass

The acceptance gate is **observed behavior on real endpoints and local
gates**, not a CI badge. GitHub Actions is OPTIONAL manual validation
(`workflow_dispatch` only) — a missing green badge is never a
validation gap.

- **Project-migration validator** (mandatory for every change set):

  ```bash
  python scripts/check-project-migrations.py --validate-all
  ```

- **CTest suites** — `ctest --test-dir build` (core) and
  `ctest --test-dir build-s3` (S3). Store-gated tests skip cleanly
  without a live store.
- **`scripts/ci-local.ps1`** — the 11-step local CI parity runner:
  migrations, vendor, core + S3 build/test, live MinIO probe, e2e,
  native `s3://` zero-local reopen, durability guards, extended
  concurrency + soak. Requires a local MinIO on `127.0.0.1:9000`.
- **`scripts/soak-test.ps1`** — sustained-write soak (4100 rows, phases
  1-3: sequential sessions, zero-local recovery, GC verification).
- **`scripts/s3-concurrency-test.ps1`** — two-writer CAS race, zero data
  loss, zero-cache reopen consistent (must pass 5/5 on CAS providers).
- **`scripts/validate-all-providers.ps1`** — the full battery against
  every provider in the git-ignored `scripts/providers.json` (one
  command). Results land in git-ignored `scripts/validation-results/`.
- **`scripts/s3-validation.ps1`** — single-provider 7-step battery.

## 3. Engineering rules

- Keep the `otsardb` executable usable throughout engine work.
- Storage changes must go through an OtsarDB-owned interface/VFS.
- Do not implement S3 as periodic backup and call it native storage.
- **Research existing approaches before implementing:** when SQLite does not
  provide a required capability, search for established SQLite forks,
  extensions, VFS or replication approaches that already solve it before
  designing from zero. Evaluate the candidate mechanisms and record which
  you considered and why, with their license status.
- Do not import code from another project until its license and
  attribution requirements are verified (SQLite is public domain; LZ4 is
  BSD-2; miniocpp is Apache-2.0 — attribution lives in `LICENSE`).
- Add fault/recovery tests for persistence changes.
- DBaaS control-plane concerns do not belong in this repository.

## 4. Change records (append-only)

Every meaningful repository change requires a new append-only project change
record (a four-digit, zero-padded sequential ID following the established
convention in this repository). A change record is the engineering equivalent
of a database migration: it answers what changed, why, how it was validated,
and what comes next. See the repository's established record format and
template for details.

- Change records are **append-only**: never edit, rename, renumber or delete
  a landed record. Corrections and reversals get a new record that
  references the old one.
- Documentation-only typo fixes may be grouped into the change record that
  motivated them; anything that changes project meaning or process needs
  its own record.
- A change set is not complete until the record documents real validation
  results — never convert an intended test into a claimed passing test.
- The current engineering state and next action are tracked in the
  project's mutable handoff document.

## 5. Evidence rules

Every claim in code, docs, change records, tests and summaries must carry one
classification: `OBSERVED` (executed and recorded: date, binary,
endpoint), `PROJECTED`, `UNTESTED`, `INCONCLUSIVE`,
`NOT_APPLICABLE`. Never write "validated", "production-ready",
"RPO zero", "high availability" or "multi-region" without the test and
artifact that support the claim. A SKIPPED test is never a PASS; results
must be reproducible (commit SHA, binary hash, OS, compiler, config,
provider, region, harness version, UTC timestamp, scenario, raw result);
never overwrite concurrent work or stage unrelated files; credentials
never enter code, change records, logs, artifacts or docs; documentation
must stay synchronized with observed behavior; CI is optional, not a
gate. The recorded performance evidence lives in
`docs/performance/BENCHMARKS.md`; the honest limitation list is
`docs/guides/KNOWN_LIMITATIONS.md`.

## 6. Credentials hygiene

- Provider secrets live only in the git-ignored `scripts/providers.json`
  (and per-run `scripts/*.run.json`, `scripts/validation-results/`).
  Never commit them, never paste them into issues or change records.
- Tests mask keys and clean child environment variables.
- Public examples use local-development defaults (e.g.
  `MINIO_ROOT_USER`/`MINIO_ROOT_PASSWORD` against `127.0.0.1`) or placeholders
  only.

## 7. License

OtsarDB is released under the **Unlicense** (public domain; see `LICENSE`).
Third-party dependencies: SQLite (public domain), LZ4 (BSD-2), miniocpp
(Apache-2.0).

## 8. DCO / CLA policy

**Recommended: Developer Certificate of Origin (DCO)** — each commit is
signed off with `Signed-off-by: <name> <email>` (as in
`git commit -s`), declaring you have the right to submit the work under
the project license. No separate Contributor License Agreement is
currently planned. This policy is a **owner decision**: it will be
confirmed or revised by the OtsarDB maintainers before the first external
pull request is accepted.
