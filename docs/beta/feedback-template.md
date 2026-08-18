# OtsarDB Beta — Structured Feedback Form

> Fill this form for every beta finding and open a GitHub issue with its
> content (or append to the beta tracking PR). Triage happens within 3
> working days (§5 of `docs/guides/BETA_LAUNCH_KIT.md`). Every finding becomes a
> change record or is explicitly marked non-issue in the triage record — never
> dropped silently.
>
> **Security rule:** never paste credentials, session tokens,
> or complete endpoints. Mask endpoints (`https://***.r2.cloudflarestorage.com`)
> and replace keys with `<redacted>`. Any file containing a credential is
> rejected and returned.

---

## 0. Metadata (the reproducibility basics)

| Field | Value |
|---|---|
| Date (UTC) | `YYYY-MM-DDTHH:MM:SSZ` |
| Tester (alias only) | `alias-<n>` |
| Binary | `otsardb --version` output (e.g. `otsardb 0.1.0-dev (engine base 3.53.4)`) |
| Commit SHA (if known) | |
| OS / architecture | e.g. Windows 11 x64, Ubuntu 24.04 x64 |
| Binary hash (optional) | e.g. `Get-FileHash otsardb.exe -Algorithm SHA256` |

## 1. Scenario

What were you doing, in one sentence? (e.g. "opened the database on a second
machine with an empty cache and ran a SELECT")

## 2. Provider + endpoint (masked)

| Field | Value |
|---|---|
| Provider (name only) | e.g. MinIO local / MinIO BR / Cloudflare R2 / Wasabi / Magalu |
| Endpoint (MASKED — host only, no keys) | `http://127.0.0.1:9000` / `https://***.r2.cloudflarestorage.com` |
| Region | e.g. `us-east-1`, `auto`, `br-se1` |
| Capability probe verdict (if you ran `otsardb-s3-probe`) | `S3 Core: yes/no` · `S3 CAS: yes/no` |
| `OTSARDB_S3_SINGLE_WRITER` set? | Y / N |

## 3. Command sequence (the session as executed)

Paste the exact commands, in order, with credentials replaced by
`<redacted>`. Use the canonical session from `docs/BETA_LAUNCH_KIT.md` §3
when possible and mark which of CMD 1–4 each line corresponds to.

```text
<redacted — command sequence>
```

## 4. Expected vs observed

| | Value |
|---|---|
| Expected | what the kit/docs said should happen |
| Observed | what actually happened |
| Diff | what differs between the two |

## 5. Exit codes

List the exit code of every command in §3 (0 = success; anything else =
failure).

```text
cmd 1 exit: 0
cmd 2 exit: 0
...
```

## 6. Error output (redacted)

Paste stderr/stdout excerpts, with secrets masked. Keep `otsardb:` diagnostic
lines — they are useful (e.g. lease acquisition, recovery, GC).

```text
<redacted — error output>
```

## 7. `OTSARDB_*` environment used

Tick all that were set for the session:

- [ ] `OTSARDB_S3_ENDPOINT`
- [ ] `OTSARDB_S3_REGION`
- [ ] `OTSARDB_S3_ACCESS_KEY` (do not paste the value)
- [ ] `OTSARDB_S3_SECRET_KEY` (do not paste the value)
- [ ] `OTSARDB_S3_SESSION_TOKEN`
- [ ] `OTSARDB_S3_SINGLE_WRITER=1`
- [ ] `OTSARDB_S3_PREFIX`
- [ ] `OTSARDB_CACHE_DIR`
- [ ] `OTSARDB_CACHE_REUSE`
- [ ] `OTSARDB_AUTO_CHECKPOINT` / `OTSARDB_AUTO_CHECKPOINT_COMMITS`
- [ ] `OTSARDB_GC` / `OTSARDB_GC_KEEP_MINUTES`
- [ ] none of the above

Other env (e.g. `AWS_*` fallbacks): ______

## 8. Cache dir behavior

- Cache directory used: `__________` (path, no secrets)
- Was the cache dir **empty** at the start of the session? Y / N
- Was it **deleted between commands** (zero-local test)? Y / N
- After the session, which files existed in the cache dir? (names only,
  e.g. `<db>.db`, journal files)
- If you reopened with a fresh cache, did the data come back? Y / N / n/a

## 9. Data loss suspicion

**Did you observe or suspect any data loss or corruption? Y / N**

If Y, mark the claim's confidence (observation / suspicion / unclear) and
describe exactly which data was written, which was read, and where the
discrepancy appeared. A suspected data-loss finding is **blocker** severity
until proven otherwise — do not discard it even if you cannot reproduce.

```text
<description>
```

## 10. Reproduction steps

Can it be reproduced? Y / N / unknown. If Y, give the minimal command
sequence (masked) that triggers it, and whether it reproduced on a second
run. Attach any logs with credentials removed.

```text
<minimal reproduction>
```

## 11. Severity

- [ ] **blocker** — data loss, corruption, wrong query results, durability
      or zero-local recovery broken
- [ ] **major** — core flow unusable for a documented use case, silent
      misbehavior
- [ ] **minor** — friction, performance, UX, configuration surprise
- [ ] **typo** — documentation or error-message issue

## 12. Consent to publish as change record

The OtsarDB feedback rule: every finding becomes an
append-only change record (or is explicitly marked
non-issue with a reason). The published change record will contain the masked
provider/endpoint and no credentials.

- [ ] I consent to this finding being published as a project change record
      (credential-free, endpoint-masked)
- [ ] I do NOT consent — the finding will be triaged internally and only
      the sanitized resolution will be recorded
