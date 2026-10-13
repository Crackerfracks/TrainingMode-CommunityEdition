# kb: the shared knowledge base (start here)

One line per area. Read an area's own INDEX.md next, then only the files you need.

- `incidents/INDEX.md`: problems agents hit, open ones first. Log one with `kb new`, close with `kb resolve`.
- `insights/INDEX.md`: the always-on list of thinking corrections, weighted by evidence. Full entries live in `insights/<domain>.md`.
- `rca/INDEX.md`: weekly root-cause reviews, newest first.
- `conventions/INDEX.md`: the protocols. How to log, how to review, how any project lays out its context, the tag registry.
- `skills/`: Claude Code skills (`kb`, `kb-rca`). `rules/`: path-triggered rule files installed into `~/.claude/rules/`. `tools/`: the `kb` CLI, hook entry points, `install.sh`.

Navigation rule: index, then the one-line summary, then the file. If an index does not name what you need, search (`rg -n "term" ~/kb`) and then fix the index so the next agent finds it. Never bulk-read a directory.
