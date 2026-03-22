# Provider Architecture Improvements — Changes

Executed all five improvements (A–E) from `BacktestEngine/docs/improvements.md`.

---

## Improvement A — Streaming Transport

**Modified:** `BacktestEngine/src/providers/transport.h`

Added three virtual methods with default implementations to `IDataTransport`:
- `is_streaming()` — returns `false` by default (batch mode)
- `read_line_blocking()` — falls back to `read_line()` by default
- `request_stop()` — calls `close()` by default

`FileTransport` inherits the defaults unchanged. No existing code was modified.

---

## Improvement B — Streaming DataBridge

**Modified:** `BacktestEngine/src/providers/data_bridge.h`

Added to `DataBridge<T>`:
- `run_streaming(handler, on_record)` — blocking streaming loop that reads from the transport via `read_line_blocking()`, parses, and feeds records through the sink. Optional per-record callback for real-time consumers.
- `stop()` — calls `transport_->request_stop()` to break the streaming loop.

Existing `load_data()` batch path is unchanged.

---

## Improvement C — Normalized Provider Event Type

**Created:**
- `BacktestEngine/src/providers/provider_event.h` — `provider::event` variant containing `provider::bar`, `provider::tick`, `provider::l2_snapshot`, `provider::l2_update`, `provider::status`
- `BacktestEngine/src/providers/provider_convert.h` — bidirectional conversion between `provider::bar`/`bar_record` and `provider::tick`/`tick_record`
- `BacktestEngine/src/providers/provider_sink.h` — `provider::event_sink()` dispatches `provider::event` to `data_handler` (bars and ticks stored, L2/status ignored for now)

---

## Improvement D — Provider-Owned Execution

**Created:**
- `BacktestEngine/src/providers/provider.h` — `IProvider` interface with `name()`, `has_data_feed()`, `has_execution()`, `open()`, `close()`, `get_transport()`, `get_execution_adapter()`
- `BacktestEngine/src/providers/local/local_provider.h` — `LocalProvider` wrapping `FileTransport` for local file data, no execution (handled by engine's `LocalBookAdapter`)
- `BacktestEngine/src/providers/binance/README.md` — stub
- `BacktestEngine/src/providers/polymarket/README.md` — stub
- `BacktestEngine/src/providers/metatrader/README.md` — stub

**Modified:** `BacktestEngine/src/core/engine_config.h`
- Added forward declaration `class IProvider`
- Added `std::shared_ptr<IProvider> provider` field (scaffolding for future use)

---

## Improvement E — Provider Registry / Factory

**Created:**
- `BacktestEngine/src/providers/provider_registry.h` — `ProviderRegistry` singleton with `register_provider()`, `create()`, `available()`, `has()`, and `REGISTER_PROVIDER` macro for self-registration
- `BacktestEngine/src/providers/local/local_register.cpp` — self-registers `LocalProvider` under the name `"local"`, requires `"path"` in config

**Modified:** `CMakeLists.txt`
- Added `BacktestEngine/src/providers/local/local_register.cpp` to both `SOURCES` and `TEST_SOURCES`

**Modified:** `BacktestEngine/src/main.cpp`
- Added `#include "providers/provider_registry.h"`
- Added `--provider <name>` CLI flag parsing
- Added provider-based path: `--provider local --path <file>` opens the provider, reports capabilities, and exits. Full engine wiring deferred to when providers mature.

---

## Step 1 — Full Provider-to-Engine Wiring

**Modified:** `BacktestEngine/src/main.cpp`

The `--provider` CLI path now runs a complete backtest end-to-end:
1. Creates the provider via `ProviderRegistry`
2. Creates a `DataBridge` with the appropriate parser (`CsvBarParser` or `CsvTickParser`)
3. Loads data through the bridge into `data_handler`
4. Constructs the engine with strategy, fee model, and threading config
5. Runs `engine.run()` (bars) or `engine.run_tick_data()` (ticks)
6. Prints the analytics summary

**New CLI flags:**
- `--format bar|tick` — data format (default: bar)
- `--strategy mean-reversion|sma|ma-crossover` — strategy selection (default: mean-reversion)
- `--sma-period <N>` — SMA period (default: 20)
- `--fee fixed|tiered` — fee model
- `--fee-value <N>` — fixed fee amount
- `--maker-rate <N>` / `--taker-rate <N>` — tiered fee rates

**New includes:** `providers/data_bridge.h`, `providers/local/csv_parser.h`, `providers/local/file_transport.h`

**Example usage:**
```bash
./truetest --provider local --path market_data.csv --strategy sma --sma-period 5
./truetest --provider local --path ticks.csv --format tick --fee tiered --maker-rate 0.001 --taker-rate 0.002
```

---

## Step 2 — Provider Infrastructure Tests

**Created:**
- `tests/test_helpers/mock_transport.h` — `MockStreamingTransport` (queue + condition variable) and `MockBatchTransport` (vector-backed), shared across test files
- `tests/test_provider_transport.cpp` — 10 tests covering `FileTransport` batch fallback, `MockStreamingTransport` blocking/unblocking/ordering, `MockBatchTransport` basics
- `tests/test_data_bridge.cpp` — 8 tests covering batch loading (bars, ticks, empty, malformed, header-only) and streaming mode (record delivery, stop promptness, per-record callback)
- `tests/test_provider_event.cpp` — 11 tests covering bar/tick round-trip conversions, side mapping, variant type holding, event_sink dispatch for all event types
- `tests/test_provider_registry.cpp` — 7 registry tests (registered/unregistered lookup, create, available) + 7 LocalProvider tests (capabilities, transport access, open/close, nonexistent file)

**Modified:** `CMakeLists.txt`
- Added 4 test files to `TEST_SOURCES`
- Added `tests/` to `target_include_directories` for test helpers

**Test count:** 43 new tests, all passing. Total suite: 255 registered tests, 0 failures.

---

## Build & Test

- Default build (no flags): **passes** — all 255 tests pass, 0 failures
- `--provider local --path market_data.csv`: **works** — full backtest runs through provider pipeline
- `--provider local --path market_data.csv --strategy sma --sma-period 5`: **works** — generates trades, equity changes
- No existing behavior changed — TUI path remains the default when `--provider` is not given
