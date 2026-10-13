# kb

A plain-file knowledge base shared by every agent Stephen runs: Claude Code on the laptop, cloud project threads, Codex, and anything else that can read Markdown and run a Python script. No server, no embeddings, no paid API. Git carries it between machines.

It holds three things that feed each other:

1. **Incidents.** Every problem an agent hits and resolves (or does not) is logged in its own file with the agent's honest best guess at why the decision that caused it felt right at the time.
2. **Weekly RCA.** A scheduled review reads the week's incidents, goes shallow on simple misses and deep on nuanced ones, and turns them into insights.
3. **Insights.** A growing, scored list of corrections to the underlying thinking (not situation-specific reminders). The top of the list is loaded into every session; the rest loads when a task touches matching files.

The layout follows one convention, described in `conventions/context-layout.md`: every directory has a compact index, every file has a one-line summary, and agents load the index first and the file only when they need it. The same convention is the standard for any project's own context.

## Install (laptop)

```sh
git clone <remote> ~/kb          # or copy the directory to ~/kb
~/kb/tools/install.sh            # symlinks `kb`, installs skills, rules, hooks, CLAUDE.md block
kb lint                          # should report 0 problems
```

`install.sh` is idempotent and backs up anything it edits. It writes:

| Piece | Where |
|---|---|
| CLI | `~/.local/bin/kb` -> `~/kb/tools/kb` |
| Skills | `~/.claude/skills/kb`, `~/.claude/skills/kb-rca` (symlinks); `~/.agents/skills/kb` for Codex if that directory exists |
| Rules | `~/.claude/rules/kb-*.md` (path-triggered loaders for the domain insight files) |
| Hooks | `SessionStart` and `Stop` entries in `~/.claude/settings.json` |
| Instructions | a marked block at the end of `~/.claude/CLAUDE.md` and `~/.codex/AGENTS.md` |

## Daily use

```sh
kb new "Dolphin left hung after headless capture" --tags testing,process --project tmce --size nuanced \
   --what "Every other capture run left a Dolphin process; the next run then failed to grab the window."
kb note INC-20261012-001 --section reasoning "I assumed the AppImage exited with the window; it only does so with X11."
kb resolve INC-20261012-001 --resolution "Kill by cgroup in the runner, not by window handle."
kb todo               # what still needs a hand (open, or missing reasoning)
kb list --week 2026-W42
kb insight list       # the scored list; `kb insight upvote I-0003 --evidence INC-...`
kb rca prepare        # bundle the unreviewed incidents for the weekly review
kb sync               # commit, pull --rebase, push (no-op without a remote)
```

Indexes regenerate on every write; never edit an `INDEX.md` by hand.

## Why files and not a context database

OpenViking (ByteDance's "context database" with `viking://` paths and L0/L1/L2 summaries) is the system Stephen half-remembered. Its tiered-loading idea is right and is reproduced here with plain files and Claude Code's own loaders (CLAUDE.md, path-scoped rules, skills). The software itself needs a server plus an embedding model and a vision-language model to write its summaries, which means either paid API calls or a local model stack. For a store this size, indexes plus `rg` cost nothing and stay readable. Revisit a local search tool (for example qmd) only if the tree outgrows its indexes.
