# 03 — Async fill pipeline

## Goal

Replace the current synchronous `submit_order` → `poll_fills` pattern with
a **push-based** fill delivery mechanism. Real exchanges push execution
reports asynchronously; polling them from the engine's main loop is a
latency tax and misses fast partial fills.

## Context

- `execution/execution_adapter.h` defines:

  ```cpp
  virtual void submit_order(const order_event& o) = 0;
  virtual bool poll_fills(std::vector<fill_event>& out) = 0;
  ```

- `engine::process_order()` calls `submit_order` then `adapter->poll_fills()`
  immediately. For `LocalBookAdapter` this works because the book matches
  synchronously; for Binance live it does not.
- `BinanceExecutor::poll_live_fills()` issues `GET /api/v3/order` per
  pending order on every poll — a rate-limit hazard and will miss fills
  between polls.

## Instructions

1. **Add a callback slot to `IExecutionAdapter`**:

   ```cpp
   class IExecutionAdapter {
   public:
       using FillHandler = std::function<void(const fill_event&)>;
       using OrderStatusHandler = std::function<void(uint64_t order_id, order_status new_status, const std::string& reason)>;

       virtual void set_fill_handler(FillHandler h) { fill_cb_ = std::move(h); }
       virtual void set_order_status_handler(OrderStatusHandler h) { status_cb_ = std::move(h); }

       // Existing poll_fills() stays for a deprecation window.
   protected:
       FillHandler fill_cb_;
       OrderStatusHandler status_cb_;
   };
   ```

2. **`LocalBookAdapter`** emits fills synchronously inside `submit_order`
   by invoking `fill_cb_` directly. `poll_fills` becomes a no-op returning `false`.

3. **`BinanceExecutor`** must stop polling REST. All live-mode fills come
   from the user-data WebSocket (see [04-binance-userdata-stream.md](04-binance-userdata-stream.md)).
   The executor exposes a method `on_execution_report(const json& report)`
   which the user-data stream calls from its reader thread; that method
   calls `fill_cb_`.

4. **Thread-safety**: fill callbacks may be invoked from a provider's reader
   thread. The engine must own a lock-free SPSC ring (reuse `RingBuffer`)
   for fills, pushed by the adapter's reader thread and consumed by the
   engine event loop. Add `engine::fill_ring_` and drain it once per tick.

5. **Delete `poll_fills` after migration**. When every adapter has a
   callback path and the tests still pass, remove the polling loop from
   `engine::process_order`. This is the acceptance gate.

6. **Order status events**: expose `order_status` transitions (`accepted`,
   `rejected`, `partially_filled`, `filled`, `cancelled`, `expired`) through
   `OrderStatusHandler`. `OrderTracker` subscribes to these.

7. **Tests**:
   - `tests/test_async_fill_pipeline.cpp`: a fake adapter that schedules
     callbacks on a background thread; assert the engine picks them up
     in order and applies them to the portfolio.
   - Thread-sanitizer build (`ENABLE_TSAN=ON`) must pass on this test.

## Acceptance criteria

- `IExecutionAdapter::poll_fills` is removed from the engine hot path.
  (Keep the method itself only if some adapter still needs it internally.)
- Fills flow through a lock-free ring from adapter thread → engine thread.
- TSAN build of the test suite passes.
- Golden regression tests unchanged.

## Out of scope

- Binance user-data stream implementation is a separate task
  ([04-binance-userdata-stream.md](04-binance-userdata-stream.md)).
- Do not redesign `fill_event` shape beyond adding `InstrumentId`
  (already covered in task 02).
