# 12 — Dead code and over-engineered surface cleanup

## Goal

Delete code that exists but has no role in the current architecture, and
pare back premature abstractions that are costing more than they earn.

Every one of these items was flagged during the technical assessment as
"not pulling its weight." Cut them before the provider-pure refactor so
the refactor has less surface to touch.

## Targets

### Delete outright

1. **`signal_event` and `signal_type`** in `core/event.h`.
   - The strategy pipeline emits `order_event` directly; no code constructs
     or consumes a `signal_event`.
   - Verify with `rg 'signal_event|signal_type' BacktestEngine/src`. If
     anything surfaces, it is tests or serialisation plumbing and is also
     dead.

2. **`ExchangeAdapter` stub** in `execution/execution_adapter.h`
   (lines ~175-191).
   - Two `TODO`s, no real implementation, not referenced anywhere.
   - `BinanceExecutor` is the real live-exchange adapter; this stub
     confuses the story.

3. **Empty provider directories** `providers/metatrader/` and
   `providers/polymarket/`.
   - CLAUDE.md claims they contain README stubs. They do not.
   - Delete until the respective implementation task
     ([05-metatrader-provider.md](05-metatrader-provider.md),
     [06-polymarket-provider.md](06-polymarket-provider.md)) starts.

4. **`MarketMakerWorker`** (`threading/market_maker_worker.h`) and its
   ring / wiring in the `extended` preset.
   - An engine-owned market maker that "replenishes" orders is speculative;
     real market making belongs inside a strategy.
   - Removing this collapses `extended` preset into `full`.

### Simplify

5. **Thread presets** in `threading/thread_preset.h`.
   - Five presets (inline / light / standard / full / extended) are
     over-engineered for the actual concurrency payoff.
   - Collapse to two: `inline` and `threaded`. `threaded` uses three
     workers: logging, risk, stats. No combined / observer variants.
   - Update `select_preset()`, remove `ObserverWorker` and
     `RiskStatsWorker`.

6. **`CopyTracker` debug mixin** on event classes in `core/event.h`
   (guarded by `#ifdef HAS_DEBUG`).
   - Creates a second ABI for the same type. Use external profiling
     (perf, flamegraph, heaptrack) instead. Remove the mixin and every
     `debug::CopyTracker<T>` base class.

7. **CLI parsing "save-then-restore" overlay** in `main.cpp`.
   - Currently: parse CLI → save every flag to a local variable → load
     JSON config (which overwrites everything) → restore flags that were
     set on CLI. This is forty lines of mechanical code, one flag at a
     time.
   - Replace with: load JSON file first into a populated
     `engine_config`, then apply only CLI flags that were set on top.
     CLI11 gives `option->count() > 0` per flag.
   - Resolves after [09-config-schema.md](09-config-schema.md) lands.

8. **`binary_cache_source` decorator** — review usage. If SQLite/Postgres
   backends supersede CSV for tenant data, `binary_cache_source` is a
   redundant intermediate layer. If no live code paths depend on it
   outside tests, delete the class and its tests.

### Hide / de-expose

9. **`market_aggression`, `qty_scale`, `fill_rng_seed`, `spread_step_factor`
   CLI flags.**
   - Users cannot calibrate these meaningfully. Keep the struct fields in
     `engine_config` (useful for programmatic callers) but remove the CLI
     flags.
   - Document the defaults in `docs/execution-parameters.md`.

10. **`ws_compress` CLI flag.**
    - Per-message deflate is an optimisation, not a user choice. Leave the
      server-side negotiation in place; remove the flag. If profiling
      later proves compression hurts, disable it unconditionally.

## Instructions

Do each target as a separate commit with a clear message. Example:

```
cleanup(core): remove unused signal_event

signal_event and signal_type were defined in event.h but no pipeline
code constructs or consumes them. Strategies emit order_event directly.
```

After each commit: `cmake --build build && ctest`. No single target may
break the build or regression tests.

## Acceptance criteria

- `rg 'signal_event|signal_type|ExchangeAdapter|MarketMakerWorker|CopyTracker' BacktestEngine/src | wc -l` returns 0.
- Thread preset enum has two members.
- `engine.cpp` shrinks proportionally (bonus, not required).
- Golden regression tests pass unchanged.
- `./truetest --help` no longer lists the hidden flags.

## Out of scope

- Renaming or moving files for aesthetic reasons.
- Removing `binary_cache_source` if it is used by any current provider
  (check before deleting).
- Removing `CheckpointStore` (K3 feature) — that is still useful for live
  mode. Only its use in backtest runs can be scoped down later.
