# Security Policy

OtsarDB's authoritative state lives in S3-compatible object storage, and the
engine is derived from SQLite's public-domain source. Report security
issues privately — do **not** open a public issue for a vulnerability.

## Supported versions

OtsarDB is in beta (`0.1.0-dev`, engine base SQLite 3.53.4). Until the first
stable release, security fixes land on the current development branch
(`desenvolvimento`, integrated to `main` via the long-lived PR) and are
described in the [changelog](CHANGELOG.md). There are no long-term-support
versions yet.

## Reporting a vulnerability

1. Report through GitHub's **private vulnerability reporting** — open the
   repository's "Security" tab → "Report a vulnerability". Only maintainers
   can see the report and the discussion.
2. If private vulnerability reporting is unavailable, contact a repository
   owner through a verified GitHub profile before disclosing details. Do not
   open a public issue containing an exploitable reproduction.

Expect an acknowledgment within 72 hours and a triage assessment within
one week. We will coordinate disclosure: fixes are published first, and
the advisory becomes public only after a fix or a decision not to fix.

## What to include

- OtsarDB version (`otsardb --version`) and commit SHA if known;
- OS, compiler/build config, S3 provider and region (for reproducible
  reproduction);
- steps to reproduce, minimal SQL if relevant, and the observed vs
  expected behavior;
- impact assessment (data integrity, confidentiality, availability).

## Known limitations (honest scope)

OtsarDB supports opt-in AES-256-GCM encryption for `.otsseg` segment payloads
through `OTSARDB_S3_ENC_KEY`; manifests, HEAD and checkpoint metadata still
need provider-side encryption (SSE, bucket policies) if they must be
encrypted. HTTP write forwarding is loopback-only, without TLS or
authentication, and must never be exposed on a network. Providers without
CAS (e.g. Wasabi, Magalu) require explicit `OTSARDB_S3_SINGLE_WRITER=1` —
an assertion, not a lock. Read the full, evidence-classified list in
[`docs/guides/KNOWN_LIMITATIONS.md`](docs/guides/KNOWN_LIMITATIONS.md).

## Credentials hygiene policy

- Secrets never enter code, commit history, logs, artifacts or docs.
- Local harness credentials live only in the git-ignored
  `scripts/providers.json`; per-run state and results are git-ignored
  (`scripts/*.run.json`, `scripts/validation-results/`).
- Tests mask keys and clean child environment variables.
- Runtime credentials are transport configuration: pass them via
  `OTSARDB_S3_ACCESS_KEY` / `OTSARDB_S3_SECRET_KEY` / `AWS_*` environment
  variables or the AWS credentials file — never in the `s3://` target
  string.
- If you believe a real credential was committed anywhere in the
  repository history, report it privately (above) and rotate it. Treat a
  leaked credential as compromised regardless of how long it was exposed:
  revoke it and issue a replacement.
