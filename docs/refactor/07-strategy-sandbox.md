# 07 — Strategy sandbox

## Goal

Allow **user-supplied strategies** to run inside TrueTest without recompiling
the engine. This is the single most important SaaS-enabling change after
the provider-pure refactor: without it, every tenant requires a build.

## Context

- Today, strategies live in `BacktestEngine/src/strategy/` and are compiled
  into the binary. `StrategyRegistry` (strategy_registry.h) supports
  name-based lookup but only for built-in strategies.
- The `IStrategy` interface (`strategy/strategy_interface.h`) is already
  minimal: `on_market`, `on_tick`, `on_l2_update`, `set_position_open`,
  `set_param`, `get_param_schema`, `get_indicator_values`. Good starting
  point.
- Three viable approaches:
  1. **Plugin ABI** — user compiles an `.so` / `.dll` exporting a C factory.
     Fastest, but requires users to have a C++ toolchain and matching ABI.
  2. **Embedded script runtime** — Lua (tiny, fast) or Python (via pybind11).
     Python is what quants expect.
  3. **WASM** — sandboxed, language-agnostic, slower. Good for untrusted
     multi-tenant. Higher implementation cost.

Recommendation: ship (1) and (2). Defer (3) until SaaS scale demands
stronger isolation.

## Instructions

### Part A — Plugin ABI

1. **Define a stable C ABI** in `BacktestEngine/src/api/strategy_plugin.h`:

   ```c
   #define TT_STRATEGY_ABI_VERSION 1

   typedef struct tt_market_bar {
       int64_t ts_us;
       const char* symbol;
       double open, high, low, close;
       int64_t volume;
   } tt_market_bar;

   typedef struct tt_order_request {
       int32_t side;      // 0=buy 1=sell
       int32_t type;      // 0=market 1=limit 2=stop 3=stop_limit
       double quantity;
       double price;
       double stop_price;
       int32_t tif;
   } tt_order_request;

   typedef void* tt_strategy_handle;
   typedef tt_strategy_handle (*tt_create_fn)(const char* params_json);
   typedef void (*tt_destroy_fn)(tt_strategy_handle);
   typedef int  (*tt_on_bar_fn)(tt_strategy_handle, const tt_market_bar*, tt_order_request* out_order);

   typedef struct {
       int abi_version;
       tt_create_fn create;
       tt_destroy_fn destroy;
       tt_on_bar_fn on_bar;
   } tt_strategy_vtable;

   // Every plugin exports this symbol:
   const tt_strategy_vtable* tt_strategy_vtable_v1(void);
   ```

2. **Create `strategy/plugin_strategy.h`** — a `PluginStrategy : IStrategy`
   that wraps a `dlopen`ed library and delegates `on_market` to `on_bar`.

3. **Registry extension**: `StrategyRegistry::load_plugin(path)` attempts
   `dlopen`, fetches the vtable, validates `abi_version`, registers under
   the plugin-declared name.

4. **CLI flag** `--strategy-plugin <path>` in `main.cpp` / `engine_config`.

5. **Example plugin** `examples/strategies/plugin_sma/` with its own
   `CMakeLists.txt` producing `libplugin_sma.so`. Prove the end-to-end
   flow in a test.

### Part B — Python embedding

1. **CMake flag** `ENABLE_PYTHON_STRATEGY=ON` pulls `pybind11` via
   FetchContent. This is the only track where a Python dep is acceptable.

2. **`strategy/python_strategy.h/.cpp`** uses pybind11 to load a Python
   class implementing the same interface methods. The class is discovered
   via `--strategy-python <module:ClassName>` on the CLI.

3. **GIL**: only one Python strategy runs at a time per engine. If the
   preset uses worker threads, Python strategy invocations happen on the
   engine main thread only — never in workers.

4. **Param passing**: the same JSON used in `StrategyRegistry::create`
   goes to `__init__(self, **params)`.

5. **Example**: `examples/strategies/python/mean_reversion.py`.

### Part C — Sandbox safeguards

1. **CPU budget**: each `on_market` call is wall-clock-timed. If it
   exceeds `cfg.strategy_max_ns` (default 5ms), emit a `status::error`
   event and skip the rest of the bar.
2. **Exception containment**: strategy exceptions never propagate into
   the engine loop. Count them against a `max_consecutive_strategy_errors`
   limit (same pattern as `Worker`).
3. **Resource cap**: for the Python track, set `sys.setrecursionlimit`
   low and refuse imports of `os`, `subprocess`, `socket` via a custom
   import hook when running multi-tenant (gate behind a
   `--strategy-sandbox=strict` flag).

## Acceptance criteria

- Plugin ABI test: load `libplugin_sma.so`, run a backtest, compare output
  to the built-in SMA strategy (should match bit-for-bit).
- Python embedding test: equivalent Python implementation of SMA produces
  the same result within floating-point tolerance.
- Strategy throwing an exception does not kill the engine; it halts that
  strategy only.
- CPU budget violation is observed and surfaced as a status event.

## Out of scope

- WASM runtime integration (defer until multi-tenant isolation is required).
- Strategy hot-reload (unload / reload at runtime).
- Code signing / verification of third-party plugins.
