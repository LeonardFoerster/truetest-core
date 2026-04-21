# TrueTest — SaaS Refactor Plan

This folder contains **self-contained implementation instructions** for turning the
current backtesting-plus-Binance engine into a true multi-backend platform.
Every file is written to be handed directly to Claude Code.

Each task file follows the same structure:

1. **Goal** — what this change achieves and why it matters
2. **Context** — the files, classes, and current state relevant to the change
3. **Instructions** — numbered, ordered steps Claude Code should execute
4. **Acceptance criteria** — how to know the change is complete and correct
5. **Out of scope** — what *not* to touch while doing this task

## Recommended execution order

The dependency chain matters. Earlier items unlock later ones.

### Phase 1 — Modularity foundation
1. [01-provider-pure-engine.md](01-provider-pure-engine.md) — remove concrete provider knowledge from the engine
2. [12-dead-code-cleanup.md](12-dead-code-cleanup.md) — delete `signal_event`, `ExchangeAdapter`, etc.
3. [11-split-main-engine.md](11-split-main-engine.md) — break up `main.cpp` and `engine.cpp`

### Phase 2 — Domain model
4. [02-instrument-and-position-model.md](02-instrument-and-position-model.md) — first-class instruments and generalised positions
5. [10-clock-abstraction.md](10-clock-abstraction.md) — `IClock` interface
6. [13-price-qty-unification.md](13-price-qty-unification.md) — fixed-point end-to-end

### Phase 3 — Execution realism
7. [03-async-fill-pipeline.md](03-async-fill-pipeline.md) — push-based fills
8. [04-binance-userdata-stream.md](04-binance-userdata-stream.md) — replace REST polling
9. [14-paper-mode-realism.md](14-paper-mode-realism.md) — depth-aware paper fills

### Phase 4 — New providers
10. [05-metatrader-provider.md](05-metatrader-provider.md) — MetaTrader EA bridge
11. [06-polymarket-provider.md](06-polymarket-provider.md) — Polymarket AMM / quote adapter

### Phase 5 — SaaS surface
12. [07-strategy-sandbox.md](07-strategy-sandbox.md) — user-supplied strategies
13. [09-config-schema.md](09-config-schema.md) — typed, versioned config
14. [08-rest-control-plane.md](08-rest-control-plane.md) — job queue API

### Keep list
- [15-keep-as-is.md](15-keep-as-is.md) — components that should **not** be refactored, with rationale

## Non-negotiable invariants

Do not violate these while executing any task:

- **Core engine compiles with zero optional dependencies.** All provider-specific
  code lives behind `ENABLE_*` CMake flags.
- **Interfaces are prefixed with `I`.** New interfaces follow the existing style.
- **Every public behaviour change ships with at least one test.** Golden
  regression tests in `tests/golden/` must continue to pass bit-for-bit unless
  the behaviour change is intentional — in which case regenerate them and
  note it in the commit.
- **No new external dependencies without explicit approval.** The only allowed
  additions are things already listed as `FetchContent` in `CMakeLists.txt`
  or standard OS libraries.
- **C++17 only.** No C++20 features.
