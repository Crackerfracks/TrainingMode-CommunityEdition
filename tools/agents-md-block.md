<!-- kb:start -->
# Shared knowledge base (kb)

Every problem that costs more than one attempt, every wrong assumption, every workaround and every misread of the user is logged with the `kb` CLI (`~/kb/tools/kb`, on PATH as `kb`): `kb new "title" --tags a,b --agent codex --what "..."` the moment you notice it, then `kb note ID --section reasoning "<honest best guess at why the decision felt right at the time>"` and `kb resolve ID --resolution "..."`. Full protocol: `~/kb/conventions/incident-protocol.md`. At the start of a session run `kb todo` and read `~/kb/insights/INDEX.md` (weighted thinking corrections from past incidents); read `~/kb/insights/<domain>.md` when a task enters that area.

Navigating any project: index first (`INDEX.md`, `AGENTS.md`), then the one-line summary, then the file. Search when the index is silent, then fix the index. Never bulk-read a directory. Standard: `~/kb/conventions/context-layout.md`.
<!-- kb:end -->
