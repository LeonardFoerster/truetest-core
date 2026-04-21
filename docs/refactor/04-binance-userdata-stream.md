# 04 — Binance user-data WebSocket for live fills

## Goal

Replace the current REST-polling live-fill path (`GET /api/v3/order` per
pending order, per engine tick) with Binance's **user-data WebSocket
stream**. This is the only production-acceptable live fill path.

## Context

- `providers/binance/binance_executor.h::poll_live_fills()` currently loops
  over `pending_live_orders_` and issues a REST GET per order. This
  will miss sub-second fills, burn rate-limit quota, and cannot distinguish
  a filled order from one that was manually cancelled on the exchange.
- Binance user-data streams require a `listenKey` obtained via
  `POST /api/v3/userDataStream`. The key is valid for 60 minutes and must
  be refreshed via `PUT /api/v3/userDataStream` every ~30 minutes.
- Messages on the user-data stream include `executionReport`,
  `outboundAccountPosition`, and `balanceUpdate`. The engine only needs
  `executionReport` for fill/status updates.

## Instructions

1. **Create `providers/binance/binance_userdata_stream.h/.cpp`**:

   ```cpp
   class BinanceUserDataStream {
   public:
       BinanceUserDataStream(std::string api_key,
                             std::string api_secret,
                             std::string ws_host,
                             std::string ws_port,
                             std::string rest_host,
                             std::string rest_port);

       // Obtain listen key via REST, open WS, spawn reader thread.
       bool open();
       void close();

       // Callbacks — invoked from the reader thread.
       using ExecReportHandler = std::function<void(const binance::ExecutionReport&)>;
       void set_execution_report_handler(ExecReportHandler h);
   };
   ```

   Use `Boost.Beast` (already a dependency under `ENABLE_BINANCE`) for the
   WebSocket client. Reuse the SSL context setup from `binance_transport`.

2. **Parse `executionReport` messages** in `providers/binance/binance_userdata_parser.h`.
   The message is JSON with fields: `s` (symbol), `c` (client order id),
   `i` (exchange order id), `S` (side), `o` (type), `q` (order qty),
   `p` (price), `X` (current status), `x` (current exec type), `l` (last
   fill qty), `L` (last fill price), `n` (commission), `N` (commission
   asset), `T` (transaction time).

   Reuse the snprintf-free JSON extraction style already present in
   `binance_parser.h` — do not add a JSON library dependency.

3. **Listen-key refresh**: spawn a lightweight background thread that
   PUTs `/api/v3/userDataStream?listenKey=...` every 30 minutes. On failure,
   call the status handler with `kind::error` and retry with the shared
   `utils/retry.h` helper.

4. **Wire into `BinanceExecutor`**:
   - On `set_live_trading(true)`, construct the stream, register an
     exec-report handler that maps reports to `fill_event` + status
     transitions, and invokes `fill_cb_` / `status_cb_` (from
     [03-async-fill-pipeline.md](03-async-fill-pipeline.md)).
   - Remove `poll_live_fills()` and the `pending_live_orders_` list —
     those become redundant once fills are pushed.

5. **Commission handling**: extract `n` (commission) and `N` (commission asset)
   from the exec report. Feed into `fill_event::commission`. If the asset is
   not the quote currency, emit a status event noting that (caller must
   convert separately).

6. **Reconnection**: on WS disconnect, reconnect with exponential backoff
   (use `utils/retry.h`). On reconnect, re-request `listenKey` and emit
   a `status::disconnected` → `status::connected` sequence so the engine
   can reconcile open orders via a single `GET /api/v3/openOrders` sweep.

7. **Timestamps**: use the exec report's `T` (transaction time, ms epoch),
   not `system_clock::now()`. Parse and store as `chrono::system_clock::time_point`.
   Pass through the `IClock` abstraction from
   [10-clock-abstraction.md](10-clock-abstraction.md) once that lands.

8. **Tests**:
   - `tests/test_binance_userdata_parser.cpp` — feed recorded
     `executionReport` JSON and assert field extraction.
   - Integration test gated behind an env var pointing at Binance testnet
     (do not run in default CI; document in `tests/README.md`).

## Acceptance criteria

- `BinanceExecutor` contains **no** `GET /api/v3/order` calls.
- Live-mode fills arrive via the push callback within one WS round trip.
- Listen-key refresh runs unattended for at least 24 hours (manual test).
- Reconnect recovers without losing order-state correctness.

## Out of scope

- Portfolio reconciliation beyond the single `openOrders` sweep on reconnect.
- Balance / account-state tracking (`outboundAccountPosition`) — future task.
- Non-spot Binance products (futures, margin). Hook shape stays the same
  but endpoints differ.
