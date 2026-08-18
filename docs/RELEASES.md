# OtsarDB Releases

Standard release layout, versioning policy and publishing procedure for
OtsarDB. The packaging scripts implement it on Windows, Linux and macOS.

Every claim below carries an evidence classification. Windows packaging
is OBSERVED; Linux/macOS packaging is UNTESTED.

## Standard release layout

Every release ships one package per OS/arch with the same shape, so
download and smoke-test instructions are identical across platforms.

### Asset naming

```text
otsardb-<semver>-<os>-<arch>.<ext>
```

| Platform | Asset example | Format |
|---|---|---|
| Windows x64 | `otsardb-0.1.0-windows-x64.zip` | ZIP |
| Linux x64 | `otsardb-0.1.0-linux-x64.tar.gz` | tar.gz |
| Linux arm64 | `otsardb-0.1.0-linux-arm64.tar.gz` | tar.gz |
| macOS arm64 (Apple Silicon) | `otsardb-0.1.0-macos-arm64.tar.gz` | tar.gz |
| macOS x64 (Intel) | `otsardb-0.1.0-macos-x64.tar.gz` | tar.gz |

`<semver>` is the release version without a leading `v` (e.g. `0.1.0` for
tag `v0.1.0`). `<os>` is `windows`, `linux` or `macos` (lowercase). `<arch>`
is `x64`, `arm64` or `arm32`.

### What every release ships

1. **The binary(ies) + required runtime DLLs (Windows only).** The Windows
   zip is fully standalone: `otsardb.exe` plus the vcpkg DLLs
   (`curlpp.dll`, `libcurl.dll`, `pugixml.dll`, `libcrypto-3-x64.dll`,
   `z.dll`) and the MSVC runtime (`MSVCP140.dll`, `VCRUNTIME140.dll`,
   `VCRUNTIME140_1.dll`). No install required. Linux/macOS ship the binary;
   required shared libraries are recorded in `MANIFEST.txt` (`libs` field)
   and the distro equivalents must be installed on the target machine.
2. **`SHA256SUMS.txt`** — one per release, next to the assets. Lists the
   SHA-256 of the packaged binary (as `otsardb.exe`/`otsardb`) and of every
   asset (the zip/tar.gz), coreutils format `<hash>  <filename>`, plus the
   release version, generated UTC timestamp and commit SHA.
3. **`MANIFEST.txt` inside each package** — commit SHA, binary SHA-256,
   build config, date UTC and the version string from `otsardb --version`
   (and `release_version`). The recorded binary hash must equal the shipped
   binary's hash.

Also inside each package (proven behavior carried over from the beta
package): `README.txt` (generated),
`QUICKSTART_BETA.md`, `KNOWN_LIMITATIONS.md`, `STUDIO.md` and `LICENSE`.

## Versioning policy

SemVer (`major.minor.patch`, optional `-prerelease`/`+build`). A release
tag is `v<semver>` (e.g. `v0.1.0`).

**Where the version string lives today** (src, OBSERVED by source
inspection 2026-08-11):

- `src/core/otsardb.c:9` — `#define OTSARDB_VERSION "0.1.0-dev"`; the engine
  version is returned by `otsardb_version()` (`src/core/otsardb.c:23-24`) and
  printed by the CLI at `src/cli/main.c:260`, `:275` and `:371` as
  `otsardb <version> (engine base 3.53.4)`.

**How to bump:** edit the `OTSARDB_VERSION` macro in `src/core/otsardb.c`,
rebuild, and verify with `otsardb --version`. A release build should report
the tag version without a `-dev` suffix (e.g. tag `v0.1.0` → binary
`0.1.0`); the packaging scripts take `-Version`/`VERSION` explicitly and
default to the binary's version token.

## Building and packaging

### Windows (OBSERVED)

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\package-windows.ps1 -Version 0.1.0
```

Produces `otsardb-0.1.0-windows-x64.zip` + `SHA256SUMS.txt` in
the artifacts directory (`artifacts/` by default; `-OutputDir`,
`-BuildDir`, `-RepoDir` override). Idempotent.

### Linux (UNTESTED — exact commands)

```bash
sudo apt-get install -y cmake build-essential make curl unzip python3 zip tar
git clone https://github.com/microsoft/vcpkg "$HOME/vcpkg"
"$HOME/vcpkg/bootstrap-vcpkg.sh" -disableMetrics
"$HOME/vcpkg/vcpkg" install --triplet x64-linux        # arm64: arm64-linux
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DOTSARDB_ENABLE_S3=ON \
  -DCMAKE_TOOLCHAIN_FILE="$HOME/vcpkg/scripts/buildsystems/vcpkg.cmake"
cmake --build build --parallel
./scripts/package-linux.sh        # VERSION=0.1.0 OUTPUT_DIR=... to override
```

### macOS (UNTESTED — exact commands)

```bash
brew install openssl@3 cmake
git clone https://github.com/microsoft/vcpkg "$HOME/vcpkg"
"$HOME/vcpkg/bootstrap-vcpkg.sh" -disableMetrics
"$HOME/vcpkg/vcpkg" install --triplet arm64-osx        # Intel: x64-osx
OPENSSL_ROOT=""
[ -d "/opt/homebrew/opt/openssl@3" ] && OPENSSL_ROOT="/opt/homebrew/opt/openssl@3"
[ -d "/usr/local/opt/openssl@3" ]  && OPENSSL_ROOT="/usr/local/opt/openssl@3"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DOTSARDB_ENABLE_S3=ON \
  -DOPENSSL_ROOT_DIR="$OPENSSL_ROOT" \
  -DCMAKE_TOOLCHAIN_FILE="$HOME/vcpkg/scripts/buildsystems/vcpkg.cmake"
cmake --build build --parallel
./scripts/package-macos.sh       # ARCH auto-detected; VERSION=0.1.0 to set
```

Both bash scripts produce `otsardb-<version>-<os>-<arch>.tar.gz` +
`SHA256SUMS.txt` in the artifacts directory (`artifacts/` next to the repo
by default).
The macOS binary is unsigned; document the Gatekeeper
`xattr -dr com.apple.quarantine otsardb` step in the release notes until
notarization exists.

## GitHub Releases publishing procedure

Before publishing or sharing a release with external testers, verify that
`otsardb --version` exactly matches the tag/package version. A tag such as
`v0.1.0` must not contain a binary reporting `0.1.0-dev`; either bump the
source version deliberately or use an explicitly labelled prerelease tag.
The release workflow and `scripts/package-windows.ps1` enforce this for
semver release versions.

1. Push the tag: `git tag v<ver>` / `git push origin v<ver>` — this
   triggers `.github/workflows/release.yml` (matrix over
   Linux/macOS/Windows), which uploads the standard-named assets and
   `SHA256SUMS.txt`.
2. Publish from the release assets (download the artifacts or the locally
   built ones):

```bash
gh release create v0.1.0 \
  otsardb-0.1.0-windows-x64.zip \
  otsardb-0.1.0-linux-x64.tar.gz \
  otsardb-0.1.0-macos-arm64.tar.gz \
  otsardb-0.1.0-macos-x64.tar.gz \
  SHA256SUMS.txt \
  --generate-notes
```

3. **Manual path (gh unauthenticated/unavailable):** upload the same files
   at https://github.com/<owner>/otsardb/releases/new — select tag `v<ver>`,
   attach the assets, and paste the release notes (template below) into
   the "Release notes" box. The web UI shows checksums; verify them against
   `SHA256SUMS.txt` after upload.

### Release notes template

```markdown
## OtsarDB <semver>

**Headline:** one sentence describing the user-visible change of this
release (e.g. "First standalone multi-OS release: S3-native OtsarDB binary
for Windows, Linux and macOS").

### Highlights
- <behavior/feature 1 with the change that introduced it>
- <behavior/feature 2 ...>

### Providers matrix state
| Provider | S3 tier | Validated | Notes |
|---|---|---|---|
| MinIO (local + BR) | CAS | 7/7 + concurrency 5/5 | multi-writer |
| Cloudflare R2 | CAS | 7/7 + concurrency 5/5 | multi-writer |
| Wasabi | Core only | 7/7 (single-writer) | |
| Magalu Cloud | Core only | 7/7 (single-writer) | |
| minio-br | CAS | 7/7 + concurrency 5/5 | |

(Current matrix lives in docs/S3_PROVIDERS.md — copy, never invent.)

### Known limitations
See https://github.com/<owner>/otsardb/blob/main/docs/guides/KNOWN_LIMITATIONS.md
(included inside every package).

### Checksums
See SHA256SUMS.txt attached to this release (also inside each package:
MANIFEST.txt records commit SHA and binary hash).
```

### Change-log pointer

Every release notes section names the change(s) that introduced the
behavior (e.g. "lazy hydration"). See `CHANGELOG.md` for the full,
append-only record of what changed and why.

## Release checklist

### Pre-release

- [ ] Harness: `scripts/validate-all-providers.ps1` — 5 providers green
  (minio-local, minio-br, R2, Wasabi, Magalu): s3-validation 7/7 and CAS
  concurrency 5/5 for CAS providers (R2 SKIPPED means SKIPPED, never PASS).
- [ ] `scripts/soak.ps1` PASSED and `scripts/ci-local.ps1` ALL STEPS PASSED.
- [ ] Version verified: `otsardb --version` reports the release version;
  `src/core/otsardb.c` `OTSARDB_VERSION` bumped if needed.
- [ ] Built on all three OS (Release + `OTSARDB_ENABLE_S3=ON`); Windows
  package built with `scripts/package-windows.ps1 -Version <ver>`; Linux
  and macOS with the bash scripts.
- [ ] `SHA256SUMS.txt` regenerated and matches the uploaded assets; each
  package's `MANIFEST.txt` binary hash equals the shipped binary's hash.

### Post-release

- [ ] Smoke test from the **published** zip on a clean machine (not the
  build machine): extract, `otsardb --version`, `otsardb :memory: "SELECT 6*7;"`
  → 42, and one minisession against local MinIO with a fresh
  `OTSARDB_CACHE_DIR` (create + insert + select, zero-cache reopen returns
  the data from object storage alone).
- [ ] Confirm the release tag `v<ver>`, assets and checksums are visible on
  GitHub; record the release URL in the changelog.
