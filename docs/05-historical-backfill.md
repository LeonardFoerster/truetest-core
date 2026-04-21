# Step 5: Historical Data Backfill on Chart Load

## Goal

When a user connects to the web UI, the chart shows only data collected since the engine started (or the last 500 bars from `bar_history_`). This step adds automatic historical backfill via the Binance REST API `/api/v3/klines` endpoint, so the chart loads with immediate context (e.g., the last 500 candles) before live streaming begins.

The existing `BinanceRestClient` in `binance_rest_client.h` already handles HTTPS, TLS, and HMAC signing. The klines endpoint is public (no API key required), so this works even without credentials.

---

## Phase 1: REST Kline Fetcher

### 1.1 Create kline backfill utility

**File:** `BacktestEngine/src/providers/binance/binance_backfill.h` (**NEW**)

A lightweight class that fetches historical klines via REST and returns them as `bar_record` vectors. Uses the existing `BinanceRestClient` for HTTP.

```cpp
#pragma once

#include "binance_rest_client.h"
#include "../../providers/parser.h"
#include "../../data/data_handler.h"

#include <string>
#include <vector>
#include <cstdint>
#include <chrono>

struct backfill_bar {
    int64_t open_time;
    double open, high, low, close;
    double volume;
};

class BinanceBackfill {
public:
    explicit BinanceBackfill(const std::string& host = "api.binance.com",
                             const std::string& port = "443")
        : host_(host), port_(port)
    {}

    /// Fetch up to `count` historical klines for `symbol` at `interval`.
    /// Returns bars oldest-first.
    std::vector<backfill_bar> fetch(
        const std::string& symbol,
        const std::string& interval = "1m",
        int count = 500,
        int64_t end_time_ms = 0) const
    {
        std::vector<backfill_bar> result;

        // Binance allows max 1000 per request
        int remaining = count;
        int64_t end_ms = end_time_ms > 0
            ? end_time_ms
            : std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch()).count();

        while (remaining > 0) {
            int batch = std::min(remaining, 1000);
            auto bars = fetch_batch(symbol, interval, batch, end_ms);
            if (bars.empty()) break;

            // Prepend (bars are oldest-first within batch)
            result.insert(result.begin(), bars.begin(), bars.end());
            remaining -= static_cast<int>(bars.size());

            // Next batch ends before the oldest bar we just got
            end_ms = bars.front().open_time - 1;

            // If we got fewer than requested, no more data available
            if (static_cast<int>(bars.size()) < batch) break;
        }

        return result;
    }

private:
    std::string host_;
    std::string port_;

    std::vector<backfill_bar> fetch_batch(
        const std::string& symbol,
        const std::string& interval,
        int limit,
        int64_t end_time_ms) const
    {
        // Build query string (no signing needed — public endpoint)
        std::string query = "symbol=" + to_upper(symbol)
            + "&interval=" + interval
            + "&limit=" + std::to_string(limit);
        if (end_time_ms > 0) {
            query += "&endTime=" + std::to_string(end_time_ms);
        }

        // Use a simple synchronous HTTPS GET
        namespace beast = boost::beast;
        namespace http = beast::http;
        namespace net = boost::asio;
        namespace ssl = net::ssl;
        using tcp = net::ip::tcp;

        net::io_context ioc;
        ssl::context ctx(ssl::context::tlsv12_client);
        ctx.set_default_verify_paths();

        ssl::stream<tcp::socket> stream(ioc, ctx);
        SSL_set_tlsext_host_name(stream.native_handle(), host_.c_str());

        tcp::resolver resolver(ioc);
        auto results = resolver.resolve(host_, port_);
        net::connect(stream.next_layer(), results);
        stream.handshake(ssl::stream_base::client);

        http::request<http::string_body> req(
            http::verb::get, "/api/v3/klines?" + query, 11);
        req.set(http::field::host, host_);
        req.set(http::field::user_agent, "TrueTest/1.0");

        http::write(stream, req);

        beast::flat_buffer buffer;
        http::response<http::string_body> res;
        http::read(stream, buffer, res);

        // Graceful SSL shutdown (ignore errors — server may close first)
        beast::error_code ec;
        stream.shutdown(ec);

        if (res.result() != http::status::ok) {
            return {};
        }

        return parse_klines_array(res.body());
    }

    /// Parse the JSON array response: [[open_time, "o", "h", "l", "c", "v", ...], ...]
    std::vector<backfill_bar> parse_klines_array(const std::string& body) const
    {
        std::vector<backfill_bar> bars;

        // Minimal JSON array-of-arrays parser (no external library)
        // Each element: [open_time, "open", "high", "low", "close", "volume", ...]
        std::size_t pos = 0;
        while (pos < body.size()) {
            // Find start of inner array
            pos = body.find('[', pos);
            if (pos == std::string::npos) break;
            // Skip the outer array bracket on first iteration
            if (body[pos + 1] == '[') { pos++; continue; }

            std::size_t end = body.find(']', pos);
            if (end == std::string::npos) break;

            std::string element = body.substr(pos + 1, end - pos - 1);
            pos = end + 1;

            backfill_bar bar{};
            if (parse_kline_element(element, bar)) {
                bars.push_back(bar);
            }
        }

        return bars;
    }

    bool parse_kline_element(const std::string& csv, backfill_bar& bar) const
    {
        // Format: open_time,"open","high","low","close","volume",close_time,...
        // Fields are comma-separated, strings are quoted
        std::vector<std::string> fields;
        std::size_t start = 0;
        bool in_quote = false;
        for (std::size_t i = 0; i <= csv.size(); ++i) {
            if (i < csv.size() && csv[i] == '"') { in_quote = !in_quote; continue; }
            if (i == csv.size() || (!in_quote && csv[i] == ',')) {
                std::string f = csv.substr(start, i - start);
                // Strip quotes
                if (f.size() >= 2 && f.front() == '"' && f.back() == '"')
                    f = f.substr(1, f.size() - 2);
                fields.push_back(f);
                start = i + 1;
            }
        }

        if (fields.size() < 6) return false;

        try {
            bar.open_time = std::stoll(fields[0]);
            bar.open      = std::stod(fields[1]);
            bar.high      = std::stod(fields[2]);
            bar.low       = std::stod(fields[3]);
            bar.close     = std::stod(fields[4]);
            bar.volume    = std::stod(fields[5]);
            return true;
        } catch (...) {
            return false;
        }
    }

    static std::string to_upper(std::string s)
    {
        for (auto& c : s) c = static_cast<char>(std::toupper(c));
        return s;
    }
};
```

---

## Phase 2: Wire Backfill into Engine Startup

### 2.1 Add backfill configuration

**File:** `BacktestEngine/src/core/engine_config.h`

Add fields:
```cpp
int backfill_bars = 500;          // Number of historical bars to fetch on start
std::string backfill_interval;    // Kline interval for backfill (default: match stream)
```

### 2.2 Add CLI flags

**File:** `BacktestEngine/src/main.cpp`

Add:
```cpp
{"backfill",       required_argument, nullptr, 0},  // --backfill 500
{"backfill-interval", required_argument, nullptr, 0}, // --backfill-interval 1m
```

Parse:
```cpp
if (opt == "backfill") prov_cfg.backfill_bars = std::stoi(optarg);
if (opt == "backfill-interval") prov_cfg.backfill_interval = optarg;
```

### 2.3 Add to start.sh

**File:** `start.sh`

In the configuration section:
```bash
# ── Historical backfill ──────────────────────────────────────────────
BACKFILL_BARS=500             # Number of historical candles to load on start (0 = disabled)
BACKFILL_INTERVAL=""          # Kline interval for backfill (empty = match stream type)
```

In `build_engine_args()`:
```bash
if [[ "$BACKFILL_BARS" -gt 0 ]]; then
    ENGINE_ARGS+=(--backfill "$BACKFILL_BARS")
fi
if [[ -n "$BACKFILL_INTERVAL" ]]; then
    ENGINE_ARGS+=(--backfill-interval "$BACKFILL_INTERVAL")
fi
```

---

## Phase 3: Fetch and Pre-populate Before Streaming

### 3.1 Backfill in `run_streaming()` before starting the WebSocket stream

**File:** `BacktestEngine/src/core/engine.cpp`

At the top of `run_streaming()`, before `bridge->run_streaming()`:

```cpp
#if defined(HAS_BINANCE) && defined(HAS_WEB_UI)
if (config_.backfill_bars > 0
    && config_.provider && config_.provider->name() == "binance")
{
    // Determine backfill interval from stream type
    std::string interval = config_.backfill_interval;
    if (interval.empty()) {
        // If streaming trades, backfill 1m bars for chart context
        // If streaming kline_1m, backfill 1m
        interval = "1m";
    }

    // Determine REST host (match testnet if configured)
    std::string rest_host = "api.binance.com";
    if (config_.binance_host.find("testnet") != std::string::npos) {
        rest_host = "testnet.binance.vision";
    }

    BinanceBackfill backfill(rest_host);
    std::string symbol = data_handler_->db_data_symbol.empty()
        ? "btcusdt" : data_handler_->db_data_symbol.back();

    std::cerr << "  Backfilling " << config_.backfill_bars
              << " bars for " << symbol << " (" << interval << ")...\n";

    auto bars = backfill.fetch(symbol, interval, config_.backfill_bars);

    if (!bars.empty()) {
        std::cerr << "  Backfill: " << bars.size() << " bars loaded\n";

        // Convert to market events and populate bar_history_ for UI
        for (const auto& b : bars) {
            // Convert epoch ms to seconds for lightweight-charts
            int64_t ts_sec = b.open_time / 1000;

            // Build bar JSON (same format as broadcast_market_with_indicators)
            char buf[512];
            std::snprintf(buf, sizeof(buf),
                R"({"type":"market","data":{"time":%lld,"open":%.8g,"high":%.8g,"low":%.8g,"close":%.8g,"volume":%.2f,"indicators":{}}})",
                static_cast<long long>(ts_sec),
                b.open, b.high, b.low, b.close, b.volume);

            if (bar_history_.size() >= MAX_BAR_HISTORY)
                bar_history_.erase(bar_history_.begin());
            bar_history_.push_back(buf);
        }

        // Broadcast to any already-connected clients
        if (ws_worker_) {
            ws_worker_->broadcast(R"({"type":"chart_reset","data":{}})");
            for (const auto& json : bar_history_) {
                ws_worker_->broadcast(json);
            }
        }
    }
}
#endif
```

### 3.2 Include the backfill header

**File:** `BacktestEngine/src/core/engine.h`

Add under `#ifdef HAS_BINANCE`:
```cpp
#include "../providers/binance/binance_backfill.h"
```

---

## Phase 4: On-Demand Backfill via WS Command

### 4.1 Handle `backfill` command from browser

**File:** `BacktestEngine/src/core/engine.cpp` in `process_ws_commands()`

Allow the UI to request backfill at any time (e.g., after symbol switch):

```cpp
else if (cmd.command == "backfill")
{
    #if defined(HAS_BINANCE) && defined(HAS_WEB_UI)
    int count = cmd.price > 0 ? static_cast<int>(cmd.price) : 500;
    std::string interval = cmd.value.empty() ? "1m" : cmd.value;

    // Run backfill in a detached thread to avoid blocking the command loop
    std::thread([this, count, interval]() {
        std::string rest_host = "api.binance.com";
        BinanceBackfill backfill(rest_host);
        std::string symbol = data_handler_->db_data_symbol.empty()
            ? "btcusdt" : data_handler_->db_data_symbol.back();

        auto bars = backfill.fetch(symbol, interval, count);
        if (bars.empty()) return;

        // Must lock or post to main thread — bar_history_ is not thread-safe
        // Use the WS broadcast queue which is thread-safe
        std::vector<std::string> jsons;
        for (const auto& b : bars) {
            int64_t ts_sec = b.open_time / 1000;
            char buf[512];
            std::snprintf(buf, sizeof(buf),
                R"({"type":"market","data":{"time":%lld,"open":%.8g,"high":%.8g,"low":%.8g,"close":%.8g,"volume":%.2f,"indicators":{}}})",
                static_cast<long long>(ts_sec),
                b.open, b.high, b.low, b.close, b.volume);
            jsons.push_back(buf);
        }

        if (ws_worker_) {
            ws_worker_->broadcast(R"({"type":"chart_reset","data":{}})");
            for (const auto& json : jsons) {
                ws_worker_->broadcast(json);
            }
        }
    }).detach();
    #endif
}
```

### 4.2 Add `value` field extraction for backfill interval

This depends on the `value` field added to `ws_command` in Step 3 (runtime-switching.md). Ensure `cmd.value` is extracted from the JSON message in `on_client_message()`.

---

## Phase 5: Frontend Integration

### 5.1 Handle chart_reset properly for backfill

**File:** `web/src/store/dispatcher.ts`

The `chart_reset` handler should clear bars and reset chart state. This already exists from prior work, but verify it handles the backfill → live transition cleanly:

```typescript
case 'chart_reset': {
    dispatchers.market({ type: 'RESET' });
    break;
}
```

### 5.2 Add backfill status indicator

**File:** `web/src/components/TopBar.tsx` (or appropriate status component)

Show a loading indicator while backfill is in progress:

```tsx
// Listen for chart_reset (backfill starting) → show "Loading history..."
// Listen for first market bar after reset → hide indicator
```

The simplest approach: track a `backfilling` boolean in EngineStore. Set it on `chart_reset`, clear it on the first `market` event after reset.

**File:** `web/src/store/EngineStore.tsx`

```typescript
interface EngineState {
    // ... existing fields ...
    backfilling: boolean;
}

// In reducer:
case 'SET_BACKFILLING':
    return { ...state, backfilling: action.value };
```

**File:** `web/src/store/dispatcher.ts`

```typescript
case 'chart_reset': {
    dispatchers.market({ type: 'RESET' });
    dispatchers.engine({ type: 'SET_BACKFILLING', value: true });
    break;
}

case 'market': {
    // ... existing market handling ...
    if (engineState.backfilling) {
        dispatchers.engine({ type: 'SET_BACKFILLING', value: false });
    }
    break;
}
```

### 5.3 Request backfill on symbol switch

When the user switches symbols (Step 3), the frontend should automatically request backfill for the new symbol. In the `set_symbol` handler in TopBar:

```tsx
function handleSymbolSubmit() {
    if (symbolInput.trim()) {
        send({ command: 'set_symbol', value: symbolInput.trim().toLowerCase() });
        // Backfill is triggered server-side by the symbol switch logic
        // Or explicitly:
        send({ command: 'backfill', price: 500 });
        setSymbolInput('');
    }
}
```

Alternatively, have the engine automatically backfill after a symbol switch (in the `pending_symbol_` handler from Step 3).

---

## Phase 6: Deduplication

### 6.1 Prevent overlap between backfill and live bars

When backfill data overlaps with the start of live streaming, duplicate bars appear. Handle this in the MarketStore reducer:

**File:** `web/src/store/MarketStore.tsx`

In the `ADD_BAR` action, check if the new bar's timestamp matches the last bar:

```typescript
case 'ADD_BAR': {
    const bars = state.bars;
    const newBar = action.bar;

    // Deduplicate: if last bar has same timestamp, update it instead of appending
    if (bars.length > 0 && bars[bars.length - 1].time === newBar.time) {
        const updated = [...bars];
        updated[updated.length - 1] = newBar;
        return { ...state, bars: updated };
    }

    // Skip bars older than the latest (out-of-order from backfill overlap)
    if (bars.length > 0 && newBar.time < bars[bars.length - 1].time) {
        return state;
    }

    const next = [...bars, newBar];
    return { ...state, bars: next.length > MAX_BARS ? next.slice(-MAX_BARS) : next };
}
```

### 6.2 Increase MAX_BAR_HISTORY for backfill

**File:** `BacktestEngine/src/core/engine.h`

When backfill is enabled, the default 500 `MAX_BAR_HISTORY` may not be enough if the user requests more. Increase or make configurable:

```cpp
static constexpr std::size_t MAX_BAR_HISTORY = 1000;
```

---

## Phase 7: CMake Integration

### 7.1 No new dependencies

`BinanceBackfill` uses the same Boost.Beast + OpenSSL that `BinanceTransport` already requires. No new CMake flags needed — it compiles when `HAS_BINANCE` is defined.

### 7.2 Add source file

**File:** `CMakeLists.txt`

Add `binance_backfill.h` to the Binance source list (header-only, but include for IDE discovery):

```cmake
if(ENABLE_BINANCE)
    target_sources(truetest PRIVATE
        # ... existing binance sources ...
        BacktestEngine/src/providers/binance/binance_backfill.h
    )
endif()
```

---

## Testing

1. **Startup backfill:** Start engine with `--backfill 500`, connect browser, verify chart immediately shows 500 historical candles
2. **No backfill:** Start with `--backfill 0`, verify chart starts empty and fills from live stream only
3. **Testnet host:** Start with `--host testnet.binance.vision`, verify backfill uses testnet REST API
4. **Symbol switch + backfill:** Switch from BTCUSDT to ETHUSDT, verify chart resets and loads ETH history
5. **Deduplication:** Verify no duplicate bars at the junction between backfill and live stream
6. **Large backfill:** Request `--backfill 2000`, verify pagination works (2 REST requests of 1000 each)
7. **Rate limit:** Request `--backfill 5000`, verify no 429 errors (each request is 1 weight, limit is 1200/min)
8. **On-demand:** Send `{"command":"backfill","price":200}` via WS, verify chart reloads with 200 bars

---

## Files Changed (Summary)

| File | Change |
|------|--------|
| `BacktestEngine/src/providers/binance/binance_backfill.h` | **NEW** — REST kline fetcher with pagination |
| `BacktestEngine/src/core/engine_config.h` | Add `backfill_bars`, `backfill_interval` fields |
| `BacktestEngine/src/core/engine.h` | Include backfill header, increase `MAX_BAR_HISTORY` |
| `BacktestEngine/src/core/engine.cpp` | Pre-stream backfill, `backfill` WS command handler |
| `BacktestEngine/src/main.cpp` | Add `--backfill`, `--backfill-interval` CLI flags |
| `start.sh` | Add `BACKFILL_BARS`, `BACKFILL_INTERVAL` config variables |
| `CMakeLists.txt` | Add `binance_backfill.h` to Binance sources |
| `web/src/store/MarketStore.tsx` | Deduplication in `ADD_BAR` reducer |
| `web/src/store/EngineStore.tsx` | Add `backfilling` state flag |
| `web/src/store/dispatcher.ts` | Handle backfill status in `chart_reset` |
