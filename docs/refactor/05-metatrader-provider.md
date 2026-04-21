# 05 — MetaTrader provider

## Goal

Implement a MetaTrader 4/5 provider so strategies can forward-test against
a broker account via a MetaTrader Expert Advisor (EA) bridge.

The directory `providers/metatrader/` is empty today. CLAUDE.md claims there
is a stub; there is not.

## Context

- MetaTrader EAs cannot open raw sockets portably on all broker setups, but
  they can read/write **named pipes** (Windows) and **files** (all platforms).
  A TCP server listening locally is the simplest cross-platform approach
  — the EA connects with `SocketCreate()` / `SocketConnect()` (MQL5 built-ins).
- Data from MetaTrader is tick-level (`bid`, `ask`, `last`, `volume`). OHLCV
  bars are derivable on the engine side via the existing `BarAggregator`.
- Orders are leveraged CFDs / forex. `AssetClass::cfd` from
  [02-instrument-and-position-model.md](02-instrument-and-position-model.md)
  is the right model.

## Instructions

1. **Protocol specification**: define a tiny line-delimited JSON protocol
   in `providers/metatrader/protocol.md`:

   ```
   // EA → TrueTest
   {"type":"tick","symbol":"EURUSD","bid":1.0823,"ask":1.0824,"ts":1712345678901}
   {"type":"bar","symbol":"EURUSD","tf":"M1","o":...,"h":...,"l":...,"c":...,"v":...,"ts":...}
   {"type":"fill","order_id":123,"symbol":"EURUSD","side":"buy","qty":0.1,"price":1.0824,"commission":0.2,"ts":...}
   {"type":"status","message":"connected"}
   {"type":"instrument","symbol":"EURUSD","digits":5,"tick_size":0.00001,"lot_size":0.01,"contract_size":100000}

   // TrueTest → EA
   {"cmd":"submit","order_id":123,"symbol":"EURUSD","side":"buy","type":"market","qty":0.1,"price":0,"sl":1.08,"tp":1.09}
   {"cmd":"cancel","order_id":123}
   {"cmd":"modify","order_id":123,"price":1.0800,"qty":0.2}
   ```

2. **Create the C++ side** `providers/metatrader/mt_transport.h/.cpp`:
   a TCP server on localhost that accepts one EA connection, reads
   newline-delimited JSON frames, and passes them up. Reuse `utils/retry.h`
   for reconnect.

3. **Create `providers/metatrader/mt_parser.h`** — same hand-rolled JSON
   extraction style as the Binance parser.

4. **Create `providers/metatrader/mt_executor.h`** implementing
   `IExecutionAdapter`. `submit_order` / `cancel_order` / `modify_order`
   serialise to the JSON protocol and write the frame over the transport.
   Fills arrive via the async push pipeline from
   [03-async-fill-pipeline.md](03-async-fill-pipeline.md).

5. **Create `providers/metatrader/mt_provider.h`** — the `IProvider`
   implementation that owns transport + executor. On `open()`, wait for
   the first `status: connected` and `instrument` message, register the
   instruments into the engine's `InstrumentRegistry`, then return.

6. **EA skeleton** `providers/metatrader/ea/TrueTestBridge.mq5`:
   - On tick, emit a `tick` JSON frame.
   - On timer, emit `bar` frames for subscribed timeframes.
   - Read inbound frames each tick and route to `OrderSend` / `OrderClose` /
     `OrderModify`.
   - Handle reconnect loop.

   Keep the MQL5 implementation minimal; the goal is proof of contract, not
   a production EA.

7. **CMake flag** `ENABLE_METATRADER=ON`. Compiles the C++ side only; the
   EA is a source artifact. No new external dependencies.

8. **Register provider** via `REGISTER_PROVIDER("metatrader", ...)` in
   `providers/metatrader/metatrader_register.cpp`.

9. **Tests**:
   - `tests/test_mt_parser.cpp` — golden JSON frames round-trip.
   - `tests/test_mt_executor.cpp` — fake transport captures outbound frames
     and replays inbound fills; verify executor state transitions.
   - Document manual end-to-end test: run the engine, attach the EA in
     MT5 Strategy Tester, verify a market order.

## Acceptance criteria

- `cmake -B build -DENABLE_METATRADER=ON` builds cleanly.
- Parser tests and executor tests pass.
- Provider shows up in `--list-providers`.
- Instruments from MetaTrader are visible in the `InstrumentRegistry`
  with `AssetClass::cfd`.

## Out of scope

- Full MT5 Strategy Tester integration (manual test only for now).
- Bi-directional instrument metadata negotiation — first connect sends
  a static catalog.
- Historical data replay from MetaTrader (use a CSV export instead).
