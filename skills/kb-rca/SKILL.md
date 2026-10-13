---
name: kb-rca
description: Run the weekly root-cause review of logged incidents and turn them into scored thinking-process insights. Use when the weekly RCA routine fires, when asked to review incidents, or at the end of a week with many logged problems.
argument-hint: [--week 2026-W42]
---

# kb-rca: the weekly review

Follow `conventions/rca-protocol.md`; this is the procedure. Depth is adaptive: a simple miss gets a paragraph, a nuanced issue gets the full treatment. Delegate per the model protocol and spot-check what comes back.

## 1. Prepare (mechanical: clerk / Haiku)

```sh
kb sync                      # pull what other agents logged (no-op without a remote)
kb lint                      # fix format problems first; note recurring ones for section 5
kb rca prepare [--week W]    # writes rca/<W>.input.md and lists the incident ids
```

Have a Haiku agent read `rca/<W>.input.md` and return: clusters by tag and apparent cause, the list of unexplained incidents (reasoning empty, deferred or forced), and a draft paragraph for each simple miss (cause in one sentence, the thinking pattern, which existing insight it confirms or that none does). Give it `insights/INDEX.md` too.

## 2. Nuanced incidents (analyst / Sonnet, one per incident or small cluster)

Brief each with the incident file(s), the relevant `insights/<domain>.md`, and the questions: timeline and decision points; what the agent believed at each point and what it could have observed; contributing factors (missing fact, wrong model of the system, misread intent, skipped verification, trusting a prior result, pressure to finish); the earliest thinking that would have caught it; and a proposed insight operation (confirm existing id, edit, or new with correction + faulty pattern + applies-when). Ask for the evidence quotes it relied on.

## 3. Synthesis (main thread: Opus or Fable)

Read the drafts against the incident text. Then apply operations, one per command so history stays precise:

```sh
kb insight upvote I-0003 --evidence INC-...,INC-...
kb insight edit I-0003 --correction "..." --why "generalised from testing to any backend"
kb insight add --domain coding --title "..." --tags coding,assumption --correction "..." --pattern "..." --applies "..." --evidence INC-...
kb insight downvote I-0009 --evidence INC-... --why "..."     # when an insight misled
```

An insight is a correction to the underlying thinking that transfers across domains. If the sentence names a tool, a flag or a project, it is a reminder, not an insight; put reminders in the project's rules or docs instead and say so in the report.

## 4. Report, mark, sync

Write `rca/<W>.md` with: one-line verdict; simple misses; nuanced sections; operations applied; logging-habit findings plus one protocol suggestion; what needs Stephen or "nothing". Then:

```sh
kb rca mark --prepared --week W      # stamps rca: W on every reviewed incident, removes the input file
kb index && kb lint && kb sync
```

Reply in the kb thread with the verdict and the report path. Keep it under ten lines; the report carries the detail.
