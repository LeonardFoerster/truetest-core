# A: Adaptive Hybrid Strategy (retired; rebuild requirements)

**See**: docs/todos/00-OVERVIEW.md for full rules, reference format, maintenance, and mapping. High-level also in thin `docs/governance/03-todo.md`. The detailed requirements remain in `docs/reference/06-adaptive-hybrid-strategy.md`. The unsafe prototype source and tests were retired: the strategy is not compiled, registered, or runnable. Items below are rebuild requirements, not operator instructions. MC-05 cannot include this strategy until that rebuild lands.

- **A-01** Design a new fixed-capacity, deterministic implementation with typed inventory/exposure units, complete validator inputs, and no prototype placeholders or process-global mutable state.
- **A-02** Define a real external-signal feed contract and provenance model. Any mock producer or `inject_spike`-style hook must remain test-only and must not be enabled by a production default.
- **A-03** Implement platform-consistent exit intents without replacing the mandatory engine exit policy or pre-trade risk chain.
- **A-04** Prove the documented event flow and validator gates with deterministic backtests, shadow evidence, malformed-input tests, pool-exhaustion tests, and ASAN/TSan runs.
- **A-05** Add the rebuilt strategy to the registry, CLI, golden/MC matrices, and both TUI stacks only when implementation and acceptance evidence land together.
- **A-06** Wire ATR and other regime indicators through typed, startup-validated configuration with zero-allocation event-path tests and measured p99 latency.
- **A-07** Any L2 dispatch or safety-surface touches for adaptive hybrid must carry `LIVE_SAFETY_CCB_APPROVED`.

(See the retained specification in `docs/reference/06-adaptive-hybrid-strategy.md`. A future implementation must add new production code and tests; the former prototype files no longer exist.)

**Last updated**: 2026-08-14 (unsafe prototype retired; items rewritten as new-build acceptance requirements).
