# OtsarDB Storage Model

> Public architecture reference. Grounded in `src/storage/*` and `src/ha/lease.c`.
> Evidence discipline (the project's evidence rules): every claim here is marked
> `OBSERVED` (read from the cited source file), `PROJECTED`, or `UNTESTED`.
> No claim below converts an intended test into a passing one.

## One database = one S3 prefix, not one file

An OtsarDB database is a **directory tree under a single S3-compatible object
prefix** (`database_root`), never a single object. Authoritative state is a
small set of mutable pointer/metadata objects plus an unbounded set of
immutable, content-addressed data objects. `OBSERVED` in
`src/storage/commit_publish.c`, `src/storage/journal.c`,
`src/storage/db_metadata.c`.

```text
<database_root>/                          one S3 prefix == one database
├── HEAD.json                             authoritative commit pointer (the ONLY
│                                         mutable publication point)
├── db.json                               database metadata (created once,
│                                         never rewritten)
├── commits/
│   └── <commit_id>.json                  commit manifest v4 (hash-linked chain
│                                         node; keyed by commit id)
├── segments/
│   └── sha256/<XX>/<sha>.otsseg          immutable WAL-page batches,
│                                         content-addressed (<XX> = first two
│                                         hex chars of the sha)
├── checkpoints/
│   ├── <manifest_sha256>.json            checkpoint manifest (content-addressed
│   │                                     by its own sha256)
│   └── <group_sha256>/page-<N>.otscp     immutable page groups (≤ 4096 pages
│                                         each; keyed by the GROUP's sha256)
├── CHECKPOINT.json                       pointer to the latest checkpoint
│                                         manifest (optimization only)
├── lease.json                            writer lease (NOT part of the database
│                                         image; invisible to recovery)
└── replica/
    ├── <commit_id>.json                  per-commit multi-S3 replication ledger
    └── epoch.json                        one-time promotion epoch fence
```

## Object catalogue (exact keys and fields)

Keys are relative to `database_root`; `root_join` adds a `/` unless the root
already ends in one. `OBSERVED` by reading the encoders/decoders cited below.

| Object | Key | Writes | Contents |
|---|---|---|---|
| HEAD | `HEAD.json` | CAS or unconditional (single-writer) | `{"otsardb_format":1,"database_id":"…","generation":N,"commit_id":"…","commit_key":"<abs manifest key>","commit_sha256":"<64 hex>"}` (`src/storage/journal.c`) |
| metadata | `db.json` | conditional create (If-None-Match) | `{"otsardb_format":1,"database_id":"…","page_size":N}` (`src/storage/db_metadata.c`) |
| segment | `segments/sha256/<XX>/<sha>.otsseg` | immutable put (content-addressed; conditional create when supported, else GET+compare) | encoded WAL-page batch; LZ4 per frame; optional AES-256-GCM envelope `[iv(12)][ciphertext][tag(16)]` (`src/storage/commit_publish.c:540`) |
| manifest | `commits/<commit_id>.json` | immutable put (content-addressed idempotent) | manifest v4 (below) (`src/storage/commit_publish.c:647`) |
| checkpoint manifest | `checkpoints/<manifest_sha256>.json` | plain put (idempotent) | page-group refs + `covered_generation` (`src/storage/checkpoint_build.c:171`) |
| checkpoint page group | `checkpoints/<group_sha256>/page-<N>.otscp` | plain put (idempotent) | raw page bytes, grouped ≤ 4096 pages (`src/storage/checkpoint_build.c:130`) |
| checkpoint pointer | `CHECKPOINT.json` | plain put | `{covered_generation, database_id, checkpoint_sha256, checkpoint_key}` (`src/storage/checkpoint_build.c:196`) |
| lease | `lease.json` | CAS If-None-Match / If-Match | `{"owner":"<instance>","generation":N,"address":"…"}` (`src/ha/lease.c:236`) |
| replica ledger | `replica/<commit_id>.json` | CAS (identity+state) | commit_id, generation, state, manifest_sha256, etags (`src/storage/replica.c:1283`) |
| replica epoch | `replica/epoch.json` | CAS If-None-Match / If-Match | `{epoch, owner, generation, model, …}` (`src/storage/replica.c:556`) |

Note on naming: the commit manifest lives under `commits/`, not `manifests/`,
and the pointer object is `HEAD.json`, not `HEAD`. These are the exact keys the
code emits; documents that name other keys are stale.

## The immutable, hash-linked commit chain

A database's history is an append-only chain of commit manifests, each of which
is content-verified by the manifest that points at it:

- `HEAD.json` names the current commit: `commit_key` (the absolute manifest
  key) and `commit_sha256` (the manifest's SHA-256 over its final JSON text).
  `OBSERVED` in `src/storage/journal.c`.
- Each manifest v4 records `parent_commit_id` and `parent_manifest_sha256`, so
  the chain can be walked from HEAD back to the genesis commit and every link
  hash-verified on the way. `OBSERVED` in `src/storage/commit_publish.c:729`
  (`otsardb_resolve_commit`) and `src/storage/recovery.c` (chain walk).
- Genesis is the only manifest with an empty `parent_commit_id` and
  `parent_generation == 0`.

```text
HEAD.json ──commit_sha256──▶ commits/w…-0002.json ──parent_manifest_sha256──▶
                             commits/w…-0001.json ──parent_manifest_sha256──▶
                             commits/w…-0000.json (genesis, parent empty)
        │ segments:             │ segments:                 │ segments:
        └▶ segments/sha256/…/…  └▶ segments/sha256/…/…      └▶ segments/sha256/…/…
```

Segments are **content-addressed**: the object key embeds the segment's
SHA-256 (plaintext: the `.otsseg` bytes; encrypted: `sha256(iv || ciphertext)`,
see `src/storage/commit_publish.c:209`). This makes segment and manifest PUTs
idempotent — re-publishing the same bytes lands on the same key, and an existing
identical object is accepted (`immutable_put`, `commit_publish.c:34`). Because
only `HEAD.json` is a mutable pointer, a commit exists as *prepared/orphan*
state until the HEAD CAS lands; a prepared object is never committed merely by
existing. `OBSERVED`.

### Manifest v4 (per-segment page sets + frame offsets)

`OTSARDB_COMMIT_MANIFEST_VERSION == 4` (`src/storage/commit_manifest.h`). Each
segment entry carries:

- `key` — suffix relative to `database_root` (`segments/sha256/XX/<sha>.otsseg`);
- `sha256` / `size` — the **stored** bytes (the encrypted envelope when
  encryption is on);
- `pages` — sorted, deduped, 1-based page numbers covered by the segment
  (empty/absent ⇒ "unknown page set", conservatively overlapping everything);
- `frame_offsets` — optional byte offsets of each page's frame block, for lazy
  range reads (absent for encrypted segments and legacy publishers).

Top-level additive fields: `post_apply_checksum` (rolling XOR checksum over the
post-commit page state) and `segment_enc` (one entry per segment; `{}` =
plaintext, else `{"alg":"AES-256-GCM","iv","tag"}`). `OBSERVED` in
`src/storage/commit_manifest.h`.

## Mutable vs immutable vs disposable

- **Mutable (authoritative):** only `HEAD.json` (commit selection),
  `CHECKPOINT.json` (checkpoint selection), `lease.json` (writer ownership),
  `replica/epoch.json` (promotion fence). Each is CAS-guarded when the store
  supports conditional writes, and every mutation is an If-None-Match /
  If-Match overwrite. `OBSERVED`.
- **Immutable (content-addressed):** `segments/…`, `commits/<commit_id>.json`,
  `checkpoints/…`, `.otscp` page groups, and per-commit replica ledgers are
  never rewritten after creation. Re-publish of the same bytes is a no-op.
- **`db.json`:** created exactly once (conditional create); a conflicting
  create with a different `database_id`/`page_size` fails the open with
  `SQLITE_MISMATCH`. `OBSERVED` in `src/storage/db_metadata.c`,
  `src/storage/wal_publisher.c:343`.
- **Disposable (not authoritative):** the local execution database image
  (`.db`, `-wal`, `-shm`) and the reusable local cache
  (`cache.json` + `otsardb-cache-index.json` under the cache dir,
  `src/storage/local_cache.cpp`) are reconstructible from S3 and must never be
  the durable source of truth. The local image is
  re-materialised from the remote HEAD on a cold open.

## GC and checkpointing

Checkpoints are a recovery *optimization*, never a second source of truth:
recovery always walks the commit chain from HEAD; a checkpoint only lets it
start from a bounded base image. `OBSERVED` in `src/storage/checkpoint_build.c`
(builds by running the normal recovery path, then uploading page groups) and
`src/storage/gc.c` (the live set `HEAD`, `CHECKPOINT.json`, `lease.json`,
`db.json`, and everything reachable from the chain is never swept; orphaned
segments are). `OTSSEG01`-style segment magic and per-frame CRC-32 are the
on-the-wire corruption guards (`src/storage/segment.c`).

## What is NOT stated here

- The storage model says nothing about *multi-region* or *high availability*;
  see `docs/architecture/DURABILITY.md` and `docs/AVAILABILITY_MODEL.md` for the
  exact, evidence-qualified claims.
- The rolling checksum and the page-set-in-manifest design are described
  neutrally above; external comparisons are out of scope for this document.
