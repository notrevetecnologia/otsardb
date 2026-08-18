# OtsarDB Beta Triage Log

This file is the sanitized index of external tester feedback. It must never
contain access keys, secret keys, session tokens or unmasked endpoints.
Detailed reports belong in issues or sanitized copies of
`docs/beta/feedback-template.md`; every finding is either linked to a new
append-only change record or explicitly marked `non-issue` with a reason.

## Status

No external tester sessions have been recorded yet. This is intentional:
the first invitation must use a release artifact generated from the final
audited commit and the current quickstart/Studio documents.

## Entry format

| ID | Date UTC | Tester alias | Area | Severity | Status | Evidence / issue | Change record or non-issue reason |
|---|---|---|---|---|---|---|---|
| T-0001 | — | — | — | — | awaiting first tester | — | — |

## Intake rules

- Mask provider hosts and never include credentials in this file.
- Treat suspected data loss, corruption or wrong query results as blocker
  until reproduced or disproved.
- Record OS, architecture, binary version/hash, browser/version/viewport for
  Studio reports, provider class, masked endpoint, exact commands and exit
  codes.
- Do not silently close a report: link a change record or record a reasoned
  `non-issue` decision.
