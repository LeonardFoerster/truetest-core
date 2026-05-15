# USDT-M Futures Live Order Lifecycle

This document gives a simple, end-to-end view of what actually happens when your strategy submits an order in **live mode** on Binance USDT-M futures (`--provider binance-futures --mode live`).

It is intentionally high-level. The goal is to help you understand where things can go wrong and why the engine has so many safety layers.

---

## High-Level Flow

```
Strategy
   │
   ▼
Engine (Risk checks + Venue pre-trade caps)
   │
   ▼
ExecutionBridge
   │
   ├─► BinanceFuturesOrderEncoder (no JSON on hot path)
   │
   ▼
BinanceRestClient → POST /fapi/v1/order (HMAC signed)
   │
   ├─► Binance acks or rejects immediately
   │
   ▼
User-Data WebSocket (ORDER_TRADE_UPDATE events)
   │
   ▼
Engine updates:
   - Portfolio
   - ExitManager (places SL/TP brackets)
   - Analytics / QuestDB
   - RiskManager (post-fill checks)
```

---

## Detailed Step-by-Step

### 1. Strategy Decision
Your strategy (e.g. `mean-reversion`, custom, etc.) calls `submit_order(...)` or the bracket helper.

The engine receives an `OrderIntent`.

### 2. Pre-Submit Checks (Engine + Futures Provider)

Before anything touches the wire, the order goes through **multiple gates**:

| Check | Where | What it does | Failure mode |
|-------|-------|--------------|--------------|
| Engine RiskManager | `risk/risk_manager.cpp` | Max position, daily loss, max trades/hour, etc. | Reject or Halt |
| Futures pre-trade caps | `binance_futures_safety.h` | `--max-notional`, `--max-leverage`, `--min-liq-distance-pct` | Rejection event |
| Reconciler (startup only) | `binance_futures_reconciler.h` | Local position vs `/fapi/v2/positionRisk` | Refuses to start |
| Symbol + one-way mode check | `BinanceFuturesProvider::open()` | Account must be in **one-way mode** | Refuses to start |

Only if all pass does the order proceed.

### 3. Client Order ID Generation
Every order gets a deterministic ID of the form:

```
tt-<epoch_hex>-<seed_hex>-<sequence_hex>
```

This makes reconnects and replays **idempotent** against Binance's duplicate detection.

### 4. Order Encoding & Signing
`BinanceFuturesOrderEncoder` builds the exact query string and signs it with HMAC-SHA256 using OpenSSL (no `nlohmann::json` on the hot path).

### 5. Submission to Binance
`BinanceRestClient` sends a **signed POST** to:

- `/fapi/v1/order` (single order)
- or `/fapi/v1/batchOrders` (if the strategy sends multiple)

The REST response gives the first status (`NEW`, `FILLED`, `REJECTED`, etc.).

### 6. User-Data WebSocket Updates (The Real Source of Truth)
After submission, **all further state changes** come from the user-data stream:

- `ORDER_TRADE_UPDATE`
- `ACCOUNT_UPDATE` (for balance / position / funding changes)

The engine listens on a `listenKey` WebSocket and parses these events in `BinanceFuturesUserDataParser`.

This is how the engine learns about:
- Partial fills
- Full fills
- Order cancellations (by you or by Binance)
- Liquidations
- Funding payments

### 7. What Happens on a Fill

When a fill arrives from the user-data stream:

1. `fill_event` is created with real exchange price, qty, fees, etc.
2. `Portfolio` is updated (realized PnL, position, cash).
3. `ExitManager` checks if this fill should trigger bracket placement or cancellation.
4. Brackets (SL/TP) are placed using `BinanceFuturesBracketAdapter`:
   - Two separate POSTs: `STOP_MARKET` + `TAKE_PROFIT_MARKET`
   - Both use `closePosition=true` + `reduceOnly=true`
   - Not atomic — there is a small window between the two POSTs
5. Event is written to QuestDB (if `--persist`)
6. RiskManager runs post-fill checks
7. Analytics / ShadowTracker updated

### 8. Bracket / Exit Behaviour (Futures Specific)

Futures does **not** use OCO like spot.

Instead:
- Engine places two conditional orders with `closePosition=true`
- When one triggers and the position goes to zero, Binance **automatically cancels** the other `closePosition=true` order on that symbol.
- If placement of the second bracket fails, the `ExitManager` has fallback logic.

### 9. Shutdown / Kill Switch

When the engine shuts down (SIGINT, risk halt, crash, etc.):

1. `BinanceFuturesKillSwitch` runs:
   - `DELETE /fapi/v1/allOpenOrders?symbol=...`
   - Then queries `/fapi/v2/positionRisk`
   - Sends a `reduceOnly=true MARKET` order to close any remaining position
2. Has a hard deadline (`--kill-switch-deadline-ms`, default 5s).
3. If it misses the deadline → loud warning. **Operator must intervene manually.**

### 10. Dead-Man's Switch (Crash Protection)

Independent of the kill-switch:

- On startup the engine posts to `/fapi/v1/countdownCancelAll`
- A background heartbeat thread refreshes it every `heartbeat_ms`
- If the engine dies / hangs / is killed without clean shutdown → Binance automatically cancels **all open orders** on the symbol after the countdown expires.
- Does **NOT** close positions (only cancels orders).
- You **must** still rely on the kill-switch + operator procedures for position flattening.

The engine also has an internal `WorkerWatchdog` that will trigger `halt_flag_` if the heartbeat thread itself stalls.

---

## Key Differences vs Spot

| Aspect                  | Spot                              | USDT-M Futures                          |
|-------------------------|-----------------------------------|-----------------------------------------|
| Reconciliation          | Wallet balances (`/api/v3/account`) | Positions + `availableBalance` (`/fapi/v2/...`) |
| Close on shutdown       | Market sell of free base asset    | `reduceOnly MARKET`                     |
| Brackets                  | OCO or two orders                 | Two `STOP_MARKET`/`TAKE_PROFIT_MARKET` with `closePosition=true` |
| Dead-man's switch       | Not available                     | `countdownCancelAll` (default on)       |
| Liquidation awareness   | None                              | `--min-liq-distance-pct`, liquidation warnings |
| Position side           | Always positive                   | Signed (`long > 0`, `short < 0`)        |

---

## Where Orders Can Fail (and What Catches It)

| Failure                          | Caught By                          | Result |
|----------------------------------|------------------------------------|--------|
| Bad API keys / permissions       | Provider `open()` + reconciler     | Refuses to start |
| Account in hedge mode            | `BinanceFuturesProvider::open()`   | Hard refusal |
| Clock skew > 2 seconds           | Time sync check                    | Hard refusal |
| Order would breach liq distance  | Futures pre-trade risk check       | Rejection event |
| Daily loss limit hit             | RiskManager                        | Halt (+ unwind if `--risk-unwind`) |
| Engine dies while orders open    | Dead-man's switch                  | Binance cancels orders automatically |
| Clean shutdown fails             | Kill-switch                        | Best-effort cancel + reduceOnly close |
| Network flap during submission   | REST retry + user-data WS          | Idempotent ClientOrderId helps |

---

## Summary for Operators

- **Submission** is synchronous (REST) but **state updates** are asynchronous (user-data WebSocket).
- The **user-data stream is the source of truth** after the initial ack.
- Brackets on futures are **not atomic**.
- The **dead-man's switch** protects against sudden death (but does not close positions).
- The **kill-switch** tries to clean up on normal shutdown (and has a time budget).
- Everything important is logged to QuestDB when `--persist` is used.

This lifecycle is why the engine forces you to go through extensive shadow + testnet validation before real money — there are many moving parts between "strategy says buy" and "position is safely closed".

---

**Related Documents**

- [`docs/futures-testnet.md`](futures-testnet.md) — Testnet setup and refusal reasons
- [`docs/demo-trading-workflow.md`](demo-trading-workflow.md) — How to validate before going live
- `src/providers/binance/binance_futures_*` — The actual implementation files
