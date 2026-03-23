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

## Step 3 — Engine Streaming Mode

**Modified:** `BacktestEngine/src/core/engine.h`

Added:
- `run_streaming(std::shared_ptr<DataBridge<bar_record>>)` — streaming mode for bar data
- `run_streaming(std::shared_ptr<DataBridge<tick_record>>)` — streaming mode for tick data
- `process_order()` — extracted from `run()`'s lambda into a member method (shared pipeline)
- `process_single_bar()` — processes one bar record through strategy → order → fill
- `process_single_tick()` — processes one tick record through strategy → order → fill

**Modified:** `BacktestEngine/src/core/engine.cpp`

- Extracted `process_order` from `run()`'s lambda into a reusable member method
- Added `process_single_bar()` and `process_single_tick()` helper methods
- Implemented `run_streaming()` for both bar and tick bridges:
  - Starts workers, sets up event logging
  - Passes a `record_callback` to `bridge->run_streaming()` that processes each record immediately
  - Prints progress every 200ms during streaming
  - Stops workers and prints summary when bridge returns
- `run()`'s inner order processing now delegates to the member `process_order()`

**Modified:** `BacktestEngine/src/main.cpp`

- Added streaming transport detection in the `--provider` path
- When `transport->is_streaming()` is true, uses `engine::run_streaming(bridge)` instead of batch load + `run()`
- Batch path remains unchanged for non-streaming transports

**Created:** `tests/test_engine_streaming.cpp`

6 tests:
- `BarStreamProcessesAllRecords` — 10 bars streamed, all arrive in data_handler and strategy
- `BarStreamStrategyGeneratesSignals` — strategy generates buy/sell signals during streaming
- `BarStreamStopCausesReturn` — `bridge->stop()` causes `run_streaming()` to return promptly
- `BarStreamBatchTransportFallback` — batch transport works through `run_streaming()` via fallback
- `TickStreamProcessesAllRecords` — 8 ticks streamed, all arrive in data_handler and strategy
- `TickStreamStopCausesReturn` — stop causes prompt return for tick streaming

**Modified:** `CMakeLists.txt`

- Added `tests/test_engine_streaming.cpp` to `TEST_SOURCES`

**Test count:** 6 new tests, all passing. Total suite: 261 registered tests, 0 failures.

---

## Step 4 — WebSocket Output (Web Interface Streaming)

**Created:** `BacktestEngine/src/core/event_json.h`

JSON serialization for all event types using `snprintf` (no external JSON library):
- `event_json::to_json()` for `market_event`, `order_event`, `fill_event`, `tick_event`
- `event_json::portfolio_to_json()` for portfolio snapshots (cash, positions)
- `event_json::analytics_to_json()` for analytics reports (sharpe, drawdown, win rate, etc.)
- `event_json::event_to_json()` generic dispatcher

**Created:** `BacktestEngine/src/threading/ws_worker.h`

WebSocket server worker using Boost.Beast (header-only):
- `WsSession` — per-client WebSocket connection with async read/write
- `WebSocketWorker` — consumes events from ring buffer, broadcasts JSON to all connected clients
- Runs Boost.Asio `io_context` on its own thread for accepting connections
- Handles client disconnect gracefully
- Gated behind `#ifdef HAS_WEB_UI`

**Modified:** `BacktestEngine/src/core/engine_config.h`

Added:
- `bool enable_web_ui = false` — enables WebSocket UI
- `uint16_t ws_port = 8765` — configurable port

**Modified:** `BacktestEngine/src/core/engine.h`

Added (behind `#ifdef HAS_WEB_UI`):
- `ws_ring_` — ring buffer for WS worker
- `ws_worker_` — WebSocket worker instance
- `ws_drops_` — drop counter

**Modified:** `BacktestEngine/src/core/engine.cpp`

- `start_workers()`: creates WS ring + worker when `enable_web_ui` is true (works with any threading preset, including inline)
- `publish_event()`: pushes to `ws_ring_` after all other rings (changed `return` to `break` in switch cases)
- `stop_workers()`: stops WS worker, includes it in failure reporting

**Modified:** `BacktestEngine/src/main.cpp`

Added CLI flags:
- `--web-ui` — enables WebSocket UI
- `--ws-port <N>` — sets WebSocket port (default 8765)

**Modified:** `CMakeLists.txt`

Added:
- `option(ENABLE_WEB_UI ...)` with `find_package(Boost REQUIRED)` (header-only)
- `HAS_WEB_UI` compile definition

**Created:** `web/index.html`

Single-file browser dashboard:
- Connects to `ws://localhost:8765`
- Live equity curve (canvas)
- Recent fills list
- Portfolio positions table
- Analytics metrics panel
- Auto-reconnect on disconnect

**Created:** `tests/test_event_json.cpp`

8 tests covering:
- Market, order, fill, tick event JSON serialization
- Portfolio and analytics JSON serialization
- Event dispatcher for all types
- Unhandled event returns empty string

**Test count:** 8 new tests, all passing. Total suite: 269 registered tests, 0 failures.

Builds verified:
- Without `ENABLE_WEB_UI`: 269 tests pass
- With `ENABLE_WEB_UI=ON`: 269 tests pass (requires Boost headers)

---

## Step 5 — Binance Provider

**Created:** `BacktestEngine/src/providers/binance/binance_transport.h`

WebSocket client for Binance stream API using Boost.Beast over TLS:
- Connects to `wss://stream.binance.com:9443/ws/<symbol>@<stream_type>`
- `is_streaming()` returns true
- `read_line_blocking()` blocks on `ws.read()`, returns JSON messages
- `request_stop()` closes the WebSocket
- Automatic reconnection with exponential backoff (max 5 retries)
- Configurable host/port for testnet usage
- Gated behind `#ifdef HAS_BINANCE`

**Created:** `BacktestEngine/src/providers/binance/binance_parser.h`

Parses Binance WebSocket JSON messages (no external JSON library):
- `binance::parse_trade()` — trade messages → `tick_record`
- `binance::parse_kline()` — kline messages → `bar_record`
- Helper functions: `extract_string()`, `extract_number()`, `extract_bool()`
- `BinanceTradeParser` — `IDataParser<tick_record>` adapter
- `BinanceKlineParser` — `IDataParser<bar_record>` adapter
- NOT gated behind `#ifdef` — pure C++ string parsing, always available for testing

**Created:** `BacktestEngine/src/providers/binance/binance_executor.h`

Execution adapter for Binance (paper mode by default):
- Paper mode: logs orders, simulates fills for market orders at last known price
- `set_live_trading(true)` enables live mode (requires API keys, not yet implemented)
- REST API order submission and fill polling are stubs for future implementation
- Gated behind `#ifdef HAS_BINANCE`

**Created:** `BacktestEngine/src/providers/binance/binance_provider.h`

`BinanceProvider : public IProvider`:
- `name()` = "binance"
- `has_data_feed()` = true
- `has_execution()` = true when API keys provided
- `open()` creates `BinanceTransport` + optionally `BinanceExecutor`
- Gated behind `#ifdef HAS_BINANCE`

**Created:** `BacktestEngine/src/providers/binance/binance_register.cpp`

Self-registers "binance" provider. Required config:
- `symbol` (required, e.g. "btcusdt")
- `stream` (optional, default "trade")
- `api_key`, `api_secret` (optional, enables execution)
- `host`, `port` (optional, for testnet)

**Modified:** `BacktestEngine/src/main.cpp`

Added CLI flags: `--symbol`, `--stream`, `--api-key`, `--api-secret`, `--host`, `--port`.
All forwarded into `provider_config`. Made `--path` optional (not needed for binance).

**Modified:** `CMakeLists.txt`

Added `ENABLE_BINANCE` option:
- `find_package(Boost REQUIRED)` + `find_package(OpenSSL REQUIRED)`
- Links `Boost::headers`, `OpenSSL::SSL`, `OpenSSL::Crypto`
- Defines `HAS_BINANCE`

**Created:** `tests/test_binance_parser.cpp`

13 tests covering:
- JSON field extraction (string, number, quoted number, bool)
- Trade message parsing (buy/sell aggressor, timestamp, missing fields, wrong type)
- Kline message parsing (OHLCV fields, missing k object, wrong type)
- IDataParser adapter round-trip for both trade and kline parsers

**Test count:** 13 new tests, all passing. Total suite: 282 registered tests, 0 failures.

**CLI usage:**
```bash
# Stream live Binance trades (data only, requires ENABLE_BINANCE=ON build)
./truetest --provider binance --symbol btcusdt --stream trade --strategy mean-reversion

# With execution (requires API keys, paper mode by default)
./truetest --provider binance --symbol btcusdt --stream trade \
    --api-key <key> --api-secret <secret> --strategy sma

# Testnet
./truetest --provider binance --symbol btcusdt --stream trade \
    --host testnet.binance.vision --port 9443
```

---

## Build & Test

- Default build (no flags): **passes** — all 282 tests pass, 0 failures
- With `ENABLE_WEB_UI=ON`: **passes** — all 282 tests pass
- With `ENABLE_BINANCE=ON`: **passes** — all 282 tests pass
- `--provider local --path market_data.csv`: **works** — full backtest runs through provider pipeline
- `--provider local --path market_data.csv --strategy sma --sma-period 5`: **works** — generates trades, equity changes
- No existing behavior changed — TUI path remains the default when `--provider` is not given
