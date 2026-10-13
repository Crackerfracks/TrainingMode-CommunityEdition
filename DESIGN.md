# Knowledge base, error log and weekly RCA: design

Written 2026-10-12 (evening, Eastern) for Stephen. Status: built and installed on the laptop and in the cloud project; one step needs your hands (section 7). Everything here is a draft you can push back on.

## 1. What you asked for, as requirements

1. Every agent (Claude Code on the laptop, cloud project threads, Codex, anything else) logs every problem it hits, resolved or not, with enough context for a later root-cause analysis, including an honest best-guess explanation of the decision that caused it. The explanation may come after the fix, but agents must be reminded until it is there.
2. At least weekly, autonomously, a detailed but adaptive RCA runs over the log (short for simple misses, deep for nuanced ones) using the delegation protocol.
3. The RCA feeds an ever-growing list of global insights: corrections to the underlying thinking, not situation-specific reminders, tagged so it scales across domains.
4. Storage and navigation follow the "compacted context plus filepath trigger" idea you half-remembered: progressive, as-needed loading, with always-active triggers that expand context as a task specialises.
5. It must not cost money, must squeeze the most out of the Max plan, and must not restrict an agent's resourcefulness (broad searches stay allowed; misfiled things are expected).
6. The same approach becomes the standard for storing and navigating context in any project.

Out of scope here: the job board's model indicator and its looks (the job board thread has those).

## 2. What the research found

**"VIKING" is OpenViking** (ByteDance/Volcengine, github.com/volcengine/OpenViking, about 39k stars, AGPL-3.0, release 0.5.0 on 2026-10-09). It is a "context database" that exposes memory, resources and skills as a virtual filesystem (`viking://...`) and gives every file and directory three tiers: an L0 abstract (about 256 characters), an L1 overview (about 4,000 characters) and L2, the full content, loaded only on demand. Retrieval is intent analysis, vector search over the abstracts, then a directory drill-down. It has an official Claude Code and Codex plugin (hooks inject up to six recalled memories per prompt and commit the session on Stop).

The catch: it is a server, and it needs two models to work, an embedding model and a vision-language model that writes the L0/L1 summaries. The embedding side can run locally (Ollama or a bundled GGUF), the summariser is a paid provider unless you run a local model through LiteLLM, and the docs show no fully offline recipe. Reviews call it promising but unproven, with precision falling off past a few hundred cross-linked files. Verdict: the tiered-loading idea is exactly right; the software is the wrong size for one laptop with no API budget. Plain files reproduce the pattern for free, and Claude Code already ships the loaders.

**The "filepath trigger" is a Claude Code feature.** `.claude/rules/<name>.md` with `paths:` globs loads only when Claude reads, writes or edits a matching file; `~/.claude/rules/` does the same for every project; nested `CLAUDE.md` files load when Claude works in their directory; skills load only their description at start and their body on use, and skills can carry `paths:` too. Auto memory loads the first 200 lines of `MEMORY.md` and nothing else until asked. Two gotchas matter: path-scoped rules are summarised away after context compaction until re-triggered, and routines (cloud scheduled runs) start from a fresh clone with no local files. Sources: code.claude.com/docs/en/memory, /skills, /hooks, /routines.

**How to turn errors into insights.** ExpeL (Zhao et al., 2023) is the closest published recipe: insights are extracted from trajectories with four operations, ADD, UPVOTE, DOWNVOTE and EDIT, each insight carries an importance count (new ones start at 2, confirmations add 1, contradictions subtract 1, removal at 0). Reflexion (verbal self-reflection stored for later attempts) and Agent Workflow Memory (reusable workflows induced from successes) point the same way. Karpathy's "LLM wiki" note (April 2026) is the practitioner version of the storage side: an `index.md` per directory, the model maintaining summaries itself, a lint pass for contradictions and orphans, and his observation that the index alone works to hundreds of pages without embeddings.

**Other free options looked at and set aside for now:** claude-mem (SQLite plus Chroma observer; spends model usage per observation), Beads (a task graph, not a knowledge base), Letta MemFS (same idea as ours, tied to Letta), mem0 (needs an API key by default), qmd (local BM25 plus vectors over Markdown; the right add-on only if the tree outgrows its indexes).

## 3. The design in one picture

```
~/kb  (git; laptop)            /mnt/project-files/kb  (cloud copy)
├── INDEX.md                   start here: one line per area
├── incidents/                 one file per problem, YYYY/INC-YYYYMMDD-NNN-slug.md
│   └── INDEX.md               generated: open first, then awaiting RCA, then unexplained
├── insights/                  the growing list of thinking corrections
│   ├── INDEX.md               generated: top 60 by weight; imported into every session
│   └── core|coding|testing|process|communication|research.md   full entries, path-triggered
├── rca/                       weekly inputs and reports, YYYY-Www.md
├── conventions/               the protocols (incident, RCA, context layout, tags, project template)
├── skills/kb, skills/kb-rca   Claude Code skills (symlinked into ~/.claude/skills)
├── rules/kb-*.md              path-triggered loaders for the domain insight files
└── tools/kb, install.sh       the CLI (Python, standard library only) and the installer
```

### 3.1 Three tiers, mapped to Claude Code's loaders

| Tier | What | How it gets into context | Budget |
|---|---|---|---|
| 0, always | the protocol in six lines, plus `insights/INDEX.md` (top 60 corrections) | a marked block at the end of `~/.claude/CLAUDE.md` with an `@import`; the same block without the import in `~/.codex/AGENTS.md` | under 80 lines in all |
| 1, on touch | domain insights (`insights/coding.md` and friends), subsystem rules | `~/.claude/rules/kb-coding.md` matches code files and imports the domain file; the same for testing, process, docs | one rule under 40 lines, one domain file under about 150 |
| 2, on demand | incidents, reports, conventions, project docs | an agent reads `INDEX.md`, then one file; `rg` when the index is silent | unbounded; never bulk-read |

The `kb` skill is how an agent learns the commands without them sitting in every session: only its one-line description is always loaded.

### 3.2 The incident protocol

An incident is anything that cost more than one attempt, a wrong assumption that changed what the agent did, a failed command it worked around, a misread of what you wanted, or a safety or permission miss. Logged at discovery with `kb new`, which writes a file with frontmatter (id, status, agent, model, session, project, tags, size, impact) and six sections: what happened, what was expected, the decision and the honest reasoning at the time, resolution, candidate lesson, RCA.

The reasoning section is the payload. The protocol text and the skill both say what honest means here: the thought pattern that made the gap invisible, in the shape "I assumed X because Y, and checking felt like a detour", written even when the cause turned out to be a missing fact. `kb resolve` refuses to close an incident without it; `DEFER: <why>` is allowed and keeps the reminders coming; closing someone else's incident needs `--force` and is flagged as unexplained for the review.

`--size simple|nuanced` is the agent's own call on RCA depth; the review can override it.

### 3.3 The reminders (hooks)

Two Claude Code hooks, both running the `kb` script with a 10 second timeout:

- `SessionStart` (startup, resume, compact): injects one paragraph: how many incidents are open or unexplained, the ids, the command to log, the five top insights. After a compaction it adds that path-scoped rules were dropped and the relevant index should be re-read.
- `Stop`: if this session logged an incident whose reasoning is empty, blocks the stop once with the exact command to run, then lets the next stop through (guarded by `stop_hook_active` and a per-session marker, so it can never loop).

Codex has hooks too, but installing one needs your trust-flow click on the laptop, so for now Codex gets the instruction block and `kb todo`. Cloud threads have no hooks; they get the protocol through project memory.

### 3.4 The weekly RCA

A cloud Routine fires into this thread every Sunday at 10:50 pm Eastern (so the report is ready Monday morning), using the `kb-rca` skill. It is adaptive and delegated:

| Step | Model | Output |
|---|---|---|
| sync, lint, `kb rca prepare` (bundles unreviewed incidents, open ones included), cluster by tag and cause, draft one paragraph per simple miss | Haiku 5.5 | clusters, drafts, the list of unexplained incidents |
| full RCA per nuanced incident: timeline, decision points, what was believed versus observable, contributing factors, earliest thinking that would have caught it, a proposed insight operation with quotes | Sonnet 5.5, one per incident or small cluster | analysis per incident |
| read the drafts against the incident text, apply insight operations, write the report, mark incidents reviewed, sync, reply here | the thread itself (Opus or Fable) | `rca/<week>.md`, updated insights, a reply under ten lines |

The report also audits the logging habit itself (missing reasoning, late logging, vague titles) and proposes one change to the protocol each week, so the system corrects its own inputs.

### 3.5 Insights and their scoring

An insight has a correction (what to think instead), the faulty pattern it replaces, when it applies, tags, evidence (incident ids) and a history. The operations are ExpeL's: add (weight 2), upvote (+1), edit (+1, an edit is a confirmation), downvote (-1), retire at 0 or when superseded. The generated `insights/INDEX.md` lists active insights by weight, capped at 60 lines, and is what every session sees; the domain files hold the full entries and load when a task touches matching files.

The guard against drifting into reminders is in the procedure and the lint: if a sentence names a tool, a flag or a project, it is a reminder and belongs in that project's rules or docs; the review says so and moves it. Six seed insights are in now, drawn from this project's memory, each phrased to transfer across domains (for example "Different checks are different claims" and "Re-derive the target of an irreversible step from current state, not from the plan").

### 3.6 The standard for any project (conventions/context-layout.md)

Tier 0: a root `CLAUDE.md` under 60 lines (what it is, where the index is, five commands, hard rules that must survive compaction), importing `AGENTS.md` so Codex sees the same. Tier 1: `.claude/rules/<area>.md` with `paths:` per subsystem, nested `CLAUDE.md` where a subsystem is big enough, skills for procedures. Tier 2: an `INDEX.md` in every directory with more than a handful of files, one line per entry, under 60 entries, split past that; every document opening with a one or two line summary. A navigation rule for agents (index, summary, file; search when silent; fix the index when you find a misfile; never bulk-read) and a budget table. `conventions/project-template/` is the drop-in starter.

Memory (Claude's auto memory, this project's MEMORY.md) stays what it is: the agent's own scratch and hand-off. Durable knowledge goes where an index names it.

## 4. What is loaded when (the token budget)

| Moment | Added to context | Approximate size |
|---|---|---|
| every session start | the kb block in CLAUDE.md plus the insights index | 1.5 to 2.5k tokens, capped by the 60-line index |
| session start hook | one paragraph of counts and ids | under 200 tokens |
| first touch of a code file | `kb-coding` rule plus `insights/coding.md` | 0.5 to 2k tokens, grows with the domain |
| the agent decides to log | the `kb` skill body | about 600 tokens, once per session |
| weekly review | everything, but in delegated agents with fresh contexts | the thread keeps only conclusions |

Compared with the alternative (summaries, hooks that recall memories on every prompt), nothing here runs a model call outside the agents' own turns, and nothing loads without a trigger.

## 5. What is built and where

- `tools/kb` (about 900 lines of readable Python, no dependencies): `new`, `note`, `resolve`, `reopen`, `show`, `list`, `todo`, `insight add|upvote|downvote|edit|retire|list`, `rca prepare|mark`, `index`, `lint`, `sync`, `hook session-start|stop`. Indexes regenerate on every write. Tested end to end in the cloud (refusal without reasoning, the one-time stop block, the compact notice, the weekly bundle, lint clean).
- `tools/install.sh`: idempotent; symlinks the CLI and the two skills (also into `~/.agents/skills` for Codex, the way Omarchy fans out its own skills), copies the four rules, merges the two hooks into `~/.claude/settings.json` with a backup, writes the marked block into `~/.claude/CLAUDE.md` and `~/.codex/AGENTS.md` with backups. Your existing hooks (jevkit, context-nudge, the TM-CE mailbox notifier) are left as they are.
- Conventions, skills, rules, the project template, six seed insights, this document.
- Laptop: `~/kb`, installed by the Remote Control session. Cloud: `/mnt/project-files/kb`, plus a project-memory note so every thread knows the protocol.
- Routine: "Weekly kb RCA", Sundays 10:50 pm Eastern, firing into this thread.

## 6. Who logs how

| Agent | Store | Reminder | Notes |
|---|---|---|---|
| Claude Code on the laptop (and Remote Control sessions) | `~/kb` | both hooks, the CLAUDE.md block, the `kb` skill | path-triggered domain insights work here |
| Codex and other CLIs on the laptop | `~/kb` | the AGENTS.md block, `kb todo` at session start | hooks later, after your trust-flow click |
| cloud project threads | `/mnt/project-files/kb` | project memory entry | no hooks; the coordinator's relays carry the rule |

## 7. Sync, and the one thing that needs your hands

The two copies are the same files but do not yet talk to each other, because the laptop has no GitHub login (no `gh auth`, no accepted SSH key). The cloud can push; the laptop can only fetch public repos. Until that changes, laptop incidents stay on the laptop and the weekly review reads the laptop copy through a Remote Control session, which works but is slow.

The fix is one minute of yours, once: on the laptop run `gh auth login` (browser flow) or add an SSH key to GitHub. Then a private repository `Crackerfracks/kb` becomes the single store, `kb sync` runs after every write, the Routine pulls both sides, and the laptop can also push TM-CE work. I will create the private repository as soon as you say so (decision card in the thread); it stays private because incidents describe your workflow.

Bootstrap detail: the first laptop copy came from a branch of your TM-CE fork (`claude/project-thread-3py1rv`), which is public. It holds only the tooling and the six seed insights, no incidents. It goes away once the private repository exists.

## 8. Risks and open questions (your call where marked)

- **Over- and under-logging.** The threshold ("more than one attempt") is deliberately low; the review marks trivial ones. If the log fills with noise, raise the threshold in `conventions/incident-protocol.md`, not in agents' heads.
- **The Stop block.** It fires at most once per session per incident. If it still annoys, the hook can switch to a non-blocking `additionalContext` note; say the word.
- **Reminders creeping into insights.** Guarded by the procedure and the review's own audit section; the lint checks structure, not wording. The first few weekly reports will show whether the guard holds. Your call: do you want to read and veto insight operations for the first month, or let the review apply them and audit afterwards?
- **Weight inflation.** Upvotes only come from new evidence incidents, so weight tracks real recurrence. Old, never-confirmed insights sink below the 60-line cap on their own.
- **Routine reliability.** A cloud Routine runs without the laptop, but it fires into this thread; if this thread is ever archived, the Routine needs re-pointing. A laptop `systemd` timer running `claude -p` headless (verified working, 8 seconds for a trivial call) is the fallback. Any such laptop job goes through the job board (`jb launch`, headline, `needs_you` only for your-hands items, Eastern times).
- **The job board overlap.** `jb issues`, `ack` and `resolve` already record crashes and hangs with a "cause -> fix" note. A weekly import of resolved job-board issues into incidents is cheap and would catch what agents forget to log; not built yet. Your call.
- **Codex hooks.** Codex supports SessionStart hooks but each needs your trust-flow approval; worth doing once the protocol settles.
- **Adopting the layout in TM-CE.** The fork would get a short root CLAUDE.md, `.claude/rules/landinglab.md` (paths `src/landinglab/**`), `AGENTS.md` brought in line, and a `docs/INDEX.md`. The research thread owns that code, so it should land through that thread (or I open a small PR after you nod).

## 9. Next steps

1. You: GitHub login on the laptop; tap the decision card about the private repository.
2. Me: create the repository, point both copies at it, retire the bootstrap branch, add `kb sync` to the Routine's prologue.
3. First real week of incidents, then the first Sunday review on 2026-10-18 at 10:50 pm Eastern; I will reply here with the verdict.
4. After the first review: decide on the job-board import, Codex hooks, and TM-CE adoption.
