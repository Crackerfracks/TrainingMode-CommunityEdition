# Incident protocol: how every agent logs a problem

An incident is any problem that cost more than one attempt, any wrong assumption that changed what you did, any failed command you had to work around, any case where you misread what the user wanted, and any safety or permission miss. A typo retried once is not an incident. When unsure, log it: a short entry costs seconds and the weekly review can mark it trivial.

## When to log

At the moment you notice the problem, not after you have fixed it. If you are mid-firefight, log the title and what happened, and come back for the rest. Resolving later is fine; forgetting is what the hooks guard against.

```sh
kb new "<what went wrong, in plain words>" --tags <a,b> --project <name> --size simple|nuanced --impact minor|moderate|major \
   --what "<the observable problem: command, file, message>" \
   --expected "<what you assumed would happen>"
```

Pick `--size nuanced` when the cause is not obvious, when several things interacted, or when you suspect your own reasoning rather than a fact you lacked. The weekly review goes deep on nuanced incidents and stays short on simple ones.

## The reasoning section (the part that matters)

Before the incident can be resolved, fill in "The decision that led here, and my honest reasoning at the time". This is not a confession and not a list of excuses; it is an educated best guess at why the decision felt right when you made it. Examples of honest reasoning:

- "I assumed the flag existed because a similar tool has it, and checking felt like a detour."
- "The earlier script had passed, so I treated the newer runtime change as covered by the same check."
- "I read the user's 'done' as approval of the whole plan, when it only answered the first question."

Write it even when the cause turned out to be a plain fact you lacked. The review is looking for the thought pattern that made the gap invisible, so the pattern, not the fact, is the payload. If you truly cannot write it yet, put `DEFER: <why>` and the hooks keep reminding you.

```sh
kb note INC-20261012-003 --section reasoning "<honest best guess>"
kb resolve INC-20261012-003 --resolution "<what fixed it, or why it stays open>" [--lesson "<one-line correction you would propose>"]
```

`kb resolve` refuses to close an incident without reasoning. Closing someone else's incident needs `--force`, and the review then treats it as unexplained.

## What the hooks do (Claude Code, laptop)

- `SessionStart`: tells the session how many incidents are open or unexplained and lists the top insights.
- `Stop`: if this session logged an incident without reasoning, blocks the stop once with a reminder. Add the reasoning or a `DEFER:` and stop again.
- After compaction: re-injects the pointers, since path-scoped rules were summarised away.

Codex and other agents have no hooks; their global instructions carry the same rule, and `kb todo` at the start of a session shows what they owe.

## Cloud threads

Cloud project threads use the copy at `/mnt/project-files/kb` with the same commands (`python3 /mnt/project-files/kb/tools/kb ...`). Until the store is a git repository with a remote that both sides can reach, laptop and cloud incidents live in two copies; `kb sync` merges them once the remote exists.

## Tags, projects, agents

Tags come from `conventions/tags.md`; add a tag there when a new area appears. `--project` is the short project name (`tmce`, `jobboard`, `kb`, `laptop`). `--agent` and `--model` are filled from the environment when known (`KB_AGENT`, `KB_MODEL`); pass them when you know better. `--session` lets the Stop hook find what you opened.
