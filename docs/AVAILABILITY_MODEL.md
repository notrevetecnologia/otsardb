# OtsarDB — Availability Model

> Direction: the owner's HA position (2026-08-10). OtsarDB is NOT a
> traditional multi-active database cluster managed by the product. It
> separates *execution* from *durable state* and delegates machine
> replacement to the client's orchestrator (Swarm, Kubernetes, restart
> policies). This document defines exactly what is guaranteed and what is
> the client's responsibility. Every claim here follows the evidence
> classification rules: OBSERVED / PROJECTED / UNTESTED /
> INCONCLUSIVE / NOT_APPLICABLE.

## 1. Separation of concerns

```text
execução do banco        = processo/container OtsarDB   (disposable)
estado durável           = S3/object storage          (source of truth)
substituição da máquina  = orquestrador do cliente    (Swarm/K8s/restart)
```

If the machine or container running OtsarDB fails, a new instance can be
provisioned by the client, recover the database from S3 and continue
operation without depending on the original disk. This is
**storage-oriented high availability with client orchestration** — not a
product-managed multi-active cluster.

OtsarDB provides:
- durable storage outside the execution machine;
- recovery with no prior local state (OBSERVED: zero-cache reopen on 5
  providers);
- writer fencing via lease + CAS + generation (OBSERVED: concurrency 5/5
  on MinIO local, MinIO BR, Cloudflare R2);
- two OtsarDB instances safely sharing one database (candidate/leader
  takeover — OBSERVED at process level, container level,
  single-node Swarm and two-node Docker-in-Docker Swarm machine-loss
  simulation; physical-host failure remains external);
- fast recovery with lazy hydration and delta materialization (OBSERVED:
  real R2 cold reopen P50 4.784 s and warm delta reopen P50 2.646 s);
- health/readiness contract for orchestrators (implemented;
  consumed as the Swarm service healthcheck);
- documentation and tests proving exactly what is guaranteed.

The client is responsible for:
- running two or more machines;
- running replicas under Swarm/Kubernetes;
- restart policy, health check, routing;
- choosing standby, automatic failover or multiple active instances.

This is an explicit division of responsibilities, not a shameful
limitation. Failure modes must be tested and documented as such.

## 2. Availability levels

### Level A — state durability

The S3 object store is the durable source of OtsarDB state. Local disk is
disposable cache/execution state.

Status: **OBSERVED** — WAL sync only returns success after segment +
manifest + HEAD are durably on S3; recovery validates the hash chain.
See also `docs/WAL_PIPELINE.md`.

### Level B — instance recovery

A new OtsarDB instance opens the same database from remote state with an
empty local cache.

Status: **OBSERVED** — zero-local reopen validated 7/7 on MinIO local,
MinIO BR, Cloudflare R2, Wasabi, Magalu (2026-08-10).

### Level C — orchestrated availability

An orchestrator recreates the instance on another machine and the client
reconnects. RTO includes failure detection, scheduling, startup,
recovery and first valid query.

Status: **OBSERVED in single-node and two-node Docker-in-Docker Swarm**:
single-node kill → first valid write P50 **6027 ms** (N=3),
and two-node machine-loss simulation measured RTO P50 **19.1 s** with
client reconnect P50 **13.5 s**, six losses, zero loss and no split-brain.
The two-node run is a real multi-daemon orchestration test, but not a
physical-host/datacenter test. Physical machine failure, network partitions
and hung-process detection remain external scenarios. Until those are
observed, OtsarDB's claim is exactly:

> OtsarDB offers state recovery from S3 and is designed for client-orchestrated
> high availability.

#### Level C contract — `otsardb --health`

Orchestrators (Swarm/K8s restart policies) use the machine-readable
readiness contract: `otsardb --health <s3://target>` (alias
`--health-json`) emits **one JSON line** and exits without keeping a
session open. The probe is **read-only**: it never claims the writer
lease and never writes the database, so it works for leaders, candidates
and readers alike (OBSERVED while a live lease was held).

```json
{"status":"ok|degraded|error","role":"leader|candidate|unknown",
 "s3_reachable":true|false,"open_ms":N,"engine":"<version>",
 "target":"<target>"[,"error":"<message>"]}
```

- `status=ok` — valid open + read succeeded (ready-for-read);
- `status=degraded` — open ok, read failed (e.g. sticky hydration error);
- `status=error` — open failed or invalid usage;
- `s3_reachable` — from the open+probe round trip;
- `role` — today always `unknown`: the read-only open never observes the
  lease, and the public API exposes no lease peek. Honest limitation, not
  a silent guess;
- exit codes: 0 = ok, 1 = degraded, 2 = error. Any nonzero is unhealthy.

Example (docker healthcheck, per instance):

```bash
docker run -d --name otsardb \
  -e OTSARDB_S3_ENDPOINT=http://minio:9000 \
  -e OTSARDB_S3_ACCESS_KEY=... -e OTSARDB_S3_SECRET_KEY=... \
  --health-cmd "otsardb --health s3://otsardb-ci/health-0061/db" \
  --health-interval 10s --health-timeout 15s --health-retries 3 \
  otsardb
```

Set the healthcheck timeout above the observed failure bound of the
endpoint (local open ~30-60 ms; refused/forbidden endpoints observed
9.5-10.8 s to fail). Full schema and semantics are described above.

### Level D — two OtsarDB instances, one database

Two instances can exist for the same database, one writer and one
candidate/reader. Lease, CAS, generation and fencing prevent split-brain.

Status: **OBSERVED at process level** — candidate opens, polls the
lease, publishes return SQLITE_BUSY until takeover; takeover via CAS
after proven expiry; concurrency 5/5 on 3 providers. Container-level
(2 containers, RTO measured) is OBSERVED and under a single-node
Swarm orchestrator. Two-node Docker-in-Docker machine-loss and
client reconnect are OBSERVED; physical-host failover remains
UNTESTED.

### Level E — multiple independent writers

Independent writers publish via CAS, detect conflicts and retry safely.
This must not be confused with instance failover.

Status: **PARTIAL** — the fencing layer (CAS publish, generation fence,
lease) is OBSERVED; page-level conflict detection/classification is
OBSERVED deterministically (plain BUSY vs SQLITE_BUSY_SNAPSHOT 517,
asserted by `otsardb_conflict_test`) and multi-writer rebase is OBSERVED on
real endpoints (5/5 local MinIO + 1/1 Cloudflare R2, 2026-08-12).
The C-API-level live e2e asserting `sqlite3_extended_errcode` == 517 at
the app boundary is OBSERVED. Do not claim automatic
multi-writer merge semantics: rebase/convergence is validated, but conflict
policy remains an explicit application/database concern.

### Level F — multiple providers/regions (multi-S3)

State replicated to more than one S3 provider/region to reduce
dependency on a single provider. Requires a replication protocol of its
own; two independent PUTs are NOT an atomic commit.

Status: **IMPLEMENTED (phases OBSERVED)** — Model B synchronous dual
publish, promotion/reconciliation, read-replica sessions,
session-level promotion store swap and Model C quorum
N-of-M are implemented; three-destination live quorum, kill matrix
and a Magalu + R2 + Wasabi Q=2 run are recorded. Remaining
UNTESTED: a real-provider destination being actively killed during that
matrix, WAN-degraded N-destination latency and read-replica against every
configured replica. The design record and acceptance criteria are part
of the project documentation.

## 3. Terminology discipline

| Term | Means | Do not confuse with |
|---|---|---|
| Durability (A) | confirmed data survives machine death | — |
| Recovery (B) | new instance reopens from S3 | — |
| Availability (C) | orchestrator keeps service up | recovery alone |
| Failover (D) | one writer + takeover | multi-writer |
| Multi-writer (E) | concurrent CAS writers | failover |
| Multi-region (F) | replicated state across providers | any of the above |

## 4. Acceptance criteria before claiming "high availability"

The phrase "automatic high availability" may only be used after ALL of
the following are OBSERVED (recorded per the reproducibility rule):

- [x] two real containers using the same database (single-node Swarm stack; sequential container takeover);
- [x] abrupt leader instance death (kill);
- [x] safe candidate takeover (lease claim by the recovered instance after proven expiry; zero split-brain);
- [x] zero loss of confirmed commits in the two-node machine-loss matrix (process-level matrix is local + WAN R2);
- [x] zero split-brain (one live lease, one writer);
- [x] first valid SELECT after recovery (the health probe is a read probe);
- [x] first valid write after recovery (`count = 3+i`, unique ids, all 3 iterations);
- [x] RTO measured across multiple executions (N=3, kill → first valid write P50 6027 ms; P50/P95/P99 in the process-level run);
- [x] empty-cache test (cache volume cleared before every iteration);
- [x] real CAS-capable provider test (MinIO is CAS; R2 too);
- [x] reproducible record without credentials (git-ignored artifact `scripts/validation-results/20260811-swarm-failover.json`);
- [x] orchestrator test separate from OtsarDB test (Docker Swarm is the orchestrator).

Observed in Docker-in-Docker across two daemons: machine-loss/rescheduling
and client-side reconnection. Physical-host/datacenter failover and network
partition behavior remain UNTESTED, so the broad production HA claim stays
scoped to the recorded test boundary.

Until then, use exactly:

> "OtsarDB oferece recuperação de estado a partir do S3 e é projetado para
> alta disponibilidade orquestrada pelo cliente."

## 5. RPO/RTO baseline (real numbers as of 2026-08-10)

RPO/RTO follow NIST SP 800-34 and AWS Well-Architected REL13 definitions.

- **RPO (confirmed commits):** by design, a commit is only confirmed to
  the client after remote durability succeeds. The RPO harness is
  delivered and executed: kill matrix RPO-OK on local MinIO and on
  Cloudflare R2 over WAN — no confirmed commit lost, corrupt/
  deleted segments fail closed. The WAN kill matrix on Wasabi/Magalu/
  MinIO-BR remains UNTESTED. No blanket RPO-0 claim is made.
- **Ambiguous commits** (response lost): classified INCONCLUSIVE at
  harness level (no engine fault-injection environment for a lost PUT
  response); deterministic commit-identity resolution exists
  (`otsardb_resolve_commit`; used by the bounded
  retry).
- **RTO (measured reopen, P50, remote-bench harness, 2026-08-10):**
  MinIO local 100.5 ms · MinIO BR (datacenter) 1440 ms · Cloudflare R2
  (ENAM) 4289 ms · Wasabi 3306 ms · Magalu 3889 ms. These are open +
  first SELECT with empty cache (level B), NOT the full orchestrated RTO
  (level C), which additionally includes detection and scheduling.
