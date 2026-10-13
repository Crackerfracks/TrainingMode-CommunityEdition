---
name: kb
description: Log a problem you hit (an incident) to the shared knowledge base, add your honest reasoning, resolve it, or look up incidents and insights. Use whenever something cost more than one attempt, an assumption turned out wrong, a command had to be worked around, you misread the user, or a safety/permission step was missed. Also use to navigate ~/kb (indexes first).
argument-hint: [new "title" | note ID | resolve ID | todo | insight list | <question>]
---

# kb: log problems, keep reasoning honest, find what is known

The store is `$KB_ROOT`, `~/kb` on the laptop, `/mnt/project-files/kb` in cloud threads. The CLI is `kb` (or `python3 <root>/tools/kb`). Read `conventions/incident-protocol.md` once if you have not; the short form is below.

## Log a problem the moment you notice it

```sh
kb new "<plain-words title>" --tags <area,kind> --project <name> --size simple|nuanced --impact minor|moderate|major \
   --what "<observable problem: command, file, message>" --expected "<what you assumed>" --session ${CLAUDE_SESSION_ID}
```

Tags: `conventions/tags.md` (areas: coding, build, testing, process, tooling, git, research, design, communication, coordination, data; kinds: assumption, verification, scope, safety, latency, memory). `nuanced` when the cause is not obvious or your own reasoning is suspect.

## Before you resolve: the reasoning

```sh
kb note <ID> --section reasoning "<the decision that caused it and your honest, educated best guess at why it felt right at the time>"
kb resolve <ID> --resolution "<what fixed it, or why it stays as is>" [--lesson "<one-line thinking correction>"] [--status wontfix]
```

`kb resolve` refuses without reasoning. If you cannot write it yet: `kb note <ID> --section reasoning "DEFER: <why>"`; the Stop hook keeps reminding you. Closing another agent's incident: `--force`, and say so in the resolution.

## Keep it honest and useful

- The reasoning is the payload. The review looks for the thought pattern that made the gap invisible, not the fact you lacked. "I assumed X because Y, and checking felt like a detour" is the shape.
- Log at discovery; fill in later. A title plus `--what` now beats a perfect entry never.
- One incident per cause. A second symptom of the same cause is a `--append` note, not a new incident. A repeat of an already-resolved incident is a new incident that cites the old id in `--what`.
- Do not log in prose to the user instead of the store; the user reads the weekly report, not your chat.

## Look things up

```sh
kb todo                          # open incidents and ones missing reasoning (yours: --session ID)
kb list --open | --week 2026-W42 | --tag testing | --project tmce | --json
kb show <ID>
kb insight list [--tag coding]   # the scored thinking corrections; full entries in insights/<domain>.md
rg -n "<term>" "$(kb where)"     # when the indexes do not name it; then fix the index
```

Navigation: `INDEX.md` at the root, then the area index, then the file. Never bulk-read the store.
