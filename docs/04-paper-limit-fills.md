# Step 4: Paper Trading Fill Simulation for Limit Orders

## Goal

When running in paper mode against live Binance data, limit orders are silently ignored — only market orders produce simulated fills. This step routes paper-mode limit orders through the existing `LocalBookAdapter` and orderbook matching engine so they receive realistic fills based on incoming market prices, with fill probability modeling and commission calculation.

---

## Phase 1: Hybrid Execution Adapter

### 1.1 Create `HybridExecutor` that combines paper market fills with book-based limit fills

**File:** `BacktestEngine/src/providers/binance/binance_executor.h`

Currently `BinanceExecutor::submit_order()` in paper mode (line ~68) handles market orders only. Instead of expanding that code, create a new adapter that delegates:

- **Market orders** → `BinanceExecutor` (paper mode, uses `last_price_` for instant fill)
- **Limit / stop-limit orders** → `LocalBookAdapter` (orderbook matching with fill probability)

**File:** `BacktestEngine/src/providers/binance/hybrid_executor.h` (**NEW**)

```cpp
#pragma once

#include "../../execution/execution_adapter.h"
#include "binance_executor.h"
#include "../../orderbook/orderbook.h"
#include "../../orderbook/fill_model.h"

#include <memory>
#include <vector>

class HybridExecutor : public IExecutionAdapter {
public:
    HybridExecutor(std::shared_ptr<BinanceExecutor> paper_exec,
                   std::shared_ptr<orderbook> book,
                   std::shared_ptr<IFillModel> fill_model = nullptr)
        : paper_(std::move(paper_exec))
        , book_adapter_(std::make_unique<LocalBookAdapter>(
              std::move(book),
              fill_model ? std::move(fill_model)
                         : std::make_shared<RealisticFillModel>(0.05, 0.8, 5.0)))
    {}

    void submit_order(const order_event& o) override
    {
        if (o.get_order_type() == order_type::market) {
            paper_->submit_order(o);
        } else {
            book_adapter_->submit_order(o);
        }
    }

    bool poll_fills(std::vector<fill_event>& out) override
    {
        bool had_paper = paper_->poll_fills(out);
        bool had_book = book_adapter_->poll_fills(out);
        return had_paper || had_book;
    }

    // Forward mid-price updates to book adapter for fill probability
    void update_mid_price(const std::string& symbol, double mid)
    {
        book_adapter_->update_mid_price(symbol, mid);
    }

    // Forward price to paper executor for market fill simulation
    void update_last_price(double price)
    {
        paper_->update_last_price(price);
    }

private:
    std::shared_ptr<BinanceExecutor> paper_;
    std::unique_ptr<LocalBookAdapter> book_adapter_;
};
```

### 1.2 Expose `update_last_price()` on BinanceExecutor

**File:** `BacktestEngine/src/providers/binance/binance_executor.h`

Add public method (if not already present):
```cpp
void update_last_price(double p) { last_price_ = p; }
```

---

## Phase 2: Seed the Orderbook with Market Data

### 2.1 Add liquidity seeding from incoming ticks/bars

The local orderbook starts empty. For limit order matching to work, it needs liquidity levels around the current price. The engine already has a `MarketMaker` class in `market_maker/market_maker.h` — reuse its logic or add a simpler seeding function.

**File:** `BacktestEngine/src/core/engine.cpp`

In `process_single_tick()` and `process_single_bar()`, after updating the market price, seed the orderbook with synthetic liquidity:

```cpp
#ifdef HAS_BINANCE
if (hybrid_exec_) {
    // Seed 10 levels each side, spread = 0.01% per level
    double mid = bar.close;  // or tick.price
    double spread_step = mid * 0.0001;
    auto& book = orderbook_registry_.get_or_create(symbol);
    book.clear_all();  // Remove stale levels

    for (int i = 1; i <= 10; ++i) {
        double bid_px = mid - i * spread_step;
        double ask_px = mid + i * spread_step;
        int64_t qty = static_cast<int64_t>(1e8);  // 1.0 in scaled units

        book.add_order({
            next_order_id(), orderbook::side_t::buy, bid_px,
            qty, orderbook::order_type_t::good_till_cancel
        });
        book.add_order({
            next_order_id(), orderbook::side_t::sell, ask_px,
            qty, orderbook::order_type_t::good_till_cancel
        });
    }

    hybrid_exec_->update_mid_price(symbol, mid);
    hybrid_exec_->update_last_price(mid);
}
#endif
```

### 2.2 Add `clear_all()` method to orderbook

**File:** `BacktestEngine/src/orderbook/orderbook.h`

Add a method to clear all orders (for re-seeding each tick):
```cpp
void clear_all()
{
    bids_.clear();
    asks_.clear();
    orders_.clear();
    // Reset node pool
    current_block_idx_ = 0;
    next_in_block_ = 0;
}
```

---

## Phase 3: Wire HybridExecutor into the Engine

### 3.1 Add HybridExecutor to engine

**File:** `BacktestEngine/src/core/engine.h`

Add under `#ifdef HAS_BINANCE`:
```cpp
#include "../providers/binance/hybrid_executor.h"
std::shared_ptr<HybridExecutor> hybrid_exec_;
```

### 3.2 Create HybridExecutor in streaming setup

**File:** `BacktestEngine/src/core/engine.cpp`

In `run_streaming()`, when the provider is Binance and mode is not live:

```cpp
#ifdef HAS_BINANCE
if (config_.provider && config_.provider->name() == "binance"
    && config_.engine_mode != engine_mode::live)
{
    auto binance_exec = std::dynamic_pointer_cast<BinanceExecutor>(
        config_.provider->execution_adapter());
    if (binance_exec) {
        auto book = std::make_shared<orderbook>();
        hybrid_exec_ = std::make_shared<HybridExecutor>(
            binance_exec, book);
    }
}
#endif
```

### 3.3 Use HybridExecutor in process_order()

**File:** `BacktestEngine/src/core/engine.cpp` in `process_order()`

Where the execution adapter is selected (around line 635), add a branch:

```cpp
IExecutionAdapter* adapter = nullptr;

#ifdef HAS_BINANCE
if (hybrid_exec_) {
    adapter = hybrid_exec_.get();
} else
#endif
if (config_.engine_mode == engine_mode::live || config_.engine_mode == engine_mode::shadow) {
    adapter = config_.provider->execution_adapter().get();
} else {
    adapter = local_book_adapter_.get();
}
```

---

## Phase 4: Commission Handling for Paper Limit Fills

### 4.1 Apply fee model to book-generated fills

The `LocalBookAdapter` already calculates commissions using the fee model passed to the engine. Ensure the `HybridExecutor`'s `LocalBookAdapter` uses the same fee model.

**File:** `BacktestEngine/src/providers/binance/hybrid_executor.h`

Add fee model parameter to constructor:
```cpp
HybridExecutor(std::shared_ptr<BinanceExecutor> paper_exec,
               std::shared_ptr<orderbook> book,
               std::shared_ptr<IFillModel> fill_model = nullptr,
               std::shared_ptr<IFeeModel> fee_model = nullptr)
    : paper_(std::move(paper_exec))
    , book_adapter_(std::make_unique<LocalBookAdapter>(
          std::move(book),
          fill_model ? std::move(fill_model)
                     : std::make_shared<RealisticFillModel>(0.05, 0.8, 5.0),
          fee_model ? std::move(fee_model)
                    : std::make_shared<ZeroFeeModel>()))
{}
```

Pass the engine's fee model when creating the HybridExecutor in `run_streaming()`:
```cpp
hybrid_exec_ = std::make_shared<HybridExecutor>(
    binance_exec, book, nullptr, config_.fee_model);
```

---

## Phase 5: Limit Order Matching on Each Market Update

### 5.1 Trigger matching after seeding

After seeding liquidity in the orderbook (Phase 2.1), any resting limit orders from the user should be checked against the new levels. The `LocalBookAdapter` handles this internally via `orderbook::match_orders()` when new liquidity is added.

However, user limit orders are submitted separately. To trigger matching when the market moves to their price:

**File:** `BacktestEngine/src/core/engine.cpp`

After the orderbook seeding block in `process_single_tick()` / `process_single_bar()`:

```cpp
// Check if any resting user orders can now be filled
if (hybrid_exec_) {
    std::vector<fill_event> limit_fills;
    if (hybrid_exec_->poll_fills(limit_fills)) {
        for (auto& f : limit_fills) {
            // Process through normal fill pipeline
            portfolio_.update_fill(f);
            strategy_->set_position_open(f.get_symbol(),
                portfolio_.get_positions().count(f.get_symbol()) > 0);
            publish_event(f);

            #ifdef HAS_WEB_UI
            if (ws_worker_) {
                ws_worker_->broadcast(event_json::fill_to_json(f));
            }
            #endif
        }
    }
}
```

### 5.2 Alternative: Check-and-fill approach

Instead of clearing and re-seeding every tick (expensive), maintain a list of pending user limit orders and check if market price has crossed them:

**File:** `BacktestEngine/src/providers/binance/hybrid_executor.h`

Add a method to check pending limits against current market:
```cpp
void check_pending_limits(double current_price, const std::string& symbol)
{
    // LocalBookAdapter already has the orders in its book.
    // Seeding opposite-side liquidity at current_price will trigger matches.
    // This is handled by the seeding in Phase 2.1.
}
```

The seeding approach is simpler: each tick re-creates the synthetic book around the current price. If a user limit buy at $100 was submitted and the market drops to $99, the seeded ask at $99.01 will match against the resting buy at $100.

---

## Phase 6: WS Command for Limit Orders

### 6.1 Ensure ws_command supports limit order parameters

**File:** `BacktestEngine/src/threading/ws_worker.h`

The existing `ws_command` struct already has `price` and `order_type` fields. Verify the frontend sends these for limit orders:

```cpp
struct ws_command {
    std::string command;
    std::string side;
    double quantity = 0.0;
    double price = 0.0;        // Used for limit price
    std::string order_type;    // "market" or "limit"
    std::string timeframe;
    std::string value;
};
```

### 6.2 Update order creation in process_ws_commands()

**File:** `BacktestEngine/src/core/engine.cpp` in `process_ws_commands()`

Currently the `"order"` handler creates market orders. Extend to handle limit:

```cpp
if (cmd.command == "order") {
    // ... existing validation ...

    order_type otype = order_type::market;
    double price = 0.0;

    if (cmd.order_type == "limit" && cmd.price > 0.0) {
        otype = order_type::limit;
        price = cmd.price;
    } else {
        // Market order: use last known price
        price = last_known_price_;  // existing field
    }

    auto o = std::make_shared<order_event>(
        next_order_id(), symbol,
        cmd.side == "buy" ? order_side::buy : order_side::sell,
        cmd.quantity, price, otype);

    process_order(o, event_count);
}
```

---

## Phase 7: Frontend Limit Order UI

### 7.1 Add limit order type to TradeEntry

**File:** `web/src/components/Sidebar/TradeEntry.tsx`

Add order type toggle and price input:

```tsx
const [orderType, setOrderType] = useState<'market' | 'limit'>('market');
const [limitPrice, setLimitPrice] = useState('');

function handleSubmit(side: 'buy' | 'sell') {
    const cmd: any = {
        command: 'order',
        side,
        quantity: parseFloat(quantity),
        order_type: orderType,
    };
    if (orderType === 'limit') {
        cmd.price = parseFloat(limitPrice);
    }
    send(cmd);
}
```

Render:
- Toggle buttons: "Market" / "Limit"
- When "Limit" selected, show price input field
- Disable submit if limit selected but no price entered

---

## Testing

1. **Market order still works:** Submit market buy in paper mode, verify instant fill at last price
2. **Limit buy below market:** Submit limit buy 0.5% below current price, wait for price to dip, verify fill
3. **Limit sell above market:** Submit limit sell 0.5% above current price, wait for price to rise, verify fill
4. **Limit order timeout:** Submit limit buy 5% below market, verify it stays pending (no immediate fill)
5. **Commission applied:** Submit limit order with `--fee tiered --maker-rate 0.001`, verify commission in fill
6. **Fill probability:** With `RealisticFillModel`, submit limit order far from mid, verify it may not fill even when price touches (probabilistic)
7. **Multiple pending:** Submit 3 limit orders at different prices, verify they fill independently as market moves
8. **Portfolio update:** After limit fill, verify portfolio position and equity update correctly

---

## Files Changed (Summary)

| File | Change |
|------|--------|
| `BacktestEngine/src/providers/binance/hybrid_executor.h` | **NEW** — Combines paper market fills with book-based limit fills |
| `BacktestEngine/src/providers/binance/binance_executor.h` | Add `update_last_price()` method |
| `BacktestEngine/src/orderbook/orderbook.h` | Add `clear_all()` method |
| `BacktestEngine/src/core/engine.h` | Add `hybrid_exec_` member |
| `BacktestEngine/src/core/engine.cpp` | Wire hybrid executor, seed orderbook per tick, handle limit fills |
| `BacktestEngine/src/threading/ws_worker.h` | Verify limit order fields in `ws_command` |
| `web/src/components/Sidebar/TradeEntry.tsx` | Add Market/Limit toggle, price input |
