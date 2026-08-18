# OtsarDB Beta Launch Kit — Operator Playbook

> Owner-facing playbook for running the OtsarDB beta with real testers.
> Every claim in this document follows the evidence classification rules:
> `OBSERVED` / `PROJECTED` / `UNTESTED` / `INCONCLUSIVE` /
> `NOT_APPLICABLE`, with the artifact that supports it.
> The structured feedback form lives in
> `docs/beta/feedback-template.md`.

## 1. Purpose

OtsarDB's acceptance gate is **observed behavior on real endpoints**, and
the beta is the mechanism that produces that observation from real users:
durability, zero-local recovery, multi-provider behavior and multi-writer CAS
fencing get exercised by testers outside the development machine, and every
finding becomes an append-only change record.

This kit tells the operator how to run that loop: what is being tested, how a
tester is onboarded, which provider to use for which goal, how feedback is
triaged, what must be green before launch, and what "beta is over" means.

## 2. Beta scope statement

### 2.1 What the beta is testing

| # | Test area | Availability-model level | Current evidence | Classification |
|---|---|---|---|---|
| 1 | **Durability** — a successful `COMMIT` means the data is durably published to object storage | A (state durability) | WAL sync fails closed on remote durability failure; soak + fault-injection tests pass; 5-provider validation 7/7 | OBSERVED (2026-08-10, harness `validate-all-providers.ps1`, artifact `scripts/validation-results/20260810-providers.json`) |
| 2 | **Zero-local recovery** — a fresh machine with an empty cache reconstructs the database from S3 alone | B (instance recovery) | zero-local reopen validated 7/7 on MinIO local, MinIO BR, Cloudflare R2, Wasabi, Magalu | OBSERVED (2026-08-10, same artifact) |
| 3 | **Multi-provider** — the identical object format and CLI work on CAS and non-CAS endpoints | A/B (all providers) | 5-provider matrix: CAS on MinIO local, MinIO BR, R2; Core-only on Wasabi, Magalu | OBSERVED (2026-08-10, same artifact) |
| 4 | **Multi-writer CAS fencing** — on CAS-capable providers, concurrent writers are fenced by lease + CAS + generation, zero data loss | E (partial) | two-writer concurrency 5/5 on MinIO local, MinIO BR, R2 (`scripts/s3-concurrency-test.ps1`) | OBSERVED (2026-08-10, same artifact) |
| 5 | **The feedback loop itself** — tester findings become change records | process | this kit + template; first real-tester change record pending | UNTESTED |
| 6 | **Studio browser workflow** — open the local browser UI, connect, query and inspect the responsive layout | Studio v0.7 | internal desktop/mobile viewport checks pass; first external browser session pending | UNTESTED |

### 2.2 What the beta is NOT testing (explicitly out of scope)

- **Encryption at rest / in transit beyond TLS** — `OTSARDB_ENCRYPTION=0`
  currently; no encryption claim is made.
- **Multi-region / multi-S3 replication** — availability level F is DESIGN
  ONLY; two independent PUTs are not an atomic commit.
- **Native Android/iOS binaries and remote mobile administration** — the
  official Studio surface is browser mode, but OtsarDB does not ship a mobile
  binary and the loopback-only server must not be exposed to the network.
- **Orchestrator-managed HA** — level C (orchestrated availability) and
  level D at container level are UNTESTED by design; the beta does not test
  failover or RTO.
- **Page-level conflict merge** — M3 phase 2 (`SQLITE_BUSY_SNAPSHOT`,
  rebase/retry) is UNTESTED. Multi-writer means **fencing** (a stale writer
  cannot corrupt), not automatic merging of two writers' edits.
- **AWS S3 / Backblaze B2** — no accounts validated yet; optional, never a
  blocker.

Terminology discipline (from `docs/AVAILABILITY_MODEL.md` §3): durability ≠
failover ≠ multi-writer ≠ multi-region. The kit must never use "high
availability", "RPO zero" or "multi-region" for the beta; until the
AVAILABILITY_MODEL §4 acceptance criteria are all OBSERVED, the exact allowed
formulation is:

> "OtsarDB oferece recuperação de estado a partir do S3 e é projetado para
> alta disponibilidade orquestrada pelo cliente."

## 3. Tester onboarding flow (5 steps)

### Step 1 — Read the quickstart

Read `docs/guides/QUICKSTART.md` (install, credentials, S3 target syntax),
`docs/guides/STUDIO.md` and `docs/guides/KNOWN_LIMITATIONS.md`. If a
limitation breaks your session, report it — that is a valid finding.

### Step 2 — Provision a bucket

Any S3-compatible bucket works. Fastest path (5 minutes, no cloud account):

```powershell
# download minio.exe from https://dl.min.io/server/minio/release/windows-amd64/minio.exe
.\minio.exe server C:\minio-data --address :9000
# second terminal — create the bucket (mc.exe from https://dl.min.io/client/mc/release/windows-amd64/mc.exe)
mc alias set local http://127.0.0.1:9000 <access-key> <secret-key>
mc mb local/my-bucket
```

Or use any of the cloud providers in §4 — the steps below are identical; only
the endpoint/region environment changes.

### Step 3 — Run the 4-command session

The canonical beta session (verified against the real CLI, `OBSERVED`
2026-08-10):

```powershell
# pick your own subfolder so testers never collide
$env:OTSARDB_S3_ENDPOINT   = 'http://127.0.0.1:9000'          # your provider endpoint
$env:OTSARDB_S3_REGION     = 'us-east-1'                      # R2: 'auto'
$env:OTSARDB_S3_ACCESS_KEY = '...'                            # never paste these into feedback
$env:OTSARDB_S3_SECRET_KEY = '...'
$db = 's3://my-bucket/<yourname>/db'

# CMD 1 — write: create schema and insert one row (expect exit 0)
otsardb $db "CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT); INSERT INTO t(v) VALUES('hello-<yourname>');"

# CMD 2 — read back from the same cache (expect exit 0, output: hello-<yourname>)
otsardb $db "SELECT v FROM t;"

# CMD 3 — zero-local recovery: fresh cache dir, data must come from S3 only
$env:OTSARDB_CACHE_DIR = "$env:TEMP\my-app-fresh"
New-Item -ItemType Directory -Path $env:OTSARDB_CACHE_DIR -Force | Out-Null
otsardb $db "SELECT v FROM t;"          # expect exit 0, same row

# CMD 4 — cleanup: delete this tester's provider prefix (cache is disposable)
mc rm --recursive --force "local/my-bucket/<yourname>/"
Remove-Item -Recurse -Force $env:OTSARDB_CACHE_DIR
```

Expected behavior notes:

- Data rows go to **stdout**; `otsardb:` diagnostic lines (e.g. lease
  acquisition) go to **stderr**. Exit code 0 = success.
- The first write creates the database in S3 (capability probe, lease,
  manifest chain, HEAD publication).
- CMD 3 must work with the local cache directory **deleted** — if the data
  is not there, the run failed and is a blocker finding.
- Never paste credentials into feedback. Mask endpoints (§5 template).

### Step 4 — Reopen on a second machine (or empty cache)

The essence of zero-local recovery: run CMD 3 on a different machine with the
same credentials and the same `s3://` target. No second machine available?
An emptied `OTSARDB_CACHE_DIR` on the same machine exercises the identical code
path (the harness does exactly this per iteration). Either way, the SELECT
must return the same row written in CMD 1.

For CAS-capable providers (R2, MinIO), optionally run the **two-writer
session**: from two terminals at the same time, both run an INSERT into the
same database. The fences (lease + CAS + generation) must serialize the
writers; the harness observes zero data loss and a consistent final read
5/5 (`scripts/s3-concurrency-test.ps1`, OBSERVED 2026-08-10). Whatever you
observe — including oddities — is a valuable finding.

### Step 5 — Exercise Studio in the browser

After the CLI session succeeds, run:

```powershell
otsardb studio --port 8717
```

Open `http://127.0.0.1:8717/` in the browser on the same computer. Confirm
that the page opens, the Workbench and Monitor tabs work, the connection
test/query flow returns a result, and the layout remains usable when the
browser is narrowed to a mobile-sized viewport. This is a local browser UI
test; it is not remote phone administration. Never expose port 8717 beyond
loopback because Studio has no authentication or TLS.

Record the browser name/version, OS, viewport class (desktop/tablet/mobile),
and any console/network error in the feedback form. The `--window` option is
not part of this beta gate.

### Step 6 — Submit feedback

Open a GitHub issue (or append to the beta tracking PR) using
`docs/beta/feedback-template.md`, with consent to publish as a change record
(the template's last field). Every submitted finding becomes a change record or
is explicitly marked non-issue in the triage record (§5). No finding is ever
dropped silently.

## 4. Provider recommendation matrix for testers

| Provider | S3 CAS | Best for | Notes | Classification |
|---|---|---|---|---|
| **MinIO local** (5-min setup) | yes | default; multi-writer session; durability/recovery; performance | free, private, no RTT; no `OTSARDB_S3_SINGLE_WRITER` | OBSERVED 2026-08-10 (7/7 + 5/5) |
| **Cloudflare R2** | yes | multi-writer on a real global cloud; free tier available | `OTSARDB_S3_REGION=auto`; conditional writes return 412 | OBSERVED 2026-08-10 (7/7 + 5/5) |
| **MinIO BR datacenter** (EasyPanel) | yes | multi-writer over real internet RTT | internal deployment; same semantics as MinIO local | OBSERVED 2026-08-10 (7/7 + 5/5) |
| **Wasabi** (us-east-1) | **no** | durability/recovery on a real provider, **single-writer mode only** | requires `OTSARDB_S3_SINGLE_WRITER=1` (explicit assertion — deployment must really serialize writers); **no multi-writer session** | OBSERVED 2026-08-10 (7/7 single-writer) |
| **Magalu Cloud** (br-se1) | **no** | same as Wasabi; BR region | same `OTSARDB_S3_SINGLE_WRITER=1` requirement; 501 on conditional headers | OBSERVED 2026-08-10 (7/7 single-writer) |
| **AWS S3 / Backblaze B2** | — | — | not validated yet; not recommended for beta testers | UNTESTED |

Rules for testers:

- **CAS providers** (R2, MinIO): you may run the multi-writer session; the
  probe selects CAS publication automatically.
- **Core-only providers** (Wasabi, Magalu): writes fail closed unless
  `OTSARDB_S3_SINGLE_WRITER=1` is set. This is expected, not a bug — report
  it only if the error message is misleading.
- Probe first if unsure: `otsardb-s3-probe` reports `S3 Core: yes/no` and
  `S3 CAS: yes/no` for any endpoint.

## 5. Feedback triage SLA and the change-record rule

Every submitted finding (GitHub issue, template filled) is triaged by the
operator/lead within **3 working days**:

| Severity | Meaning | Triage outcome |
|---|---|---|
| **blocker** | data loss, corruption, wrong query results, durability/recovery broken | next change set; a change record is opened the same day; feature work on the affected area pauses until triaged |
| **major** | core flow unusable for a documented use case, silent misbehavior | next change set; change record in ≤ 1 cycle |
| **minor** | friction, performance, UX, config surprise | change record or bundled into the next minor change record |
| **typo** | docs/error-message issue | bundled with the change record that motivated it |

**The rule (append-only):** every finding becomes a new change record
`NNNN-short-description.md` **or** is explicitly marked *non-issue* with a
recorded reason in the triage record (e.g. `docs/beta/triage-log.md` when
the log is opened). A non-issue mark must answer *why*, never just a "no".
Nothing is dropped silently, and no finding is ever resolved by rewriting an
older change record.

Consent: findings are published as change records only with the tester's consent
(the template's last field); the published change record carries the masked
provider/endpoint and no credentials.

## 6. Launch checklist (verifiable items with evidence)

| # | Item | How to verify | Status at launch | Classification |
|---|---|---|---|---|
| 1 | Harness green on all 5 providers | `powershell -File scripts/validate-all-providers.ps1`; every provider row `7/7` (+ `5/5` concurrency for CAS providers); artifact lands in `scripts/validation-results/YYYYMMDD-providers.json` | DONE — 2026-08-10, ALL PROVIDERS PASSED, artifact `20260810-providers.json` | OBSERVED |
| 2 | Package/release path is coherent | packaging scripts and workflow reference existing `docs/guides/*` files and include `STUDIO.md`; source paths corrected, but a new release asset must still be generated from the final commit | **DONE — source paths corrected; final artifact pending** | OBSERVED / UNTESTED |
| 3 | Quickstart walkthrough executed by a fresh person | a person who has never used OtsarDB completes steps 1–6 of §3 without help; result recorded in `docs/beta/triage-log.md` | **PENDING** — needs first real tester | UNTESTED |
| 4 | Known limitations and Studio guide published | `docs/guides/KNOWN_LIMITATIONS.md` and `docs/guides/STUDIO.md` are present and synchronized with the kit and `docs/AVAILABILITY_MODEL.md` | **DONE — source files present and synchronized** | OBSERVED (2026-08-12) |
| 5 | Feedback template ready | `docs/beta/feedback-template.md` exists, covers all required fields (§5 of this kit); a sample issue accepted | DONE | OBSERVED (file present) |
| 6 | Current audited CLI session verified | run CMD 1–4 from §3 against local MinIO with the final release binary; all exits 0, row read back on empty cache | **PENDING** — the historical result is not a current release-artifact result | UNTESTED |
| 7 | Studio browser smoke verified by a fresh person | complete Step 5 on the final release binary and record browser/OS/viewport/error result | **PENDING** — first external browser session | UNTESTED |
| 8 | Change-record validator green | `python scripts/check-project-migrations.py --validate-all` | **DONE** | OBSERVED |
| 9 | Artifact version matches its release label | `otsardb --version` token equals the tag/package version; no unexplained `-dev` binary under a stable tag | **BLOCKED** — current source reports `0.1.0-dev` while the existing GitHub release is labelled `v0.1.0` | INCONCLUSIVE / UNTESTED |

## 7. Definition of beta-exit

The beta is over when **all** of the following hold (evidence required for
each, reproducibility-style record: date, binary, artifact):

1. **No open blocker/major findings** older than one triage cycle (3 working
   days) in the triage log.
2. **≥ 3 distinct testers** completed the full onboarding (steps 1–6),
   recorded as feedback entries with consent.
3. **Sessions on ≥ 3 distinct providers**, including ≥ 1 CAS-capable provider
   with at least one two-writer session — i.e. multi-writer CAS fencing
   exercised outside the development machine.
4. **At least one tester finding converted into a landed change record** — the
   feedback loop is proven end-to-end, not just documented.
5. **Launch checklist fully green** (§6): package/release path coherent,
   final artifact smoke-tested, quickstart walkthrough done by a fresh
   person, Studio browser smoke completed, and the known-limitations/Studio
   pages published and consistent. The artifact version must also match
   its release label; a stable-looking tag must not ship an unexplained
   `-dev` binary.
6. **Exit does not grant HA claims.** Level C (orchestrated) and F (multi-S3)
   remain UNTESTED; "high availability", "RPO zero" and "multi-region"
   remain forbidden until the AVAILABILITY_MODEL §4 acceptance list is
   fully OBSERVED. Beta exit claims exactly what the beta tested:
   durability, zero-local recovery, multi-provider behavior and multi-writer
   CAS fencing validated by real users.

## 8. References

- `docs/AVAILABILITY_MODEL.md` — levels A–F, §4 acceptance criteria
- `docs/S3_PROVIDERS.md` — capability model (Core/CAS), probe, per-provider
  observations
- `docs/guides/QUICKSTART.md` — end-user install/usage
- `docs/beta/feedback-template.md` — the feedback form
- `docs/performance/BENCHMARKS.md` — recorded evidence for the claims in this
  kit
