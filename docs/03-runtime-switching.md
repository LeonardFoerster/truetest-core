# Step 3: Runtime Symbol & Strategy Switching

## Goal

Allow users to change the trading symbol (e.g., BTCUSDT to ETHUSDT) and the active strategy (e.g., mean-reversion to SMA) from the web UI without restarting the engine. This requires a strategy factory, Binance transport reconnection, and new WS commands.

---

## Phase 1: Strategy Factory

### 1.1 Create strategy factory

**File:** `BacktestEngine/src/strategy/strategy_factory.h` (**NEW**)

```cpp
#pragma once

#include "strategy_interface.h"
#include "mean_reversion_strategy.h"
#include "sma_strategy.h"
#include "ma_crossover_strategy.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <functional>

struct strategy_params {
    std::size_t sma_period = 20;
    double balance = 10000.0;
    double risk_fraction = 0.02;
    double sl_pct = 0.005;
    double tp_pct = 0.01;
};

class StrategyFactory {
public:
    static std::shared_ptr<IStrategy> create(
        const std::string& name, const strategy_params& params = {})
    {
        if (name == "sma")
            return std::make_shared<sma_strategy>(params.sma_period);
        if (name == "ma-crossover")
            return std::make_shared<ma_crossover_strategy>(params.sma_period);
        // Default: mean-reversion
        return std::make_shared<mean_reversion_strategy>(
            params.sma_period, params.balance, params.risk_fraction,
            params.sl_pct, params.tp_pct);
    }

    static std::vector<std::string> available() {
        return {"mean-reversion", "sma", "ma-crossover"};
    }
};
```

### 1.2 Add strategy setter to engine

**File:** `BacktestEngine/src/core/engine.h`

Add public method:
```cpp
void set_strategy(std::shared_ptr<IStrategy> strategy);
```

**File:** `BacktestEngine/src/core/engine.cpp`

```cpp
void engine::set_strategy(std::shared_ptr<IStrategy> strategy)
{
    if (!strategy) return;
    strategy_ = std::move(strategy);
    // Transfer current position state to new strategy
    for (const auto& [symbol, pos] : portfolio_.get_positions()) {
        strategy_->set_position_open(symbol, pos.qty > 0.0);
    }
}
```

---

## Phase 2: Symbol Switching (Binance Transport Reconnection)

### 2.1 Add reconnect-with-new-stream to BinanceTransport

**File:** `BacktestEngine/src/providers/binance/binance_transport.h`

Add public method:
```cpp
bool reconnect_stream(const std::string& new_symbol,
                      const std::string& new_stream_type)
{
    // Close existing connection
    request_stop();

    // Wait for close to complete
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Update stream parameters
    symbol_ = new_symbol;
    stream_type_ = new_stream_type;

    // Reset state
    stopped_ = false;
    reconnect_attempts_ = 0;

    // Reconnect
    ioc_.restart();
    ws_.reset();
    return open();
}
```

### 2.2 Add symbol change to engine

**File:** `BacktestEngine/src/core/engine.h`

Add public method:
```cpp
#ifdef HAS_BINANCE
void switch_symbol(const std::string& new_symbol);
#endif
```

**File:** `BacktestEngine/src/core/engine.cpp`

```cpp
#ifdef HAS_BINANCE
void engine::switch_symbol(const std::string& new_symbol)
{
    // 1. Close open positions for old symbol (optional — could also carry over)
    // 2. Reset bar aggregator
    #ifdef HAS_WEB_UI
    if (tick_aggregator_) {
        tick_aggregator_->flush();
        tick_aggregator_ = std::make_unique<BarAggregator>(
            tick_bar_interval_,
            [this](const market_event& bar) {
                broadcast_market_with_indicators(bar);
            });
    }
    bar_history_.clear();
    #endif

    // 3. Update data handler symbol
    data_handler_->db_data_symbol.clear();
    data_handler_->db_data_symbol.push_back(new_symbol);

    // 4. Notify strategy of new symbol context
    strategy_->set_position_open(new_symbol, false);

    // 5. Broadcast chart reset to UI
    #ifdef HAS_WEB_UI
    if (ws_worker_) {
        ws_worker_->broadcast(R"({"type":"chart_reset","data":{}})");
        ws_worker_->broadcast_status("running",
            config_.provider ? config_.provider->name() : "",
            new_symbol);
    }
    #endif
}
#endif
```

Note: The actual Binance transport reconnection happens in `run_streaming()` — since the transport's `read_line_blocking()` call blocks the streaming thread, the symbol switch must be coordinated. The cleanest approach is:

- The WS command sets a `pending_symbol_switch_` atomic string
- The streaming loop checks this flag after each tick/bar
- When set, the transport is reconnected inline in the streaming thread

### 2.3 Add pending switch mechanism

**File:** `BacktestEngine/src/core/engine.h`

```cpp
#ifdef HAS_BINANCE
std::mutex switch_mu_;
std::string pending_symbol_;
std::string pending_strategy_;
#endif
```

**File:** `BacktestEngine/src/core/engine.cpp` in `run_streaming()` (tick variant)

Inside the `bridge->run_streaming()` callback, after processing each tick:

```cpp
#ifdef HAS_BINANCE
{
    std::lock_guard<std::mutex> lk(switch_mu_);
    if (!pending_symbol_.empty()) {
        std::string new_sym = std::move(pending_symbol_);
        pending_symbol_.clear();

        // Reconnect transport
        auto* binance_transport = dynamic_cast<BinanceTransport*>(
            transport.get());
        if (binance_transport) {
            switch_symbol(new_sym);
            binance_transport->reconnect_stream(new_sym, cli_stream);
            // The next read_line_blocking() will read from the new stream
        }
    }
    if (!pending_strategy_.empty()) {
        std::string new_strat = std::move(pending_strategy_);
        pending_strategy_.clear();
        strategy_params params;
        params.sma_period = cli_sma_period;
        params.balance = config_.initial_balance;
        set_strategy(StrategyFactory::create(new_strat, params));
    }
}
#endif
```

---

## Phase 3: New WS Commands

### 3.1 Handle `set_symbol` and `set_strategy` in `process_ws_commands()`

**File:** `BacktestEngine/src/core/engine.cpp`

Add to the command handler switch:

```cpp
else if (cmd.command == "set_symbol" && !cmd.timeframe.empty())
{
    // Reuse timeframe field for the symbol string (avoid adding new ws_command fields)
    // Or better: add a "value" field to ws_command
    std::string new_symbol = cmd.timeframe;
    // Convert to lowercase (Binance requires lowercase)
    std::transform(new_symbol.begin(), new_symbol.end(),
                   new_symbol.begin(), ::tolower);

    std::lock_guard<std::mutex> lk(switch_mu_);
    pending_symbol_ = new_symbol;

    ws_worker_->broadcast_status("switching",
        config_.provider ? config_.provider->name() : "", new_symbol);
}
else if (cmd.command == "set_strategy" && !cmd.timeframe.empty())
{
    std::string new_strategy = cmd.timeframe;

    // Validate
    auto available = StrategyFactory::available();
    bool valid = std::find(available.begin(), available.end(), new_strategy)
                 != available.end();
    if (!valid) {
        ws_worker_->broadcast(event_json::error_to_json(
            "Unknown strategy: " + new_strategy));
        return;
    }

    std::lock_guard<std::mutex> lk(switch_mu_);
    pending_strategy_ = new_strategy;

    ws_worker_->broadcast_status("running",
        new_strategy,
        (!data_handler_->db_data_symbol.empty())
            ? data_handler_->db_data_symbol.back() : "");
}
```

### 3.2 Extend `ws_command` struct

**File:** `BacktestEngine/src/threading/ws_worker.h`

Add a generic `value` field to the command struct:
```cpp
struct ws_command {
    std::string command;
    std::string side;
    double quantity = 0.0;
    double price = 0.0;
    std::string order_type;
    std::string timeframe;
    std::string value;       // NEW: generic value for set_symbol, set_strategy
};
```

Update `on_client_message()` to extract it:
```cpp
cmd.value = extract("value");
```

Then use `cmd.value` instead of `cmd.timeframe` in the `set_symbol`/`set_strategy` handlers.

---

## Phase 4: Frontend Controls

### 4.1 Add symbol selector to TopBar

**File:** `web/src/components/TopBar.tsx`

Add an input field or dropdown next to the existing symbol display:

```tsx
const [symbolInput, setSymbolInput] = useState('');
const { send } = useWebSocket();

function handleSymbolSubmit() {
    if (symbolInput.trim()) {
        send({ command: 'set_symbol', value: symbolInput.trim().toLowerCase() });
        setSymbolInput('');
    }
}
```

Render as a small text input with an "Apply" button or Enter-to-submit, placed next to the current symbol badge. Show the current symbol from `engine.symbol`.

Common presets as quick-select buttons: `BTCUSDT`, `ETHUSDT`, `SOLUSDT`, `BNBUSDT`.

### 4.2 Add strategy selector to TopBar

**File:** `web/src/components/TopBar.tsx`

Add a dropdown/select for strategy:

```tsx
const strategies = ['mean-reversion', 'sma', 'ma-crossover'];

function handleStrategyChange(e: React.ChangeEvent<HTMLSelectElement>) {
    send({ command: 'set_strategy', value: e.target.value });
}
```

Render as a `<select>` element styled to match the dark theme, next to the current strategy display.

### 4.3 Broadcast available strategies on connect

In `send_state_snapshot()`, broadcast the list of available strategies:

```cpp
// Available strategies list
{
    auto names = StrategyFactory::available();
    std::string json = R"({"type":"strategies","data":[)";
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i > 0) json += ",";
        json += "\"" + names[i] + "\"";
    }
    json += "]}";
    ws_worker_->broadcast(json);
}
```

Handle in `dispatcher.ts`:
```typescript
case 'strategies': {
    dispatchers.engine({
        type: 'SET_AVAILABLE_STRATEGIES',
        strategies: msg.data as string[],
    });
    break;
}
```

Add to EngineStore:
```typescript
availableStrategies: string[]  // populated from 'strategies' message
```

---

## Testing

1. **Strategy switch:** Select "sma" from dropdown while running, verify status updates to "sma", new trades use SMA logic
2. **Symbol switch:** Enter "ethusdt" in symbol input, verify chart resets, new candles are ETH prices
3. **Invalid strategy:** Send `set_strategy` with "nonexistent", verify error toast
4. **Invalid symbol:** Send `set_symbol` with "xxxyyy", verify Binance transport reports connection error
5. **Position carryover:** Switch strategy while holding a position, verify new strategy knows about open position
6. **Concurrent safety:** Switch symbol and strategy simultaneously, verify no crash (mutex protects both)

---

## Files Changed (Summary)

| File | Change |
|------|--------|
| `BacktestEngine/src/strategy/strategy_factory.h` | **NEW** — Strategy factory with available() list |
| `BacktestEngine/src/core/engine.h` | Add `set_strategy()`, `switch_symbol()`, pending switch fields |
| `BacktestEngine/src/core/engine.cpp` | Implement switching, handle WS commands, broadcast strategies |
| `BacktestEngine/src/providers/binance/binance_transport.h` | Add `reconnect_stream()` method |
| `BacktestEngine/src/threading/ws_worker.h` | Add `value` field to `ws_command` |
| `web/src/components/TopBar.tsx` | Add symbol input, strategy dropdown |
| `web/src/store/dispatcher.ts` | Handle `strategies` message |
| `web/src/store/EngineStore.tsx` | Add `availableStrategies`, `SET_AVAILABLE_STRATEGIES` |
