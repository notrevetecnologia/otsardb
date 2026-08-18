# OtsarDB Studio — user guide (v0.7)

OtsarDB Studio is the visual workbench of OtsarDB: a browser UI to see and
touch databases that live in S3-compatible object storage (MinIO,
Cloudflare R2, Wasabi, Magalu or any S3-compatible endpoint). Every read
and write goes through the same engine path the CLI uses
(`otsardb s3://bucket/database "SQL"`) — Studio never manipulates local
database files or S3 objects directly.

The official Studio product contract is **browser-first**: the same embedded
page is served through the loopback browser mode on Windows, Linux and macOS,
and is designed for modern desktop and mobile browsers. The OtsarDB process
must still run on a supported desktop/server platform; this does not yet mean
that an Android or iOS OtsarDB binary is shipped.

## Starting Studio

```text
otsardb studio
```

A loopback-only web server starts on `http://127.0.0.1:8717` and opens
the default browser (the browser-open attempt is best-effort and silent:
on headless machines it just does nothing). To pick a different port:

```text
otsardb studio --port 9000          # --port wins
OTSARDB_STUDIO_PORT=9000 otsardb studio   # environment fallback
```

Port precedence: `--port` flag > `OTSARDB_STUDIO_PORT` env > default 8717.
`--port` is only valid with `otsardb studio` (any other use exits with a
usage error). Press `Ctrl+C` to stop the server.

## Legacy desktop window mode (optional Windows convenience)

```text
otsardb studio --window
OTSARDB_STUDIO_WINDOW=1 otsardb studio   # environment equivalent
```

This is an optional legacy convenience path, not the official Studio
compatibility contract. When available, the same UI is hosted in a native
**WebView2 (Edge Chromium) desktop window** instead of a browser tab. The
official and portable path remains `otsardb studio` in a browser.

WebView2 is never a build dependency. At runtime the window looks for the
WebView2 SDK loader DLL (`WebView2Loader.dll`) in the WebView2 Runtime
directories (Program Files / `%LOCALAPPDATA%`), the executable directory
and the vcpkg store, and loads it on the fly. When the loader is not
found — or on non-Windows platforms — `otsardb studio --window` prints the
exact reason and falls back to the browser behavior. The fallback is useful
for compatibility, but release acceptance must be based on browser mode,
not on the optional window shell:

```text
Desktop window unavailable: WebView2Loader.dll was not found (searched
the WebView2 runtime directories under Program Files and %LOCALAPPDATA%,
the executable directory and the vcpkg store); the desktop window is
unavailable
Falling back to the default browser.
```

The window is a thin shell only: every read and write still goes through
the loopback server and the same engine path. It may be removed or replaced
without changing the official Studio contract.

## Interface and platform support (v0.7)

The embedded Studio page has a responsive, dependency-free interface:
the workbench becomes a single-column layout on narrow screens, the
connection panel remains usable on small windows, controls wrap instead
of overflowing, and the result grid keeps horizontal scrolling for wide
tables. Dark mode remains the default and light mode remains available.

The official cross-platform contract is the loopback browser mode:
`otsardb studio` serves the same responsive page for Windows, Linux and macOS
browsers, and for mobile browser viewports. At the current security boundary
the server binds to `127.0.0.1`, so a phone cannot administer a Studio
running on another computer unless a future authenticated/secure remote
transport is deliberately added. Do not work around this by exposing the
unauthenticated port.

The optional `--window` shell is Windows/WebView2-only convenience behavior.
It is not required for support, packaging or release acceptance.

## Connecting

Fill the connection form (endpoint, region, bucket, database root, and
optionally access/secret keys — blank fields keep the process
environment) and press **Connect / Refresh schema**. The connection test
button (**Test connection**) runs a read-only health probe with the
form's current credentials and shows `ok` + open time (ms) or the error.

Connections can be saved as named **profiles** (browser `localStorage`
only), imported/exported as JSON, and switched from the picker — each
profile remembers its own editor SQL, result grid and page size. Query
history is kept per profile (last 100 queries, re-run with one click).

## Tabs

- **Workbench**: SQL editor + result grid + schema explorer. The grid
  supports client-side paging and sorting (current page), CSV export,
  and — for plain `SELECT * FROM "t"` results — server-side paging with
  a row total and editable rows (Save = UPDATE on the primary key, Del =
  DELETE, + Add row = INSERT; single-user optimistic, no locks).
  Clicking a table in the schema tree opens it and shows its DDL.
- **Monitor**: readiness status (role, lease owner/generation, S3
  reachable, open ms, engine, target) with a refresh button and a 5 s
  auto-refresh toggle; admin actions (checkpoint on close, GC) with a
  consent gate for single-writer connections.

## Shortcuts

- `Ctrl+Enter` (Windows/Linux) or `Cmd+Enter` (macOS): run the current
  query.
- The layout is intended for touch-sized mobile browser viewports; mobile
  browser rendering is the official UI compatibility target, while native
  Android/iOS OtsarDB binaries remain UNTESTED/NOT_SHIPPED.

## Theme

The **Dark** button in the header toggles light/dark (default dark);
the choice is remembered per profile in the browser's `localStorage`.

## Exporting

- **Export CSV**: downloads the current result set as CSV (UTF-8 BOM).
- **SQL dump**: downloads a `.sql` file with the schema's `CREATE TABLE`
  statements (verbatim from `sqlite_schema`) followed by the table data
  as `INSERT INTO ... VALUES (...);` rows. The dump is assembled in the
  browser from the same `/api/query` results the grid shows, so it is
  bounded by the `--json` result caps (100 000 rows / 8 MiB per
  statement): a table that exceeds a cap fails that table's fetch and
  the dump reports the error instead of emitting a partial or truncated
  dump. Values are rendered as SQL literals (`'...'` with `''`
  doubling; NULL as `NULL`; numbers raw).

## Security boundary (important)

- The server binds **127.0.0.1 only** and speaks plaintext HTTP with **no
  authentication** — it will happily run arbitrary SQL for any request.
  Never expose the port beyond loopback (same boundary as
  `--forward-port`).
- **Credentials stay in the browser.** Keys you type are held in the
  page's JavaScript and sent, per request, to the local server, which
  applies them to the process environment only for the duration of that
  request (never logged, never written to disk). Exported profile files
  contain your keys — never commit them.
- The UI is embedded in the binary: no CDN, no third-party scripts;
  the page works offline.
- Everything Studio persists is browser-side (`localStorage`: profiles,
  history, theme). Deleting it loses nothing authoritative — the S3
  store remains the only source of truth.

## Version

v0.7 (responsive UI and bounded server reads; browser-first platform
contract), on top of
v0.6 (desktop window mode) / v0.5 / v0.4 / v0.3 / v0.2 / v0.1. See
`studio/README.md` for the wire contract and the project changelog for the
engineering history.
