# Insights: testing

Tests, harnesses, emulator and capture runs, what counts as evidence. Entries are maintained with `kb insight ...`; the weekly RCA adds evidence. Weight = incidents backing it.

## I-0002 Different checks are different claims
- tags: testing,verification
- weight: 2
- status: active
- added: 2026-W42
- evidence: seed: AGENTS.md working agreement 2026-10
- history: 2026-W42 add

**Correction.** Name which check actually ran (build passed, patch reconstructed, in-game behaviour seen, console output read) and claim only that. A newer change is unverified until a check that exercises it runs.

**Faulty pattern it replaces.** Treating an older script's pass as covering a newer runtime change, because both live under the word 'tests'.

**Applies when.** Reporting status after any change to code, data or environment.
