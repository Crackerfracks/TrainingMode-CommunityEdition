# Insights: core

Cross-domain corrections: how to reason, verify and decide, whatever the task. Entries are maintained with `kb insight ...`; the weekly RCA adds evidence. Weight = incidents backing it.

## I-0001 A remembered or inferred fact is a hypothesis until observed this session
- tags: assumption,verification
- weight: 2
- status: active
- added: 2026-W42
- evidence: seed: project memory 2026-10 (blind timed menu presses opened the wrong event)
- history: 2026-W42 add

**Correction.** Before acting on a fact you did not observe in this session (a path exists, a flag works, a file is current, a process exited), spend one cheap check on it. Treat the memory as a lead, not as evidence.

**Faulty pattern it replaces.** Acting on a remembered or similar-tool fact because checking felt like a detour; the detour is cheaper than the rework.

**Applies when.** Any step that builds on a precondition you have not seen with your own tools.

## I-0004 Re-derive the target of an irreversible step from current state, not from the plan
- tags: safety,verification
- weight: 2
- status: active
- added: 2026-W42
- evidence: seed: protected-file rules in project memory
- history: 2026-W42 add

**Correction.** Immediately before deleting, overwriting, killing or sending, look at the actual target (name, path, pid, recipient) as it is now and compare it with what the plan said. The plan was written against an older state.

**Faulty pattern it replaces.** Executing a destructive step from a variable or instruction set earlier in the session, trusting that nothing changed.

**Applies when.** Deletes, force-pushes, process kills, outgoing messages, config edits.
