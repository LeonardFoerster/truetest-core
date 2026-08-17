# Adaptive Hybrid — retired prototype

**Status (2026-08): unavailable.** The former `adaptive-hybrid` prototype is not
compiled, registered, or accepted by the CLI. Its source and dedicated test
harness were removed rather than presenting an unsafe demo as a supported
strategy. Do not use the old commands, configuration files, headers, or factory
examples from historical revisions.

The prototype was retired because it did not meet current engine contracts:

- inventory units and reconciliation state were incomplete;
- configuration and validation inputs were not propagated consistently;
- process-global/static state made independent instances nondeterministic;
- dynamic containers and other allocation risks existed on the event path;
- the mock on-chain producer and concurrency model lacked lifetime and TSan
  evidence;
- risk, L2, and safety behavior was described more strongly than it was
  implemented.

## Rebuild contract

A future implementation is a new strategy, not a restoration of checkpointed
prototype state. Before it may be registered it must provide all of the
following:

1. Typed, reconciled inventory and exposure units with per-lot attribution.
2. Complete startup validation for every configuration input and no silent
   defaults that weaken risk limits.
3. Per-instance deterministic state and seeds; no shared mutable strategy
   globals between engines or Monte Carlo trials.
4. Fixed-capacity, prewarmed event-path storage with runtime growth forbidden.
5. A single-producer/single-consumer ownership proof for any auxiliary feed;
   prompt, race-free teardown; ASAN and TSan evidence.
6. Real L2 and external-signal provenance. Mock producers must remain test-only.
7. Mandatory platform exit policy and engine risk checks; strategy intents may
   refine protection but cannot replace it.
8. Characterization, deterministic golden, pool-exhaustion, malformed-config,
   and multi-instance tests plus measured p99 latency/allocation evidence.
9. CLI, registry, TUI/desk, strategy-development, and Monte Carlo documentation
   updated only after the implementation and gates land together.

Work is tracked in [07-A-adaptive-hybrid.md](../todos/07-A-adaptive-hybrid.md).
Any future L2 or live-safety surface change remains subject to the freeze and
`LIVE_SAFETY_CCB_APPROVED` process.
