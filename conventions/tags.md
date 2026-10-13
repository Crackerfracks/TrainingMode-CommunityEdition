# Tag registry

Use existing tags first; add a line here before using a new one. Tags describe the kind of failure or area, not the project (that is `--project`).

## Areas
- `coding`: writing or changing code, build errors, wrong API use.
- `build`: compilers, toolchains, ISO/patch builds, Docker, nix.
- `testing`: test design, test harnesses, emulator runs, captures, false passes.
- `process`: process management, services, timers, background jobs, hangs, idle-outs.
- `tooling`: CLIs, scripts, hooks, environment, PATH, permissions.
- `git`: branches, merges, pushes, PRs, history.
- `research`: facts looked up, sources, docs, measurements.
- `design`: UI/UX or feature design judgement.
- `communication`: misread intent, wrong audience, missing or noisy updates, wrong channel.
- `coordination`: hand-offs between agents or sessions, stale state, duplicated work.
- `data`: file formats, parsing, tables, captures as data.

## Failure kinds
- `assumption`: acted on an unverified belief.
- `verification`: skipped or trusted a weaker check.
- `scope`: did more, less, or something other than asked.
- `safety`: touched something that should have been protected, or needed a permission.
- `latency`: slow turnaround, waiting on the wrong thing.
- `memory`: lost or stale context, forgot an instruction, compaction loss.
