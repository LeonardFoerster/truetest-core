# TrueTest — Next Steps

Instructions for Claude Code: execute each step in order. After each step,
build with `cmake -B build -DBUILD_TESTS=ON && cmake --build build` and run
`timeout 300 ./build/truetest_tests` to verify nothing is broken. Update
`done.md` with what changed after each step.

---

## Step 2 — Provider Infrastructure Tests

**Goal:** Add unit test coverage for the provider architecture introduced in
improvements A–E. No production code changes — tests only.

### Reference files before starting:
- `providers/transport.h` — `IDataTransport` with streaming methods
- `providers/data_bridge.h` — `DataBridge<T>` with batch + streaming modes
- `providers/provider_event.h` — `provider::event` variant
- `providers/provider_convert.h` — bar/tick conversion functions
- `providers/provider_sink.h` — `provider::event_sink()`
- `providers/provider.h` — `IProvider` interface
- `providers/provider_registry.h` — `ProviderRegistry` singleton
- `providers/local/local_provider.h` — `LocalProvider`
- `providers/local/file_transport.h` — `FileTransport`

### 2-1 — Create `tests/test_provider_transport.cpp`

Test the streaming transport contract:

```cpp
// MockStreamingTransport: backed by a std::queue + mutex + condition_variable.
// - open() sets is_open_ = true
// - read_line_blocking() blocks on the CV until data is enqueued or stopped
// - request_stop() sets stopped_ = true, notifies CV
// - is_streaming() returns true
//
// Tests:
// - FileTransport::is_streaming() returns false
// - FileTransport::read_line_blocking() returns same as read_line() (batch fallback)
// - FileTransport::read_line_blocking() returns nullopt at EOF
// - MockStreamingTransport::is_streaming() returns true
// - MockStreamingTransport::read_line_blocking() blocks until data arrives
// - MockStreamingTransport::request_stop() unblocks read_line_blocking(), returns nullopt
// - MockStreamingTransport::read_line_blocking() returns data in order
```

Use `std::thread` to test blocking behavior: start `read_line_blocking()` on a
background thread, sleep 50ms, enqueue data, join thread, verify result.

For `request_stop()` test: start `read_line_blocking()` on a background thread,
sleep 50ms, call `request_stop()`, join thread, verify nullopt returned.

### 2-2 — Create `tests/test_data_bridge.cpp`

Test both batch and streaming DataBridge paths:

```cpp
// MockTransport: a non-streaming transport backed by a vector of lines.
// read_line() returns lines in order, then nullopt.
//
// Tests:
// - DataBridge batch: load 5 bar records, verify data_handler has 5 entries
// - DataBridge batch: empty transport returns false from load_data()
// - DataBridge batch: malformed lines are skipped, count reflects only valid records
// - DataBridge streaming: MockStreamingTransport delivers 10 records with 10ms delays.
//   Call run_streaming() on a thread. Verify all 10 arrive in handler.
// - DataBridge streaming: start run_streaming() on a thread, call bridge.stop()
//   from main thread, verify run_streaming() returns promptly (< 500ms).
// - DataBridge streaming: on_record callback fires for each parsed record
```

For the streaming tests, use the same `MockStreamingTransport` from 2-1
(put it in a shared test helper header `tests/test_helpers/mock_transport.h`).

### 2-3 — Create `tests/test_provider_event.cpp`

Test the provider event type system:

```cpp
// Tests:
// - provider::bar → bar_record round-trip: all 7 fields preserved
// - provider::tick → tick_record round-trip: all 5 fields preserved
// - tick side values: bid=0, ask=1, unknown=2 map correctly to data_tick_side
// - provider::event_sink with bar: data_handler->db_data_close_value populated
// - provider::event_sink with tick: data_handler->tick_data populated
// - provider::event_sink with status: no crash, data_handler unchanged
// - provider::event_sink with l2_snapshot: no crash, data_handler unchanged
// - provider::event variant holds each type correctly (std::get succeeds)
```

### 2-4 — Create `tests/test_provider_registry.cpp`

Test the registry and LocalProvider:

```cpp
// Tests:
// - ProviderRegistry::create() throws for unregistered name
// - ProviderRegistry::has() returns false for unregistered name
// - Register a mock provider, has() returns true
// - Register a mock provider, create() returns correct instance
// - ProviderRegistry::available() includes registered names
// - LocalProvider registered as "local" (via static init in local_register.cpp)
// - LocalProvider::create with {"path": "market_data.csv"} succeeds
// - LocalProvider::has_data_feed() returns true
// - LocalProvider::has_execution() returns false
// - LocalProvider::get_execution_adapter() returns nullptr
// - LocalProvider::get_transport() returns non-null after open()
// - LocalProvider::create without "path" throws
```

Use `TEST_FIXTURES_DIR` macro (defined in CMakeLists.txt) to locate test data
files. Create `tests/fixtures/test_bars.csv` with 5 rows of OHLCV data if it
doesn't exist.

### 2-5 — Update CMakeLists.txt

Add the new test files to `TEST_SOURCES`:
```cmake
tests/test_provider_transport.cpp
tests/test_data_bridge.cpp
tests/test_provider_event.cpp
tests/test_provider_registry.cpp
```

### Conventions
- Follow the existing test file naming pattern (`test_*.cpp`)
- Use the existing test style: each test file uses `TEST(SuiteName, TestName)`
- Look at `tests/test_engine.cpp` or `tests/test_data_handler.cpp` for examples
- Put the MockStreamingTransport in `tests/test_helpers/mock_transport.h` so
  both transport and bridge tests can share it
- Keep tests fast — use short sleeps (10-50ms) for threading tests

---

## Step 3 — Engine Streaming Mode

**Goal:** Enable the engine to process records as they arrive from a streaming
DataBridge, instead of requiring all data to be pre-loaded. This is the core
architectural change needed for live providers.

### Reference files before starting:
- `core/engine.h/.cpp` — engine class with `run()` and `run_tick_data()`
- `core/engine_config.h` — `engine_mode` enum, provider field
- `providers/data_bridge.h` — `DataBridge::run_streaming()` with `record_callback`
- `core/event.h` — event types
- `data/data_handler.h` — `data_handler` with bar/tick storage

### 3-1 — Add `run_streaming()` to engine

**Modify `core/engine.h`:**

Add a new public method:
```cpp
// Run engine in streaming mode. The bridge feeds records one at a time
// via its record_callback. The engine processes each record immediately.
// Returns when the bridge's transport closes or stop() is called.
void run_streaming(std::shared_ptr<DataBridge<tick_record>> bridge);
void run_streaming(std::shared_ptr<DataBridge<bar_record>> bridge);
```

**Modify `core/engine.cpp`:**

Implement `run_streaming()` for both bar and tick bridges. The key difference
from `run()` is:
- Do NOT iterate over pre-loaded data
- Instead, pass a `record_callback` to `bridge->run_streaming()` that:
  1. Appends the record to `data_handler` (bar: `load_into_queue()`, tick: `tick_data.push_back()`)
  2. Creates a `market_event` from the record
  3. Pushes it through the strategy → order → fill pipeline (same as the inner loop of `run()`)
  4. Publishes events to worker rings
- Call `start_workers()` before entering the streaming loop
- Call `stop_workers()` when `run_streaming()` returns
- The `record_callback` runs on the bridge thread, so access to engine state
  must be safe. Since the engine's hot path is single-threaded by design
  (Core 0), and the bridge runs on the same thread when using `run_streaming()`
  directly, this is safe without locking.

**Important:** Extract the inner event-processing logic from `run()` into a
shared helper method (e.g. `process_bar(size_t index)` or
`process_market_event(market_event&)`) so both `run()` and `run_streaming()`
use the same pipeline. Do NOT duplicate the strategy/order/fill logic.

### 3-2 — Wire streaming mode in main.cpp

**Modify `main.cpp`:**

In the `--provider` path, detect streaming transports and use the streaming
engine path:

```cpp
// After creating the provider and bridge:
if (transport->is_streaming())
{
    engine eng(dh, nullptr, prov_strategy, std::move(prov_cfg));
    eng.run_streaming(bridge);  // blocks until transport closes
    eng.print_summary();
}
else
{
    // existing batch path: bridge->load_data(dh), then eng.run()
}
```

The batch path remains unchanged. Streaming is only used when the transport
reports `is_streaming() == true`.

### 3-3 — Add streaming engine tests

**Create `tests/test_engine_streaming.cpp`:**

Use `MockStreamingTransport` to feed records into the engine one at a time.
Verify:
- Engine processes each record as it arrives (portfolio updates after each tick)
- Strategy generates signals during streaming
- `stop()` on the bridge causes `run_streaming()` to return
- Analytics report reflects all streamed records
- Worker threads receive events during streaming (if threading enabled)

### Conventions
- Extract, don't duplicate — the event processing pipeline must be shared
  between batch and streaming
- Keep the streaming path single-threaded on the hot side (same as batch)
- The bridge thread IS the engine thread in streaming mode — no cross-thread
  complexity
- Do NOT change the batch path behavior — `run()` and `run_tick_data()` must
  remain identical

---

## Step 4 — WebSocket Output (Web Interface Streaming)

**Goal:** Add a WebSocket server that streams engine events to a browser for
real-time visualization. This is the output side — the engine pushes events
(fills, portfolio snapshots, analytics) to connected WebSocket clients.

### Architecture

```
Engine hot path → event_pointer → WebSocket worker ring → WS Worker thread
                                                              ↓
                                                      WebSocket server
                                                              ↓
                                                      Browser clients
```

The WebSocket worker is a new worker thread (like LoggingWorker, StatsWorker)
that consumes events from a ring buffer and broadcasts them as JSON to all
connected WebSocket clients.

### Dependencies

This requires a WebSocket server library. Options:
1. **Boost.Beast** — already gated behind `ENABLE_LIVE_DATA` in CMakeLists.txt.
   Use this for consistency.
2. **uWebSockets** — lighter, faster, but adds a new dependency.
3. **Simple custom implementation** — too much work for no benefit.

Use Boost.Beast. Gate behind `ENABLE_WEB_UI` CMake flag.

### 4-1 — Create the WebSocket worker

**Create `BacktestEngine/src/threading/ws_worker.h`:**

```cpp
// WebSocketWorker: consumes events from a ring buffer, serializes them
// to JSON, and broadcasts to all connected WebSocket clients.
//
// Runs a Boost.Asio io_context on its thread. Accepts connections on
// a configurable port (default 8765).
//
// JSON format for each event:
// {
//   "type": "fill"|"market"|"order"|"portfolio"|"analytics",
//   "timestamp": <epoch_ms>,
//   "data": { ... event-specific fields ... }
// }
```

The worker should:
- Inherit from the same pattern as `LoggingWorker` (consume from ring,
  process events)
- Run a `boost::asio::io_context` for the WS server
- Accept WebSocket upgrade requests
- Maintain a set of active sessions
- On each event from the ring: serialize to JSON, broadcast to all sessions
- Handle disconnects gracefully (remove from session set)

**Create `BacktestEngine/src/threading/ws_worker.cpp`:**

Implementation using `boost::beast::websocket::stream`.

### 4-2 — Add JSON serialization for events

**Create `BacktestEngine/src/core/event_json.h`:**

Inline functions to convert engine events to JSON strings. Use a simple
approach — no dependency on nlohmann/json. Just string concatenation or
`snprintf` into a buffer:

```cpp
std::string to_json(const market_event& e);
std::string to_json(const order_event& e);
std::string to_json(const fill_event& e);
// Portfolio snapshot: { equity, cash, positions: [...] }
std::string portfolio_to_json(const portfolio& p);
// Analytics snapshot: { sharpe, drawdown, win_rate, ... }
std::string analytics_to_json(const Analytics& a);
```

Keep the JSON format simple and flat. The browser client will parse it.

### 4-3 — Wire into engine

**Modify `core/engine_config.h`:**
- Add `uint16_t ws_port = 8765;` field
- Add `bool enable_web_ui = false;` field

**Modify `core/engine.h`:**
- Add `ws_ring_` and `ws_worker_` members (gated behind `#ifdef HAS_WEB_UI`)

**Modify `core/engine.cpp`:**
- In `start_workers()`: create the WS ring and worker when `config_.enable_web_ui`
- In `publish_event()`: push to `ws_ring_` alongside other rings
- In `stop_workers()`: stop the WS worker

**Modify `main.cpp`:**
- Add `--web-ui` CLI flag that sets `config.enable_web_ui = true`
- Add `--ws-port <N>` CLI flag

### 4-4 — CMake integration

**Modify `CMakeLists.txt`:**

```cmake
option(ENABLE_WEB_UI "Build with WebSocket UI server (requires Boost.Beast)" OFF)

if(ENABLE_WEB_UI)
    find_package(Boost REQUIRED COMPONENTS system)
    target_sources(truetest PRIVATE BacktestEngine/src/threading/ws_worker.cpp)
    target_link_libraries(truetest PRIVATE Boost::system)
    target_compile_definitions(truetest PRIVATE HAS_WEB_UI)
endif()
```

Note: `ENABLE_LIVE_DATA` already uses Boost. If both are on, share the
`find_package`. Use an `if(NOT Boost_FOUND)` guard or just let CMake
deduplicate.

### 4-5 — Minimal browser client (optional)

**Create `web/index.html`:**

A single-file HTML/JS page that:
- Connects to `ws://localhost:8765`
- Displays a live equity curve (canvas or simple table)
- Shows recent fills in a scrolling list
- Shows current portfolio positions
- Reconnects on disconnect

This is optional — the WebSocket server is the deliverable, not the frontend.
But a basic client makes it testable.

### Tests
- Unit test: serialize each event type to JSON, parse it back, verify fields
- Integration test: start engine with `enable_web_ui = true`, connect a
  WebSocket client, feed one market event, verify JSON arrives
- Test graceful shutdown: start streaming, stop engine, verify WS server closes

### Conventions
- Gate everything behind `#ifdef HAS_WEB_UI` / `ENABLE_WEB_UI`
- The WS worker follows the same pattern as existing workers
- JSON serialization is in `core/event_json.h`, NOT in the worker
- No external JSON library — keep it simple with string formatting
- The web UI is a dev/monitoring tool, not a production trading interface

---

## Step 5 — Binance Provider

**Goal:** Implement the first live exchange provider: Binance spot market data
via WebSocket + REST execution. This validates the entire provider architecture
end-to-end with a real exchange.

### Dependencies

- Boost.Beast for WebSocket client (already available via `ENABLE_LIVE_DATA`)
- OpenSSL for TLS (Binance requires wss://)
- nlohmann/json or manual JSON parsing for Binance messages

Gate behind `ENABLE_BINANCE` CMake flag.

### 5-1 — Create Binance transport

**Create `providers/binance/binance_transport.h/.cpp`:**

```cpp
// BinanceTransport: WebSocket client connecting to Binance's stream API.
//
// Endpoint: wss://stream.binance.com:9443/ws/<streamName>
// Stream names: <symbol>@trade, <symbol>@kline_<interval>, <symbol>@depth
//
// - open() establishes the TLS WebSocket connection
// - is_streaming() returns true
// - read_line_blocking() blocks on ws.read(), returns the JSON message
// - request_stop() closes the WebSocket (unblocks read)
// - close() tears down the connection
//
// Configuration: symbol, stream type (trade/kline/depth), via constructor.
```

Implementation:
- Use `boost::beast::websocket::stream<boost::asio::ssl::stream<tcp::socket>>`
- Connect to Binance stream endpoint
- `read_line_blocking()` calls `ws.read(buffer)` which blocks until data
- `request_stop()` sets a flag and calls `ws.close()`
- Handle ping/pong frames (Binance sends pings)
- Handle reconnection on disconnect (exponential backoff, max 5 retries)

### 5-2 — Create Binance parser

**Create `providers/binance/binance_parser.h`:**

```cpp
// BinanceParser: parses Binance WebSocket JSON messages into provider::event.
//
// Trade messages → provider::tick
// Kline messages → provider::bar
// Depth messages → provider::l2_update
//
// Message format (trade):
// {"e":"trade","E":123456789,"s":"BTCUSDT","t":12345,"p":"0.001","q":"100",...}
```

Parse the JSON manually (field extraction with string search) or use
nlohmann/json. Produce `provider::event` variants.

### 5-3 — Create Binance execution adapter

**Create `providers/binance/binance_executor.h/.cpp`:**

```cpp
// BinanceExecutor: submits orders via Binance REST API and polls for fills.
//
// - submit_order() sends POST /api/v3/order
// - poll_fills() checks GET /api/v3/myTrades or uses WebSocket user data stream
//
// Requires API key and secret for authenticated endpoints.
// Uses HMAC-SHA256 for request signing.
```

For initial implementation:
- `submit_order()` logs the order but does NOT send to exchange (safety)
- `poll_fills()` returns false (no real fills)
- Add a `live_trading_enabled` flag that must be explicitly set to actually
  send orders. Default: false (paper mode).

### 5-4 — Create BinanceProvider

**Create `providers/binance/binance_provider.h/.cpp`:**

```cpp
class BinanceProvider : public IProvider
{
    // name() = "binance"
    // has_data_feed() = true
    // has_execution() = true (when API keys provided)
    // open() creates BinanceTransport + BinanceExecutor
    // get_transport() returns the WS transport
    // get_execution_adapter() returns the executor
};
```

### 5-5 — Register and wire

**Create `providers/binance/binance_register.cpp`:**
```cpp
REGISTER_PROVIDER("binance", [](const provider_config& cfg) {
    return std::make_shared<BinanceProvider>(
        cfg.at("symbol"),           // e.g. "btcusdt"
        cfg.at("stream"),           // e.g. "trade" or "kline_1m"
        cfg.count("api_key") ? cfg.at("api_key") : "",
        cfg.count("api_secret") ? cfg.at("api_secret") : ""
    );
});
```

**Modify `CMakeLists.txt`:**
```cmake
option(ENABLE_BINANCE "Build with Binance exchange provider" OFF)

if(ENABLE_BINANCE)
    find_package(Boost REQUIRED COMPONENTS system)
    find_package(OpenSSL REQUIRED)
    target_sources(truetest PRIVATE
        BacktestEngine/src/providers/binance/binance_transport.cpp
        BacktestEngine/src/providers/binance/binance_executor.cpp
        BacktestEngine/src/providers/binance/binance_register.cpp
    )
    target_link_libraries(truetest PRIVATE Boost::system OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(truetest PRIVATE HAS_BINANCE)
endif()
```

### 5-6 — CLI usage

After implementation, the user can run:
```bash
# Stream live Binance trades (data only, no execution)
./truetest --provider binance --symbol btcusdt --stream trade --strategy mean-reversion

# With execution (requires API keys)
./truetest --provider binance --symbol btcusdt --stream trade \
    --api-key <key> --api-secret <secret> --strategy sma
```

Add the corresponding CLI flag parsing in main.cpp's `--provider` path:
forward `--symbol`, `--stream`, `--api-key`, `--api-secret` into the
`provider_config` map.

### Tests
- Unit test: parse sample Binance trade JSON → verify provider::tick fields
- Unit test: parse sample Binance kline JSON → verify provider::bar fields
- Unit test: BinanceProvider registered as "binance" in registry
- Integration test (optional, requires network): connect to Binance testnet
  (`wss://testnet.binance.vision`), receive one trade, verify parsing
- Do NOT write tests that hit the real Binance API with orders

### Conventions
- Gate everything behind `#ifdef HAS_BINANCE` / `ENABLE_BINANCE`
- Execution is OFF by default — paper mode unless explicitly enabled
- Never hardcode API keys — always from config/CLI/environment
- Handle Binance rate limits (1200 requests/minute for REST)
- Handle WebSocket disconnects with automatic reconnection
- All Binance-specific code stays in `providers/binance/`

---

## Implementation Order

Execute sequentially. Each step must build and pass tests before moving on.

| Step | Effort | Dependencies |
|------|--------|-------------|
| 2 — Provider tests | Low | None (tests only) |
| 3 — Engine streaming | High | Step 2 (uses mock transport from tests) |
| 4 — WebSocket output | Medium | Step 3 (needs streaming engine) |
| 5 — Binance provider | High | Steps 3 + 4 (needs streaming + output) |
