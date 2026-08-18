# OtsarDB — S3 Provider Compatibility Reference

> Public reference. Evidence discipline (the project's evidence rules)
> applies: every status
> below is classified `OBSERVED` (executed and recorded, with date/artifact),
> `UNTESTED` (not yet observed), or `UNRESOLVED` (executed with an ambiguous
> outcome). The consolidated matrix is maintained in this file; the
> capability model is defined in `docs/S3_PROVIDERS.md`.

## 1. Required provider properties

OtsarDB is capability-based, never brand-based: a provider is classified by
what the runtime probe observes, not by its name (`OBSERVED`,
`docs/S3_PROVIDERS.md`, `src/storage/capability_probe.c`).

**S3 Core tier** — required for the baseline storage path:

- authenticated GET / HEAD / PUT object operations;
- whole-object publication;
- usable read-after-write behavior for OtsarDB metadata objects;
- ETag or equivalent object-identity metadata;
- Range GET for the optimized/lazy read path;
- multipart completion behavior (probed before large-object optimization is
  enabled).

**S3 CAS tier** — optional capability that additionally enables direct
optimistic writer fencing:

- conditional create (`If-None-Match: *` → **412** on existing object);
- conditional update (`If-Match: <etag>` → **412** on wrong ETag).

The probe records: PUT + immediate GET, ETag availability, Range GET,
`If-None-Match: *` behavior, `If-Match` behavior, stale-condition error
behavior, and multipart completion (`OBSERVED`, `docs/S3_PROVIDERS.md`).

## 2. What happens when a property is missing (fail-closed)

| Missing property | Consequence | Evidence |
|---|---|---|
| S3 Core (auth, read-after-write, ETag) | authoritative writes are **not enabled** for that endpoint | `docs/S3_PROVIDERS.md` "If S3 Core fails..." |
| S3 CAS | write-capable native open **fails closed** unless `OTSARDB_S3_SINGLE_WRITER=1` is set — an explicit deployment assertion (one externally serialized writer), **not a lock** | `OBSERVED`, `docs/S3_PROVIDERS.md`; Wasabi/Magalu observed 2026-08-10 |
| Range GET | part of S3 Core; the lazy/optimized read path depends on it (cold reopen falls back to full recovery otherwise) | `OBSERVED` (probe spec), `docs/S3_PROVIDERS.md` |
| Multipart completion | large-object multipart optimization is not enabled until the probe passes | `OBSERVED` (probe spec), `docs/S3_PROVIDERS.md` |

The database object layout is identical in both tiers; only the write strategy
changes (CAS fencing vs externally serialized single writer). A provider that
returns 412 for conditional writes is CAS-eligible automatically; OtsarDB never
infers capabilities from the brand name (`OBSERVED`).

## 3. Provider matrix (real statuses)

Statuses as of the 2026-08-13 audit. Validation 7/7 = capability probe (Core,
CAS), native write, zero-local-state reopen, durability guards,
auto-checkpoint + recovery, s3_bench. Concurrency 5/5 = two-writer CAS race,
zero data loss, zero-cache reopen consistent.

| Provider | Endpoint | S3 Core | S3 CAS | Write mode | Validation | Concurrency | Multipart | Zero-local recovery | Status |
|---|---|---|---|---|---|---|---|---|---|
| MinIO (local) | `http://127.0.0.1:9000` | yes | yes | CAS multi-writer | 7/7 | 5/5 | probed (7/7 probe) | yes | **OBSERVED** 2026-08-10/13 |
| MinIO (BR datacenter) | `teste-minio.xjhjpb.easypanel.host` | yes | yes | CAS multi-writer | 7/7 | 5/5 | probed (7/7 probe) | yes | **OBSERVED** 2026-08-10 |
| Cloudflare R2 (ENAM) | `cf90d4227b9a1df5ce0515f841d3077e.r2.cloudflarestorage.com` | yes | yes | CAS multi-writer | 7/7 | 5/5 | probed (7/7 probe) | yes | **OBSERVED** 2026-08-10/12 (first real paid/global CAS provider; delta WAN bench recorded) |
| Wasabi (us-east-1) | `s3.us-east-1.wasabisys.com` | yes | **no** | single-writer (`OTSARDB_S3_SINGLE_WRITER=1`) | 7/7 (single-writer) | n/a | probed (7/7 probe) | yes | **OBSERVED** 2026-08-10/13 (conditional headers ignored: 200 overwrite) |
| Magalu Cloud (br-se1) | `br-se1.magaluobjects.com` | yes | **no** | single-writer (`OTSARDB_S3_SINGLE_WRITER=1`) | 7/7 functional; **full matrix 4/5** | n/a | probed (functional run) | yes (functional) | **OBSERVED / UNRESOLVED** (conditional headers → 501; remote benchmark cleanup failed twice) |
| AWS S3 | n/a (no account) | — | — | — | — | — | — | — | **UNTESTED** (no account available; R2 and MinIO share AWS's conditional-write semantics — that is a statement of protocol semantics, NOT a validation claim) |
| Backblaze B2 | n/a (no account) | — | — | — | — | — | — | — | **UNTESTED** (no account available) |

Notes on the conditional-write behavior observed with signed requests
(`OBSERVED` 2026-08-10):

- MinIO, Cloudflare R2: `If-None-Match: *` on existing object → **412**;
  wrong `If-Match` → **412**. CAS enforced — multi-writer safe.
- Wasabi: both cases return **200** (silent overwrite). CAS absent —
  single-writer mode, with unchanged durability in that mode.
- Magalu Cloud: both cases return **501 Not Implemented**. CAS absent —
  single-writer mode.

The full five-provider functional run passed on 2026-08-11
(`validate-all-providers.ps1`); the latest full matrix
(2026-08-13) is **4/5** because Magalu's remote performance/cleanup step failed
twice — a validation-result, not a provider capability change
(`OBSERVED`, `docs/S3_PROVIDERS.md`).

## 4. Multi-writer eligibility

- CAS-capable endpoints (MinIO local/BR, Cloudflare R2): direct multi-writer
  with optimistic HEAD fencing; two-writer races recorded 5/5 on all three.
- Core-only endpoints (Wasabi, Magalu): multi-writer is **not available**;
  the deployment must serialize the writer itself and assert it explicitly.
- AWS S3 and Backblaze B2: multi-writer eligibility **UNTESTED** (never
  probed).

## 5. How to verify a provider yourself

Probe any endpoint (S3 build required):

```bash
export OTSARDB_S3_ENDPOINT='https://s3.example.com'
export OTSARDB_S3_REGION='region-1'
export OTSARDB_S3_BUCKET='otsardb-tests'
export OTSARDB_S3_ACCESS_KEY='...'
export OTSARDB_S3_SECRET_KEY='...'
export OTSARDB_S3_PREFIX='compatibility-test'

./build-s3/otsardb-s3-probe        # reports: S3 Core yes/no, S3 CAS yes/no, write strategy CAS or SINGLE_WRITER
```

For the Core-only case, a write-capable open requires
`OTSARDB_S3_SINGLE_WRITER=1` and the deployment must actually guarantee the
single writer. Full battery (validated providers):
`powershell -ExecutionPolicy Bypass -File scripts/validate-all-providers.ps1`
(credentials live in git-ignored `scripts/providers.json`; results in
git-ignored dated artifacts, never secrets).
