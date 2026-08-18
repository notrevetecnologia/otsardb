# OtsarDB — Performance Model (Read/Write Paths and HTTP Entry Points)

> Written against commit `58c55f79ec64169515c4f6de56bae17332e35480`
> (`desenvolvimento`), verified by **source inspection on 2026-08-13**. No
> binary was rebuilt or executed for this document. Evidence per the
> project's evidence rules: `OBSERVED` = code path confirmed by inspection
> (file:line cited); `PROJECTED` = request count derived from code structure,
> **not** a measured result; `UNTESTED` = not observed. Measured
> bytes/timings are quoted from the record that captured them (with its
> date/binary/endpoint), never restated as new results.

## 1. Read paths and exactly where HTTP enters

The read path is served from the local execution image; HTTP enters only at
materialisation time or on a lazy page miss.

```text
HOT read                        WARM read (reopen, cache reuse)
page in local image/RAM         HEAD GET -> cache probe (exact match)
  |  0 HTTP                        |  copy saved image; skip recovery
  v                                v
SQL engine reads OS VFS          SQL engine reads OS VFS

COLD read (reopen, empty/advanced cache)
HEAD GET + db.json GET + manifest-chain GETs
  + checkpoint base + segment GETs (full recovery)
  or + on-demand coalesced Range GETs (lazy hydration)
```

- **Hot read** — `otsardb_io_read` (`local_vfs.c:94`) delegates to the OS VFS
  `xRead`; zero store/HEAD calls. `OBSERVED`. A lazy session's
  `page_hydrate_hook` (`local_vfs.c:100`) is the only read-side path that can
  touch S3, and only for pages not yet materialised.
- **Warm read** — HEAD GET (`s3_database.cpp:1301`) + `db.json` GET at publisher
  init (`wal_publisher.c:344`) + local copy; **zero** segment/manifest GETs.
  `OBSERVED` (`s3_database.cpp:1332-1336` probe).
- **Cold read (full)** — HEAD (`recovery.c:773`) + metadata + manifest chain +
  segments, oldest-first into a new image.
- **Cold read (lazy)** — HEAD (`lazy_hydration.c:3276`) + metadata + newest
  checkpoint base; the remaining segments are fetched as **coalesced Range
  GETs** on page misses (`lazy_hydration.c:1861,2584,2924`; coalescing and
  frame-offset precision are enabled by default).
- **Cold read (delta)** — HEAD (`recovery.c:912`) + chain walk + only the
  segments newer than the cached image.

### Where HTTP enters (summary)

| Entry | Code | Direction |
|---|---|---|
| Commit segment/manifest/HEAD | `commit_publish.c:553,658,672-684` | PUT |
| Open HEAD read | `journal.c:112` (`otsardb_store_get`) | GET |
| Open db.json read | `db_metadata.c:134` | GET |
| Manifest chain walk | `recovery.c:233,720` | GET |
| Lazy page miss | `lazy_hydration.c:1861` (`otsardb_store_get_range`) | Range GET |
| Writer-lease renew/claim | `s3_database.cpp:356` (thread) | PUT/CAS |

## 2. Write path request model

```text
SQL COMMIT -> WAL xSync
  -> wal_reader_sync_hook (local_vfs.c:195-240)
  -> wal_publisher sync: refresh_head (1 HEAD GET)   [wal_publisher.c:1522]
  -> publish_one_commit -> otsardb_publish_commit
      1. PUT each segment        [commit_publish.c:553]
      2. PUT manifest            [commit_publish.c:658]
      3. PUT HEAD (If-Match CAS) [commit_publish.c:672-684]
  -> xSync returns SQLITE_OK
```

- **Small transaction (1 segment):** 1 HEAD GET (refresh) + **3 PUTs**
  (segment + manifest + HEAD). `PROJECTED` (derived from the code path).
- **Large transaction (N segments):** 1 HEAD GET + **(N + 2) PUTs**.
  `PROJECTED`.
- **Segment count bound:** a commit spans at most `OTSARDB_MANIFEST_MAX_SEGMENTS`
  (128) segments (`WAL_PIPELINE.md` "Bounded/streaming segment construction");
  a transaction exceeding it fails at `xSync` rather than committing partially.
- **Large-object multipart:** the object-store PUT layer splits large segment
  uploads into multipart parts with parallel workers
  (`s3_object_store.cpp:292,450-515`). This is *inside* one logical segment PUT,
  not an extra commit-level request.

## 3. Write amplification

Amplification is **per segment, not per page**. SQLite pages → WAL frames →
batched into bounded immutable segments (default 16 MiB payload budget,
`otsardb_wal_publisher_set_segment_payload_limit`, `WAL_PIPELINE.md`). A
single-page update and a multi-page transaction both produce **one manifest and
one HEAD**; the number of segment objects grows with transaction *payload size*
(in bounded chunks), never with the number of *rows* or *pages* touched in
isolation. There is no per-page remote object. `OBSERVED` (code structure);
per-page payload cost `PROJECTED`.

## 4. Remote request model

| Path | Remote requests | Evidence |
|---|---|---:|---|
| **Hot read** (page present) | 0 | `OBSERVED` `local_vfs.c:94-108` |
| **Cold page miss** (lazy) | 1 coalesced Range GET | `OBSERVED` path; count `PROJECTED` |
| **Warm reopen** (unchanged HEAD) | 2 GET (HEAD + db.json), 0 segments | `OBSERVED` `s3_database.cpp:1301`/`wal_publisher.c:344` |
| **Cold reopen — full** | HEAD + db.json + manifests + all segments | `OBSERVED` (measured 9 GETs / 374,615 B, 20k-row) |
| **Cold reopen — lazy** | HEAD + db.json + checkpoint base + N coalesced Range GETs | `OBSERVED` (measured 4 Range GETs / 13,010 B hydration) |
| **Cold reopen — delta** | HEAD + db.json + chain walk + delta segments | `OBSERVED` (measured 8 GETs / 14,135 B) |
| **Commit (small tx)** | 1 HEAD GET + 3 PUTs | `PROJECTED` |
| **Commit (large tx, N segments)** | 1 HEAD GET + (N + 2) PUTs | `PROJECTED` |
| **Writer-lease renew** | 1 CAS PUT / TTL÷3 | `OBSERVED` `s3_database.cpp:362-364,372` |

### Measured vs projected

- **Measured (OBSERVED elsewhere, quoted not re-run):**
  - Delta vs full reopen on the same 20k scenario: 14,135 B vs 374,615 B
    (**26.5× fewer bytes**), 0 vs 4 hydration GETs — recorded 2026-08-11,
    local MinIO.
  - Session-level delta reopen **128 ms** vs full lazy reopen **237 ms**
    (single-sample, local MinIO) — recorded 2026-08-12.
  - Cloudflare R2 cold reopen P50 **4.784 s**, warm delta reopen P50
    **2.646 s** — recorded 2026-08-12.
- **Projected (derived here, not measured):** all per-operation request counts
  in the table above (e.g. "1 commit ≈ 3 PUTs"). They follow directly from the
  code, but were not re-counted against a live endpoint for this document.
- **Untested:** WAN-degraded delta request accounting (open item in the
  benchmarks page), per-provider large-multipart write amplification.

## 5. Where the cost concentrates

- **Writes** are the only latency-bearing path for an in-flight transaction:
  synchronous remote publication before the SQL COMMIT ack
  (`WAL_PIPELINE.md` "Durable publication point").
- **Reads** after materialisation are local; the read-side remote cost is
  entirely a *reopen* cost (HEAD + metadata + materialisation), amortised by
  the reusable cache and the delta path across sessions.
