# Guides

Guides are task-oriented compositions of the public API. Start with the
[Getting started](../getting-started/README.md) page if this is your first
application.

- [Robot HAL](../examples/robot-hal.md) — resource ownership boundaries,
  serialized domains, `LatestWins`, `RejectIfBusy`, `RequestContext`, and
  application-owned diagnostics.
- [Production diagnostics](production-diagnostics.md) — the deliberately small
  `stats()` scope, health/readiness guidance, and sampling rules.
- [0.6 → 0.7 migration](../migration/0.6-to-0.7.md) — source-breaking API
  changes and ownership updates.
- [Route options design](../api/route-options-design.md) — how advanced
  scheduling stays behind one explicit options object.

The 0.6 performance and experiment documents remain historical evidence, not
beginner configuration advice. See [0.6 experiments](../EXPERIMENTS_0.6.md)
when you need the rationale for a rejected optimization.
