# Context layout: the standard for any project

Goal: an agent loads a small, always-correct core, then pulls in exactly the context a task needs as the task specialises, and can still search everything when the indexes fail it. Three tiers, all plain files.

## Tier 0: always loaded (keep it under about 60 lines, hard limit 200)

- `CLAUDE.md` at the project root: what the project is in two lines, where the index is, the five commands that matter, the hard rules that must survive compaction. Nothing procedural (that is a skill) and nothing area-specific (that is a rule).
- `@`-import only small files that must be in every session. In the global `~/.claude/CLAUDE.md` that is `~/kb/insights/INDEX.md` (the weighted insight list, capped at 60 lines).
- `AGENTS.md`: the same content for Codex and other agents. Codex concatenates `~/.codex/AGENTS.md` and every `AGENTS.md` from the repo root down to the working directory, 32 KiB total, so keep each short. Claude Code reads `AGENTS.md` only when no `CLAUDE.md` exists, so keep the two in step or make one import the other.

## Tier 1: loaded when the task touches matching files

- `.claude/rules/<area>.md` with `paths:` frontmatter (globs). Claude Code loads the rule the first time it reads, writes or edits a matching file. This is the "filepath trigger": a task that starts in `src/landinglab/` gets the Landing Lab rules without anyone asking.
- A nested `CLAUDE.md` inside a subsystem directory loads the same way. Use it for a subsystem large enough to need its own map.
- Skills (`SKILL.md`) carry procedures. Only name and description are loaded at start; the body loads on use. A skill can also carry `paths:` to be offered automatically.
- Domain insight files are wired this way too: `~/.claude/rules/kb-coding.md` matches code files and imports `~/kb/insights/coding.md`.
- Caveat: after context compaction, path-scoped rules and nested CLAUDE.md are summarised away until the task touches a matching file again. Put anything that must persist in the root `CLAUDE.md`. The kb `SessionStart` hook (matcher `compact`) re-injects pointers.

## Tier 2: loaded on demand by reading

- Every directory with more than a handful of files has an `INDEX.md`: one line per entry, each line a path plus a one-line summary. Indexes under about 60 entries; split the directory when it grows past that.
- Every document starts with a one or two line summary so an agent can stop after the first screen.
- Generated indexes (kb) are never hand-edited. Hand-written indexes are updated by whoever adds a file.
- Memory (Claude's auto memory, the project's MEMORY.md) is scratch for the agent's own notes. Anything durable goes into a documented file that the index names.

## How an agent navigates

1. Read the root `INDEX.md` (or CLAUDE.md) of the project, then the area index, then the file. Stop as soon as the question is answered.
2. If the index does not name what you need, search: `rg -n "term" <root>`. Misfiled things exist; broad searches are allowed and expected.
3. When you find something the index should have named, fix the index in the same change.
4. Never bulk-read a directory, never read a whole file to answer a question the summary line answers, never load a procedure you are not about to run.
5. When a task specialises (moves into a subsystem), expect the rule for that subsystem to arrive; if it does not, read the subsystem's INDEX.md yourself.

## Budgets

| Thing | Budget |
|---|---|
| Root CLAUDE.md / AGENTS.md | under 60 lines; 200 is the documented ceiling |
| Always-on imports (insights index) | under 60 lines total |
| A rule file | under 40 lines; longer means it is really a document the rule should point to |
| An INDEX.md | under 60 entries |
| A skill body | under 500 lines; put reference material in sibling files |

## Adopting it in a project

Copy `conventions/project-template/` into the repo, fill the blanks, add `.claude/rules/<area>.md` for each subsystem that has its own rules, and give `docs/` an `INDEX.md`. Keep the first version small; the indexes grow with the project.
