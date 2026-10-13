# Insights: process

Processes, services, sessions, timers, background jobs and hand-offs between them. Entries are maintained with `kb insight ...`; the weekly RCA adds evidence. Weight = incidents backing it.

## I-0003 Report the state of what you can observe, never a proxy for it
- tags: process,coordination,assumption
- weight: 2
- status: active
- added: 2026-W42
- evidence: seed: RC idle-out mixups 2026-10-12
- history: 2026-W42 add

**Correction.** When you describe a remote session, job or device, say what its last report said and when. If you have no report, say that. A status flag or a quiet channel is not evidence of idle, done or stuck.

**Faulty pattern it replaces.** Inferring 'the laptop is idle' or 'the job finished' from the absence of messages, then acting or reporting on the inference.

**Applies when.** Any cross-session, cross-machine or background-job status.
