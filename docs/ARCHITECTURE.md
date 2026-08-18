# OtsarDB Architecture

## Product boundary

OtsarDB is the database engine. It is not a wrapper process around an installed SQLite binary and it is not the future DBaaS control plane.

The bootstrap uses SQLite source as the initial SQL engine codebase. The release binary is `otsardb`; users do not install SQLite separately. OtsarDB owns the storage boundary from day one through an OtsarDB VFS.

```text
Application / CLI / Studio
          |
          v
      OtsarDB API
          |
          v
   OtsarDB SQL Core
          |
          v
  OtsarDB Storage VFS
      /        \
  local VFS   S3 VFS (implemented; native `s3://` path, manifest v4, LZ4, recovery)
                 |
                 v
                S3
```

## Source strategy

The first bootstrap pins the official SQLite 3.53.4 amalgamation. It is fetched at build time, checksum-verified and statically compiled into OtsarDB. This keeps upstream updates reproducible while OtsarDB's own changes live in small, reviewable modules.

As OtsarDB begins changing SQLite internals (pager, WAL, transaction boundaries or page format), the affected upstream files will move under `src/engine/` and OtsarDB will maintain those files directly. Upstream sync remains explicit through `upstream/` metadata and patches.

## Storage contract

Today `otsardb-local` is a pass-through OtsarDB-owned VFS over the operating-system VFS. Its purpose is architectural: all database opens already cross an OtsarDB storage boundary.

The next VFS will be `otsardb-s3`. It must not be implemented as periodic file backup. S3 will become authoritative persistence for that target, with local RAM/NVMe treated as disposable cache.

## OtsarDB Studio

`otsardb studio` is the embedded browser visual administration surface. The
official product path is one loopback HTTP server plus the dependency-free
HTML/CSS/JS page, usable from desktop and mobile browser viewports on
Windows, Linux and macOS. Studio must use the public OtsarDB API and must not
read database files or S3 objects behind the engine's back. A Windows/WebView2
window may remain as an optional shell, but it is not the platform contract.
