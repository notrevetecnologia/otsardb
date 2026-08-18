# OtsarDB — Cache Coherence

> Written against commit `58c55f79ec64169515c4f6de56bae17332e35480`
> (`desenvolvimento`), verified by **source inspection on 2026-08-13**. The
> FRESH-mode material (§1 table, §7) was added with the FRESH consistency
> mode and verified by **execution on 2026-08-13** (local MinIO +
> deterministic memory-store suite). Evidence per the project's
> evidence rules: `OBSERVED` = code path confirmed by
> inspection (file:line cited) or executed and recorded; `PROJECTED` =
> derived count; `UNTESTED` = not observed.

## 1. Two properties, deliberately separate

OtsarDB splits what is often collapsed into "cache consistency" into two
independent questions:

| Property | Question | How it is satisfied |
|---|---|---|
| **Data-cache validity** | Does the local image match *its own* generation? | Recovery validates the hash chain + segment checksums + whole-file checksum (`recovery.c`; `wal_publisher.c` `local_image_proof_current`). |
| **Global freshness** | Is that generation still the latest `HEAD`? | By re-reading `HEAD` at open, on a write attempt (`wal_publisher.c` `refresh_head`) — and, in FRESH mode, at the start of every statement (synchronous per-statement HEAD check + in-place re-materialisation). |

A session may hold an image that is **valid but stale**: byte-correct for
generation G, while S3 is already at G+k. That is not a bug — it is the
"session snapshot pinned at open" model (`CONSISTENCY_MODEL.md`).

## 2. The generation→cache association

The saved cache is bound to a generation by three fields stored together in
local metadata (`local_cache.h:19-23`):

```c
uint64_t generation;        // generation the image was saved at
char commit_id[64];         // commit id at that generation
char commit_sha256[65];     // manifest SHA-256 at that generation
```

- **Save** — on close, the publisher's current state is persisted
  (`s3_database.cpp:722-751`): `state.generation = publisher.current_generation`,
  `state.commit_sha256 = publisher.current_manifest_sha256`, then
  `otsardb_local_cache_save` (metadata written atomically, `local_cache.h:52`).
- **Probe (unchanged HEAD)** — `otsardb_local_cache_probe` requires the remote
  HEAD to match all three fields *exactly* (`local_cache.h:31`;
  `s3_database.cpp:1332-1336`). Match → copy image, skip recovery.
- **Delta (advanced HEAD)** — `otsardb_local_cache_read_state` reads the saved
  generation/manifest-SHA without copying (`local_cache.h:73`); if
  `cached.generation < head.generation`, the delta path applies only the newer
  segments on top of the cached image, after re-validating the chain and
  verifying that the manifest at `cached.generation` hashes to the saved SHA
  (`recovery.c:912`; `s3_database.cpp:1369-1408`).

The in-session equivalent of the association is the publisher's
`current_generation` / `local_image_generation` pair: the write path refuses to
publish when `current_generation > local_image_generation`
(`wal_publisher.c:1530-1539`), i.e. when the local image no longer represents
the HEAD it was based on.

## 3. Staleness analysis

- **Where staleness enters** — a remote commit from another instance is only
  discovered at reopen or at a write attempt (§1).
- **Magnitude** — up to the full generation delta since open; a read-only,
  long-lived session can be arbitrarily stale (bounded only by its own
  activity).
- **Cost to catch up** —
  - unchanged HEAD → warm cache copy (no segments), `OBSERVED` §2 probe path;
  - changed HEAD + cached image → delta materialization (segments `G+1..HEAD`),
    `OBSERVED` measured 26.5× fewer bytes than full recovery on
    the 20k-row scenario;
  - no cache → full recovery or lazy hydration, `OBSERVED`.

## 4. Failure analysis

| Failure | Behaviour | Evidence |
|---|---|---|
| **S3 down, session already open, hot reads** | Reads continue locally (zero remote work); writes fence (lease self-demotes after 3 transport errors). | `OBSERVED` `local_vfs.c:94`; `s3_database.cpp:388-397`. |
| **S3 down at reopen, hot cache** | Open fails: the HEAD read needed to confirm cache currency cannot complete. | `OBSERVED` `s3_database.cpp:1301` → `1436-1439` error path. |
| **HEAD read timeout at open** | Open fails closed; no image is served. | `OBSERVED` `s3_database.cpp:1436-1439`. |
| **Stale HEAD at write** (another writer advanced) | Generation fence → `SQLITE_BUSY`; CAS `If-Match` → `CONFLICT`. | `OBSERVED` `wal_publisher.c:1530-1539`; `journal.c:180`. |
| **Corrupt/missing cached image** | Probe/read_state returns 0 → full recovery from S3 (S3 stays authoritative). | `OBSERVED` `local_cache.cpp` `load_index` (self-heals); `s3_database.cpp:1339-1435`. |
| **Hot cache + cache probe match, but S3 object later deleted out-of-band** | See §5 — unsupported. | — |

## 5. Out-of-band bucket changes — NOT supported

External modification of the bucket is **not supported** and is documented as
such:

- **External `HEAD`/manifest/segment modification or rewrite** — breaks the
  hash-linked chain; recovery fails closed on the checksum/hash mismatch
  rather than serving the tampered state (`recovery.c` chain validation; the
  deterministic parent-manifest tamper test in `WAL_PIPELINE.md`). It is not a
  supported operation.
- **Lifecycle deletion of the current `HEAD`/manifest/segments** — recovery
  cannot reconstruct the current generation; the read fails closed. There is no
  "keep serving stale cache in lieu of the authoritative chain" behaviour at
  open (open must read `HEAD`).
- **A hot-cache session survives S3 object deletion for *reads already open***
  (reads are local), but this is an accident of the read model, not a
  supported durability guarantee — a reopen will fail.

The only supported cache-transition paths are the internal save/probe/delta
machinery (§2). GC (`OTSARDB_GC=1`) is the *only* supported deleter of
unreachable objects, and it is reachability-based with a grace period
(`S3_STORAGE.md` "Garbage collection").

## 6. Failure semantics of the generation fence

The fence compares two generations held in the publisher
(`wal_publisher.h:27,37-38`): `current_generation` (last HEAD the publisher
adopted) and `local_image_generation` (generation the materialised execution
image represents). It is armed right after materialisation
(`s3_database.cpp:1464` `otsardb_wal_publisher_set_local_image_generation`) and
re-armed on every successful commit (`wal_publisher.c:1096-1097`). Because the
same instance that writes is the one that materialised, its own writes never
trip the fence — only a *foreign* generation advance does.

## 7. Freshness modes

| Mode | Status | Behavior |
|---|---|---|
| **SNAPSHOT** (`OTSARDB_CONSISTENCY` absent or `snapshot`) | **implemented, default** | session snapshot pinned at open (this document's §1–§6). |
| **FRESH** (`OTSARDB_CONSISTENCY=fresh`) | **implemented** | one remote HEAD GET at the start of every `otsardb_exec` statement; a generation advance re-materialises the session image (probe → delta → lazy/full ladder) and swaps it in place before the statement executes. Fail-closed when the HEAD cannot be read; own commits adopt the anchor without re-materialising; the check is deferred inside an explicit transaction. `OBSERVED` 2026-08-13: deterministic F1–F8 suite (11/11 runs) + live cross-process e2e (fresh session saw the other process's commit at its next statement; snapshot control stayed pinned). |

The following remain **design ideas only**; none exists in the current tree
(`UNTESTED`, not implemented). They are recorded so future work can reference a
deliberate gap instead of assuming one was missed:

- **TTL freshness** — a configurable `max_staleness`; on a read whose snapshot
  exceeds it, re-read HEAD and re-materialise (or fail). FRESH mode (0123) is
  the stricter per-statement form of this; a TTL variant would trade
  staleness bound for fewer HEAD GETs.
- **Background HEAD polling** — a watcher that periodically re-reads HEAD and
  marks the snapshot stale (today the only background loop is the writer-lease
  thread; FRESH mode deliberately does its check synchronously per statement —
  an optional `OTSARDB_FRESH_POLL_MS` watcher was considered and not built).
- **Coordinator / read-your-writes forwarding** — an out-of-band coordinator
  (or the existing loopback forward server) that routes reads to the current
  leader for always-fresh reads.

Until one of those is implemented, the contracts are exactly
SNAPSHOT ("session snapshot pinned at open", §1, `CONSISTENCY_MODEL.md`)
and FRESH (per-statement HEAD validation).
