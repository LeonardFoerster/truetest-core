# 01 — Provider-pure engine

## Goal

Make the core engine depend on **`IProvider` only**. Remove every
`#ifdef HAS_BINANCE` (and any other concrete provider identifier) from
`engine.h`, `engine.cpp`, and `engine_config.h`. Binance-specific wiring
(hybrid executor, backfill) must move behind the Binance provider.

This is the single highest-leverage refactor: every subsequent task is
cheaper once it lands. Adding a fourth backend (Polymarket) without this
will repeat the leak.

## Context

- `engine.h` lines ~37-40: `#ifdef HAS_BINANCE` pulls in
  `hybrid_executor.h` and `binance_backfill.h`, and declares
  `std::shared_ptr<HybridExecutor> hybrid_exec_` as a member.
- `engine_config.h` has `std::shared_ptr<IProvider> provider;` marked as
  *"scaffolding — full provider-based engine wiring comes later."* Finish that.
- `engine.cpp` contains conditional logic that reaches into Binance-specific
  types. Grep `HAS_BINANCE` inside `BacktestEngine/src/core/` to find all
  call sites.
- `BinanceProvider` (providers/binance/binance_provider.h) already implements
  `IProvider`, but the engine does not consume it via the interface.
- `IExecutionAdapter` (execution/execution_adapter.h) is the correct seam:
  the engine should call `submit_order` / `poll_fills` on it, nothing more.
- `IProvider::get_transport()` returns the data stream; the engine should
  feed it through `DataBridge<T>` exactly like `run_streaming()` already does
  for non-Binance providers.

## Instructions

1. **Audit.** Run `rg 'HAS_BINANCE|hybrid_exec|binance_backfill' BacktestEngine/src/core` and
   list every hit. Each is a leak to be sealed.
2. **Move `HybridExecutor` behind `BinanceProvider`.**
   - Extend `BinanceProvider::get_execution_adapter()` so that when running
     in paper or hybrid mode it returns a `HybridExecutor` constructed
     internally.
   - The caller (engine) sees only `IExecutionAdapter`.
   - Pass the local `orderbook`, fee model, and fill model into the provider
     via a new `configure(...)` method or through the constructor.
3. **Move `binance_backfill` invocation into `BinanceProvider::open()`**.
   The engine must not know backfill exists. Expose backfilled bars through
   the transport's normal read path (prepend them to the stream).
4. **Delete all `#ifdef HAS_BINANCE` from `engine.h`, `engine.cpp`, and
   `engine_config.h`.** The only `HAS_BINANCE` guard that should remain
   in the entire repo is inside the `providers/binance/` directory.
5. **Add an `IProvider::lifecycle_state()` query** (enum: `closed`, `opening`,
   `open`, `error`) so the engine can log connection state without guessing.
6. **Update `engine_config.h`**: remove the "scaffolding" comment from
   the `provider` field. Document that `provider` is the *only* way to
   attach data + execution in live/shadow/paper modes.
7. **Update `main.cpp`** to build the provider via `ProviderRegistry` and
   pass it through `engine_config::provider`. Delete any `BinanceProvider`
   direct construction from the engine-mode codepath.
8. **Write a unit test** `tests/test_provider_engine_wiring.cpp` that:
   - Implements a tiny in-memory `FakeProvider` (data feed emits 3 bars,
     execution adapter echoes every order as an immediate fill).
   - Constructs `engine` with `cfg.provider = fake`.
   - Runs the engine and asserts portfolio state.
   - Passes without any `HAS_*` define being on.

## Acceptance criteria

- `grep -rn HAS_BINANCE BacktestEngine/src/core/ | wc -l` returns `0`.
- `cmake -B build && cmake --build build` succeeds with all `ENABLE_*` flags OFF.
- `cmake -B build -DENABLE_BINANCE=ON && cmake --build build` succeeds.
- All existing tests pass.
- The new `FakeProvider` test passes without any external flag enabled.
- Binance live mode still submits orders end-to-end against the testnet.

## Out of scope

- Do not change the `IExecutionAdapter` interface shape in this task —
  that happens in [03-async-fill-pipeline.md](03-async-fill-pipeline.md).
- Do not introduce a new Polymarket or MetaTrader provider here.
- Do not move `main.cpp` logic around beyond what is required to delete
  the Binance branch; the full split is a separate task
  ([11-split-main-engine.md](11-split-main-engine.md)).
