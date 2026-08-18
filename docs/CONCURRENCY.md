# OtsarDB Concurrency Model

This document separates the meanings of "many readers", "many clients writing", "many writer nodes", and "multi-master". They are different engineering problems and OtsarDB treats concurrency as several distinct problems, each with its own proven mechanism, instead of one feature.

## Level 0 — local engine concurrency

Inherited from the SQLite core.

- many simultaneous readers
- one effective writer at a time per database
- WAL mode minimizes reader/writer blocking
- multiple callers may request writes; the engine serializes the actual write transactions

This is the baseline OtsarDB behavior and remains useful even after S3 support exists.

## Level 1 — S3-backed readers + one safe write source

```text
                  S3
             authoritative
                 storage
            /      |      \
         read    read    writer
        OtsarDB A  OtsarDB B  OtsarDB C
```

The storage engine provides page-level S3 access, manifest/version state, local disposable caches and immutable object versions. Multiple reader instances must observe committed versions safely. One write source avoids distributed commit conflicts while the object-storage contract is proven.

## Level 2 — many clients writing through one authoritative writer

```text
App A ----write----> OtsarDB A (follower) --forward--+
App B ----write----> OtsarDB B (follower) --forward--+--> OtsarDB C (writer)
App C ----write------------------------------------+

reads -> local replicas when consistency permits
```

To the application, many clients/nodes can write concurrently. Internally, one current writer serializes commits. A lease/epoch identifies that writer; followers forward writes; replicated WAL/commit state keeps followers warm; failover promotes another writer; fencing prevents two leaders from committing simultaneously.

This is not "fake concurrency". It is a normal distributed-database pattern and gives high availability without requiring arbitrary concurrent physical commits.

## Level 3 — optimistic independent writers on S3

Each writer reads a database version/snapshot, prepares an immutable change set, then attempts to publish a new manifest version conditionally.

```text
Writer A reads v100 ---- prepares A ---- CAS v100 -> v101  OK
Writer B reads v100 ---- prepares B ---- CAS v100 -> v101  FAIL
                                              |
                                    refresh / conflict check
                                              |
                                      retry or conflict
```

Required OtsarDB mechanics:

- versioned snapshots/manifests
- immutable segments/page groups
- conditional S3 writes (CAS/If-Match or equivalent)
- write-set/page-set conflict detection
- bounded automatic retry
- deterministic conflict errors
- orphan-object recovery/GC

Non-conflicting work may be retried/merged into a later version. True conflicts must never silently become lost updates.

## Level 4 — convergent multi-master data

This is an optional semantic layer for data explicitly declared as convergent/CRDT-enabled. Independent writers may continue during partitions and exchange changes later.

CRDT behavior must be opt-in because LWW/counter/set merge rules are not identical to ordinary relational transaction semantics.

## Capability map

| Capability | OtsarDB work |
|---|---|
| concurrent reads (SQLite) | preserve/test |
| serialized local writes (SQLite) | preserve/test |
| S3 page VFS/cache/manifest | adapt/harden |
| many nodes accepting writes via forwarding | adapt into OtsarDB API/topology |
| writer lease/failover/self-fencing | adapt/provider abstraction |
| WAL replication through object storage | integrate with OtsarDB durability contract |
| optimistic S3 writer CAS | port design to OtsarDB C/storage core |
| conflict detection/retry | adapt and test at OtsarDB transaction/page level |
| true multi-master merge (CRDT) | integrate later as explicit semantics |

## Product promise by stage

OtsarDB must state concurrency capability precisely:

- **Stage A:** many readers, serialized writes.
- **Stage B:** many reader nodes and many clients issuing writes, one authoritative committer with automatic failover.
- **Stage C:** multiple independent S3 writer processes with optimistic concurrency and conflict detection.
- **Stage D:** optional CRDT multi-master convergence.

No stage is considered complete until crash/fault tests prove that committed data cannot be silently lost or overwritten.
