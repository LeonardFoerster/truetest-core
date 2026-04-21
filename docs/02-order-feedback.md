# Step 2: Order Feedback Loop (Acknowledgements, Rejections, Errors)

## Goal

When a user submits an order from the web UI, they must receive explicit feedback: acknowledged, filled, rejected, or errored. Currently orders fail silently. This step adds a new `order_response` event type that flows from the engine back to the browser and surfaces in the UI.

---

## Phase 1: New Event Type on the C++ Side

### 1.1 Define response structure in `event_json.h`

**File:** `BacktestEngine/src/core/event_json.h`

Add a new serialization function (no new event class needed — this is a UI-only message):

```cpp
inline std::string order_response_to_json(
    uint64_t order_id,
    const std::string& status,      // "accepted", "filled", "rejected", "error"
    const std::string& reason,      // human-readable reason (empty on success)
    const std::string& symbol = "",
    const std::string& side = "",
    double quantity = 0.0,
    double price = 0.0)
{
    char buf[512];
    std::snprintf(buf, sizeof(buf),
        R"({"type":"order_response","data":{"order_id":%llu,"status":"%s","reason":"%s","symbol":"%s","side":"%s","quantity":%.8g,"price":%.6f}})",
        static_cast<unsigned long long>(order_id),
        status.c_str(), reason.c_str(), symbol.c_str(), side.c_str(),
        quantity, price);
    return buf;
}
```

---

## Phase 2: Emit Responses from the Engine

### 2.1 Order submission feedback in `process_ws_commands()`

**File:** `BacktestEngine/src/core/engine.cpp` in `process_ws_commands()`

Currently the `"order"` command handler (around line 434) builds an order and calls `process_order()`. Wrap this with response broadcasting:

**Before submitting:**
```cpp
// Broadcast acceptance
ws_worker_->broadcast(event_json::order_response_to_json(
    o->get_order_id(), "accepted", "",
    symbol, cmd.side, cmd.quantity, price));
```

**After `process_order()` returns:**
- If `process_order()` returns `false` (risk halt): broadcast rejection
  ```cpp
  ws_worker_->broadcast(event_json::order_response_to_json(
      o->get_order_id(), "rejected", "risk manager halt",
      symbol, cmd.side, cmd.quantity, price));
  ```

**On validation failure** (empty side, zero quantity, no symbol):
```cpp
ws_worker_->broadcast(event_json::order_response_to_json(
    0, "rejected", "invalid order parameters"));
```

### 2.2 Risk manager rejection feedback

**File:** `BacktestEngine/src/core/engine.cpp` in `process_order()`

The risk manager check (around line 600) returns `risk_action::halt` or `risk_action::reject`. Currently a halt just sets the flag. Add WS broadcast:

```cpp
#ifdef HAS_WEB_UI
if (ws_worker_) {
    const char* reason = (action == risk_action::halt)
        ? "risk limit breached — engine halted"
        : "order rejected by risk manager";
    ws_worker_->broadcast(event_json::order_response_to_json(
        o.get_order_id(), "rejected", reason,
        o.get_symbol(),
        o.get_side() == order_side::buy ? "buy" : "sell",
        o.get_quantity(), o.get_price()));
}
#endif
```

### 2.3 Fill confirmation as order response

After a fill is processed in `process_order()`, broadcast a filled response:

```cpp
#ifdef HAS_WEB_UI
if (ws_worker_) {
    ws_worker_->broadcast(event_json::order_response_to_json(
        f.get_order_id(), "filled", "",
        f.get_symbol(),
        f.get_side() == order_side::buy ? "buy" : "sell",
        f.get_filled_quantity(), f.get_fill_price()));
}
#endif
```

### 2.4 Live execution errors

**File:** `BacktestEngine/src/providers/binance/binance_executor.h`

In `submit_order()` for live mode, after the REST call:
- If HTTP status is not 200, capture the error body
- Store it in a new field: `std::string last_error_`
- Add getter: `const std::string& last_error() const`

**File:** `BacktestEngine/src/core/engine.cpp` in `process_order()`

After `adapter->submit_order()`, check for errors:
```cpp
#ifdef HAS_WEB_UI
if (auto* binance = dynamic_cast<BinanceExecutor*>(adapter.get())) {
    if (!binance->last_error().empty() && ws_worker_) {
        ws_worker_->broadcast(event_json::order_response_to_json(
            o->get_order_id(), "error", binance->last_error(),
            o->get_symbol(), side_str, o->get_quantity(), o->get_price()));
    }
}
#endif
```

---

## Phase 3: Frontend Handling

### 3.1 Add dispatcher case

**File:** `web/src/store/dispatcher.ts`

Add new case in the `switch (msg.type)` block:

```typescript
case 'order_response': {
    const d = msg.data;
    const orderId = d.order_id ?? 0;
    const status = d.status as string ?? 'unknown';
    const reason = d.reason as string ?? '';
    const side = (d.side as string ?? '').toUpperCase();
    const qty = d.quantity ?? 0;
    const price = d.price ?? 0;
    const symbol = d.symbol ?? '';

    // Add to orders tab
    dispatchers.engine({
        type: 'ADD_ORDER_RESPONSE',
        response: { orderId, status, reason, symbol, side, qty, price, time: Date.now() },
    });

    // Toast notification
    if (dispatchers.toast) {
        switch (status) {
            case 'accepted':
                dispatchers.toast(`Order #${orderId} accepted: ${side} ${qty} ${symbol}`, 'info');
                break;
            case 'filled':
                dispatchers.toast(`Order #${orderId} filled: ${side} ${qty} ${symbol} @ ${price.toFixed(2)}`, 'success');
                break;
            case 'rejected':
                dispatchers.toast(`Order rejected: ${reason}`, 'error');
                break;
            case 'error':
                dispatchers.toast(`Order error: ${reason}`, 'error');
                break;
        }
    }
    break;
}
```

### 3.2 Add order response tracking to EngineStore

**File:** `web/src/store/EngineStore.tsx`

Add to state:
```typescript
interface OrderResponse {
    orderId: number;
    status: 'accepted' | 'filled' | 'rejected' | 'error';
    reason: string;
    symbol: string;
    side: string;
    qty: number;
    price: number;
    time: number;
}

interface EngineState {
    // ... existing fields ...
    orderResponses: OrderResponse[];
}
```

Add action:
```typescript
| { type: 'ADD_ORDER_RESPONSE'; response: OrderResponse }
```

Add reducer case:
```typescript
case 'ADD_ORDER_RESPONSE': {
    const responses = [action.response, ...state.orderResponses].slice(0, 100);
    return { ...state, orderResponses: responses };
}
```

### 3.3 Update OrdersTab to show responses

**File:** `web/src/components/BottomPanel/OrdersTab.tsx`

Currently this tab likely shows a placeholder. Replace with a table that reads `orderResponses` from EngineStore:

| Time | Order ID | Symbol | Side | Qty | Price | Status | Reason |
|------|----------|--------|------|-----|-------|--------|--------|

Color-code the Status column:
- `accepted` → blue
- `filled` → green
- `rejected` → red
- `error` → red

### 3.4 Add loading state to TradeEntry

**File:** `web/src/components/Sidebar/TradeEntry.tsx`

After sending an order command:
1. Disable the Buy/Sell button temporarily (prevent double-submit)
2. Re-enable after receiving an `order_response` for the submitted order, or after a 5-second timeout
3. Show a small spinner or "Submitting..." text on the button

This requires tracking the last submitted order ID. Since the order ID is assigned server-side, use a simple `pendingOrder` state that clears on the next `order_response`.

---

## Phase 4: Error Event Type for General Engine Errors

### 4.1 Add generic error broadcast

**File:** `BacktestEngine/src/core/event_json.h`

```cpp
inline std::string error_to_json(const std::string& message,
                                  const std::string& source = "engine")
{
    char buf[512];
    std::snprintf(buf, sizeof(buf),
        R"({"type":"error","data":{"message":"%s","source":"%s","timestamp":%lld}})",
        message.c_str(), source.c_str(),
        static_cast<long long>(epoch_ms(std::chrono::system_clock::now())));
    return buf;
}
```

### 4.2 Broadcast errors from critical paths

Use `error_to_json()` in:
- `engine.cpp` when `worker_failed_` is detected: `"Worker thread crashed"`
- `engine.cpp` when halt_flag is set by risk worker: `"Risk manager halted engine"`
- `binance_transport.h` on reconnect failure: `"Binance connection lost after N retries"`

### 4.3 Handle in dispatcher

**File:** `web/src/store/dispatcher.ts`

```typescript
case 'error': {
    const message = (msg.data as any).message ?? 'Unknown engine error';
    if (dispatchers.toast) {
        dispatchers.toast(message, 'error');
    }
    break;
}
```

---

## Testing

1. **Order accepted:** Submit a market buy from TradeEntry, verify toast "Order #N accepted" appears
2. **Order filled:** After acceptance, verify "Order #N filled" toast appears with price
3. **Order rejected:** Configure tight risk limits (`--risk-fraction 0.001`), submit a large order, verify "rejected by risk manager" toast
4. **Invalid order:** Submit with zero quantity, verify "invalid order parameters" rejection
5. **Error event:** Kill the Binance WebSocket mid-stream, verify "connection lost" error toast
6. **OrdersTab:** Verify all responses appear in the Orders tab in the bottom panel with correct color coding

---

## Files Changed (Summary)

| File | Change |
|------|--------|
| `BacktestEngine/src/core/event_json.h` | Add `order_response_to_json()`, `error_to_json()` |
| `BacktestEngine/src/core/engine.cpp` | Broadcast responses on order accept/fill/reject/halt |
| `BacktestEngine/src/providers/binance/binance_executor.h` | Add `last_error_` field and getter |
| `web/src/store/dispatcher.ts` | Handle `order_response` and `error` message types |
| `web/src/store/EngineStore.tsx` | Add `orderResponses[]`, `ADD_ORDER_RESPONSE` action |
| `web/src/components/BottomPanel/OrdersTab.tsx` | Render order response table |
| `web/src/components/Sidebar/TradeEntry.tsx` | Add pending state, disable button during submission |
