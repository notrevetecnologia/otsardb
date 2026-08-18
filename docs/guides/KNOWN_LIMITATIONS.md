# OtsarDB — Known Limitations (beta, honest edition)

> One page, on purpose. OtsarDB's evidence rules require every
> claim to be classified: **OBSERVED** (executed and recorded, with date),
> **PROJECTED**, **UNTESTED**, **INCONCLUSIVE**, **NOT_APPLICABLE**.
> Nothing below is exaggerated; nothing below is a marketing statement.

Version: audited against branch `desenvolvimento` on 2026-08-13; consult
Git for the exact commit. The original beta edition was dated
2026-08-10 (Windows x64).
Read this before evaluating.

## 1. Encryption is opt-in and segment-scoped

Segment payload encryption at rest is IMPLEMENTED:
`OTSARDB_S3_ENC_KEY` (64 hex chars) enables an AES-256-GCM per-object
envelope on every published `.otsseg`
segment, off by default (unset key = byte-identical plaintext path).
Scope: segment payloads only — the manifest JSON, checkpoint page
objects and `HEAD` stay plaintext. Use provider-side encryption
(server-side encryption, bucket policies) for those. Wrong/missing keys
fail closed (OBSERVED 2026-08-11, incl. a live `mc cp`
plaintext-pattern grep).

## 2. HTTP forwarding is loopback-only — never expose on a network

`--forward-port` / `OTSARDB_S3_FORWARD_PORT` serves write forwarding on
`127.0.0.1` **without TLS and without authentication** (design: M2
HTTP forwarding). It is a local same-machine
convenience. Exposing it on any network is a security hole. Use the
standard path: each machine opens the database directly against S3
with its own credentials.

## 3. Wasabi / Magalu: single-writer required (no CAS)

Wasabi ignores conditional headers (`If-None-Match: *` -> 200 overwrite,
wrong `If-Match` -> 200) and Magalu returns 501 Not Implemented for them
(OBSERVED with signed requests, 2026-08-10). OtsarDB therefore refuses
writes on these providers unless `OTSARDB_S3_SINGLE_WRITER=1` — which is an
**assertion, not a lock**: you must guarantee only one writer can publish
that database at a time. Validation on both providers: 7/7 single-writer
(OBSERVED 2026-08-10, `scripts/validation-results/20260810-providers.json`).

## 4. Multi-writer (CAS) — what is actually validated

Two-writer CAS concurrency is validated **5/5 PASS** on:
- MinIO local (OBSERVED 2026-08-10);
- MinIO BR datacenter (OBSERVED 2026-08-10);
- Cloudflare R2 (OBSERVED 2026-08-10, first real paid/global CAS provider).

AWS S3 and Backblaze B2: **not exercised** (no accounts available) —
OPTIONAL runs, explicitly NOT a gap: R2 and MinIO share AWS's
conditional-write semantics, and five providers are already validated.
Capability detection is
provider-agnostic (any endpoint returning 412 for conditional PUTs gets
CAS automatically).

High-volume boundary (OBSERVED 2026-08-13): the corrected professional S8
CAS stress recorded 100/100 writer sessions and 100 rows on a fresh
verification with a lease-aware 60-attempt retry budget. An 8-attempt
diagnostic runner completed 97/100 sessions and observed 97 fresh rows, with
no acknowledged rows lost. The release evidence therefore proves the
recorded local CAS envelope and retry budget, but not arbitrary
WAN/provider/client retry policies.

Magalu benchmark boundary (OBSERVED 2026-08-13): functional validation passed
7/7 when the remote benchmark was skipped, but the complete provider run
failed its performance step twice while cleaning up remote objects. The
latest full five-provider matrix is therefore 4/5 PASS, not 5/5. Treat the
Magalu data as functionally validated but its current benchmark result as
UNRESOLVED until the endpoint cleanup/performance behavior is isolated.

## 5. Studio is browser-first, loopback-only, with an optional Windows shell

`otsardb studio` IS BUILT: an embedded single-page web UI served on
127.0.0.1 (default port 8717, `--port` flag, browser auto-open), with
connection profiles, schema explorer, grid editing, monitor tab, admin
actions (checkpoint/GC), server-side paging, query history, DDL panel,
connection test, SQL dump, dark/light mode and a responsive layout.
Server reads are bounded so one silent client cannot starve the single
Studio worker forever.

The official product contract is the browser mode: the embedded page is
intended for Windows, Linux and macOS browsers and for modern mobile browser
viewports. This is a UI/platform contract, not evidence that an Android or
iOS OtsarDB binary is shipped. The server remains loopback-only with no
auth/TLS, so a phone cannot remotely control a Studio process on another
computer; never expose the port as a workaround. The Windows WebView2 shell
is optional convenience behavior and is not a release requirement. Bucket/
database discovery before opening, autocomplete, EXPLAIN view and
full-result server-side sorting are not implemented. The CMake embedder
splits the page into compiler-safe chunks; UI growth must continue to be
tested on each release toolchain.

## 6. Multi-S3 is implemented; external failure coverage remains scoped

Multi-S3 replication IS IMPLEMENTED beyond the design record:
Model B synchronous dual publish, promotion/reconciliation,
read-replica sessions, session-level promotion store swap
and Model C quorum N-of-M with up to three replica destinations.
Real two-provider e2e OBSERVED, and the three-destination
Model C live run plus quorum kill matrix are OBSERVED.
A real Magalu + R2 + Wasabi Q=2 provider run and RPO data-leg check are
also recorded there. Remaining scope: an actual real-provider destination
failure during the live matrix, WAN-degraded N-destination latency and
read-replica exercise against every configured replica. This is
replication to configured destinations — not product-managed
multi-region sync.

## 7. RPO claims are scoped (harness delivered)

The RPO/RTO harness IS delivered and executed: `scripts/rpo-rto-test.ps1`
kill matrix RPO-OK on local MinIO, and its WAN edition
`scripts/rpo-wan-test.ps1` RPO-OK on Cloudflare R2 — no confirmed
commit lost; corrupted/deleted segments fail closed. Honest scope: the
lost-commit-response scenario is INCONCLUSIVE at harness level
(deterministic coverage in `otsardb_commit_fault_test`); the WAN
kill matrix on Wasabi/Magalu/MinIO-BR is UNTESTED; no
blanket RPO-0 claim is made.

## 8. macOS binary is untested; browser-mode portability is the contract

Windows x64 package is exercised end-to-end. Linux support was built and
exercised on an older WSL2 artifact (WSL2 GCC 13.3),
but the current site does not publish Linux until a package is rebuilt from
the current checkout. macOS OtsarDB binary build/run remains **UNTESTED** (no
build/run observed). Studio's browser mode is the official cross-platform UI
path, but this is not evidence that a macOS binary, Android/iOS binary or
native desktop window already exists.

---

**In short, for a beta tester:** you can create/insert/select data on S3,
reopen it from another machine, publish synchronously to multiple
configured destinations (dual/quorum), and use the responsive loopback
Studio browser UI on desktop or mobile-sized viewports (within the stated
loopback boundary). What you cannot do today: OtsarDB-side encryption of
manifests/checkpoint objects (segments only), safe network forwarding,
remote phone administration of another machine, multi-writer on
Wasabi/Magalu, a shipped mobile binary, physical multi-node orchestrator
failover, or a blanket RPO-0 claim.
