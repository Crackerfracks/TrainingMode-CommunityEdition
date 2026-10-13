# Weekly RCA protocol

Runs once a week without being asked (a cloud Routine wakes the kb thread on Sunday night Eastern). Any agent can also run it by hand: `kb rca prepare`, then follow `skills/kb-rca/SKILL.md`.

## Adaptive depth

- **Simple miss**: one paragraph. Cause in one sentence, the thinking pattern behind it, and which existing insight it confirms (upvote) or whether it needs a new one. No five-whys ceremony for a forgotten flag.
- **Nuanced issue**: full treatment. Timeline, the decision points, the reasoning the agent gave, what information was available at each point, contributing factors (missing fact, wrong model of the system, misread intent, skipped verification, over-trusting a prior result, pressure to finish), and the counterfactual: what thinking would have caught it earliest.
- **Unexplained incidents** (no reasoning, or closed with `--force`): still reviewed; the missing reasoning is itself a finding about the logging habit, and the review says so.
- **Open incidents**: included; a long-open incident is a finding too.

## Delegation (the standard protocol)

| Step | Who | Why |
|---|---|---|
| Prepare the batch, lint, cluster by tag, draft the simple-miss paragraphs | Haiku 5.5 (clerk) | mechanical |
| Full RCA of each nuanced incident | Sonnet 5.5 (analyst), one agent per incident or small cluster | needs reading and judgement, bounded scope |
| Cross-incident synthesis into insight operations, the report's conclusions | the main thread (Opus or Fable) | judgement across the whole week |

Agents get the incident files and the current insight domain files, nothing else. Spot-check their claims against the incident text before applying insight operations.

## Insight operations (ExpeL scoring)

An insight is a correction to the underlying thinking, phrased so it transfers to other domains. It is not a reminder about one tool or one project.

| Bad (reminder) | Good (thinking correction) |
|---|---|
| "Remember that Dolphin needs `--platform=x11` under Xvfb." | "A tool's behaviour under one backend is not evidence for another; treat each backend as a separate claim until observed." |
| "Check the ISO name before deleting." | "Before an irreversible step, re-derive the target from the current state, not from the plan that named it." |

Operations, each recorded in the entry's history with the evidence incident ids:

- `add`: a new pattern, starts at weight 2.
- `upvote`: an incident confirms an existing insight, +1.
- `edit`: the wording improves or generalises, +1 (an edit is a confirmation).
- `downvote`: an incident shows the insight misleads or is too broad, -1. At 0 the insight retires.
- `retire`: superseded or merged; say by what.

Each entry has: the correction (what to think instead), the faulty pattern it replaces, when it applies, tags, evidence. One domain file per area (`core` for cross-domain, then `coding`, `testing`, `communication`, `process`, `research`). The generated `insights/INDEX.md` carries the top 60 by weight and is loaded in every session; domain files load when a task touches matching files.

## The report (`rca/<week>.md`)

1. One-line verdict (counts, the week's theme).
2. Simple misses: one paragraph each.
3. Nuanced: one section each.
4. Insight operations applied, with ids.
5. Logging-habit findings (missing reasoning, late logging, unclear titles) and one suggestion for the protocol itself.
6. Anything that needs Stephen (a decision, a permission, an environment change), or "nothing".

Then `kb rca mark --prepared`, `kb index`, `kb lint`, `kb sync`, and a short reply in the kb thread with the verdict and a link to the report.
