# OtsarDB S3 Provider Compatibility

OtsarDB targets the S3 protocol, not a specific vendor.

There is exactly one object-storage backend conceptually: `otsardb-s3`. AWS S3, MinIO, EVEO, Magalu Cloud, Wasabi and other S3-compatible services are configured through endpoint, region and credentials. Provider-specific code must not leak into the OtsarDB SQL/storage core.

## Capability model

S3 compatibility is not binary. Different providers implement different subsets or semantics. OtsarDB therefore enables behavior from tested capabilities instead of a provider-name allowlist.

### S3 Core

Required for the baseline OtsarDB S3 storage path:

- authenticated GET / HEAD / PUT object operations;
- whole-object publication;
- usable read-after-write behavior for OtsarDB metadata;
- ETag or equivalent object identity metadata;
- Range GET for the optimized/lazy read path.

S3 Core supports:

- authoritative persistence in object storage;
- disposable local RAM/NVMe cache;
- many readers;
- one OtsarDB-authoritative writer, or many clients routed through one serialized writer/leader;
- immutable segments/manifests and normal HEAD publication.

### S3 CAS

Optional capability tier:

- conditional create (`If-None-Match: *`);
- conditional update (`If-Match: <etag>`).

S3 CAS additionally supports OtsarDB's direct optimistic commit fencing against the object store. A stale writer cannot replace a newer HEAD.

A provider that lacks S3 CAS is not unsupported. It uses the same OtsarDB object format, but direct independent writers cannot be made safe merely by calling ordinary PUT. The writer must be serialized by OtsarDB deployment/coordination.

## Safe native-open behavior

The native `otsardb s3://bucket/database` path follows a fail-closed rule:

- **S3 CAS endpoint:** OtsarDB can select CAS publication automatically.
- **S3 Core without CAS:** OtsarDB refuses a write-capable native open unless `OTSARDB_S3_SINGLE_WRITER=1` is set.

`OTSARDB_S3_SINGLE_WRITER=1` is not a locking mechanism. It is an explicit deployment assertion that something outside the object-store primitive already guarantees only one authoritative OtsarDB writer for that database (for example, one process/leader or a later OtsarDB coordination layer).

This distinction is required for correctness. OtsarDB must not turn a provider limitation into silent lost-update behavior.

```text
S3 CAS
  independent writer attempts
          |
          v
   HEAD If-Match / CAS
          |
  stale writer fails safely

S3 Core without CAS
  one externally serialized writer
          |
          v
   ordinary HEAD publication
```

## Coordination is separate from storage

```text
                         OtsarDB
                           |
                    Storage protocol
                           |
                         S3 API
                           |
        +------------------+------------------+
        |                                     |
     S3 Core                               S3 CAS
        |                                     |
 serialized writer                       direct CAS
 / OtsarDB leader                         optimistic fencing
        |                                     |
        +------------------+------------------+
                           |
                  same OtsarDB format
```

The database object layout does not change between tiers.

## Runtime capability probe

OtsarDB must test an endpoint before enabling write features. The probe records at least:

- PUT + immediate GET;
- ETag availability;
- Range GET;
- `If-None-Match: *` behavior;
- `If-Match: <etag>` behavior;
- error behavior for stale conditions;
- multipart completion behavior before large-object optimization is enabled.

Do not infer capabilities solely from a brand name or from the phrase "S3-compatible".

The backend-neutral probe lives in `src/storage/capability_probe.c`. Native database opens isolate probe keys by database/process and serialize same-process probes so concurrent opens do not accidentally make a CAS-capable endpoint look non-CAS.

## Generic network adapter

The network implementation is named `s3_object_store`; there are no `aws_store`, `minio_store`, `magalu_store`, or `wasabi_store` variants.

Current configuration surface:

```text
endpoint
region
bucket
access_key
secret_key
session_token (optional)
prefix (optional)
https
```

The first implementation uses the Apache-2.0 MinIO C++ S3 client internally for HTTP/SigV4/endpoint compatibility. That is an implementation dependency only. OtsarDB's public storage contract remains the S3 protocol plus the runtime capability report.

Static access/secret credentials are the first authentication path. AWS IAM roles, container/workload identities, environment/provider chains and other credential sources can be added later behind the same `otsardb-s3` API.

## Build the S3 adapter

The normal OtsarDB build does not require an S3 SDK. S3 is opt-in at build time while the native path is being hardened:

```bash
cmake -S . -B build \
  -DOTSARDB_ENABLE_S3=ON \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build
```

The MinIO C++ package is expected to be available to CMake as `miniocpp::miniocpp`.

## Probe any S3-compatible endpoint

When S3 support is enabled, OtsarDB builds `otsardb-s3-probe`.

Example environment:

```bash
export OTSARDB_S3_ENDPOINT='https://s3.example.com'
export OTSARDB_S3_REGION='region-1'
export OTSARDB_S3_BUCKET='otsardb-tests'
export OTSARDB_S3_ACCESS_KEY='...'
export OTSARDB_S3_SECRET_KEY='...'
export OTSARDB_S3_PREFIX='compatibility-test'

./build/otsardb-s3-probe
```

Expected classification is one of:

```text
S3 Core: yes
S3 CAS: yes
OtsarDB write strategy: CAS
```

or:

```text
S3 Core: yes
S3 CAS: no
OtsarDB write strategy: SINGLE_WRITER
```

For the second case, a write-capable native database open additionally requires:

```bash
export OTSARDB_S3_SINGLE_WRITER=1
```

and the deployment must actually guarantee that assertion.

If S3 Core fails, OtsarDB must not enable authoritative writes for that endpoint.

## Provider examples

Compatibility targets, not separate adapters:

- AWS S3
- MinIO
- EVEO Object Storage S3
- Magalu Cloud Object Storage
- Wasabi
- Cloudflare R2
- Backblaze B2 S3 API
- DigitalOcean Spaces
- any other endpoint that passes the OtsarDB S3 Core probe

A provider may pass S3 Core while failing S3 CAS. OtsarDB must report that clearly and select the safe coordination requirement instead of silently weakening correctness.

## Observed on real providers

Empirical probe verdicts and validation runs (2026-08-10):

| Provider | Probe S3 Core | Probe S3 CAS | Validated mode |
|---|---|---|---|
| MinIO (public + local) | yes | yes | CAS multi-writer: concurrency 5/5, probe 3/3 |
| Cloudflare R2 (ENAM) | yes | yes | CAS multi-writer: validation 7/7, concurrency 5/5 on live cloud |
| Wasabi (us-east-1) | yes | no | single-writer: validation 7/7 (write, zero-local reopen, guards, checkpoint, bench) |
| Magalu Cloud (br-se1) | yes | no | single-writer: validation 7/7 (write, zero-local reopen, guards, checkpoint, bench) |

### Current audit boundary (2026-08-13)

The latest full provider run completed the functional path on **4/5 configured
providers**. MinIO local, MinIO BR, Cloudflare R2 and Wasabi completed their
recorded functional/performance paths; Magalu completed the functional checks,
but the remote performance/cleanup portion failed twice and is therefore not
counted as a full-provider PASS. This is a validation result, not a provider
capability change: Magalu remains S3 Core without S3 CAS and write-capable
opens still require the explicit single-writer assertion.

The corrected professional high-concurrency stress passed its local CAS
envelope: 100/100 writer sessions and 100 fresh rows with a lease-aware
60-attempt retry budget. An 8-attempt diagnostic run completed 97/100 sessions
without losing acknowledged rows. This is a retry/liveness boundary, not a
proven universal WAN or provider result; production claims must name the
provider, schedule and retry budget.

Cloudflare R2 is the first real global provider validated with full S3
CAS: `If-None-Match: *` and wrong `If-Match` on
`PutObject` both return 412 as required. The full acceptance plan passed
7/7 and the two-writer concurrency test passed 5/5 on the live endpoint
(one writer wins the CAS race each iteration, loser opens in candidate
mode, zero data loss) — multi-writer with CAS fencing proven on a real
paid/global provider, not just self-hosted MinIO. Signing region is
`auto`.

Wasabi ignores conditional headers on object writes: `PutObject` with
`If-None-Match: *` on an existing object returns 200 (overwrite) instead
of 412, `PutObject` with a wrong `If-Match` returns 200, and `DeleteObject`
with a wrong `If-Match` returns 204. The OtsarDB capability probe therefore
correctly reports S3 CAS absent, and native write-capable opens on Wasabi
require `OTSARDB_S3_SINGLE_WRITER=1` (explicit serialization assertion).
The probe is capability-based, not a provider allowlist: if Wasabi later
enforces conditional writes, OtsarDB picks CAS up automatically.

Magalu Cloud (`br-se1.magaluobjects.com`) rejects conditional headers
outright: `PutObject` with `If-None-Match: *` or a wrong `If-Match`
returns **501 Not Implemented** (not 412). Same consequence as Wasabi:
S3 Core passes, S3 CAS is absent, and write-capable native opens require
`OTSARDB_S3_SINGLE_WRITER=1`. Both were validated end-to-end in that mode.

## Client library rule

OtsarDB may use an existing S3 client library internally for signing, HTTP, retries and endpoint compatibility. That library is an implementation dependency, not the OtsarDB storage contract.

The resulting OtsarDB module remains named `s3_object_store`, not `minio_store`, regardless of which client library is used internally.
