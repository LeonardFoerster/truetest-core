# 06 — Polymarket provider

## Goal

Implement a Polymarket provider for binary-outcome prediction markets.

Polymarket is fundamentally different from every other venue in the
codebase: binary outcomes (YES/NO), AMM-style pricing, settlement on
oracle resolution. It is the test case that proves the provider
abstraction is general enough. If something is wrong with the abstraction,
Polymarket will expose it.

## Context

- `providers/polymarket/` is empty.
- Polymarket uses the CLOB (central limit order book) model for most
  markets now — traded as ERC-1155 outcome tokens on Polygon. WebSocket
  market data and REST order submission are available:
  - Market data: `wss://ws-subscriptions-clob.polymarket.com/ws/market`
  - Orders: `POST https://clob.polymarket.com/order` (signed EIP-712)
- An outcome token has price in `[0, 1]`. This is NOT a currency pair —
  it's a probability. Settlement is 0 or 1 on resolution.
- Shorting is done by holding the opposite outcome token, not by short
  selling.

## Instructions

1. **Instrument metadata**: each Polymarket *market* contributes two
   instruments (YES token, NO token). Fields:

   ```
   asset_class = AssetClass::amm_outcome  // (or a new AssetClass::binary_outcome)
   settlement  = SettlementModel::binary
   tick_size   = 0.001  // 0.1 cent
   lot_size    = 1.0    // one outcome share
   price_decimals = 3
   min_notional = 1.0   // USDC
   ```

   Extend `Instrument` with an optional `resolution_source` field (string)
   and `expiry` (time_point) to capture oracle + deadline.

2. **Transport** `providers/polymarket/pm_transport.h/.cpp`: WebSocket
   client against the CLOB market-data endpoint. Subscribe by
   `asset_id` (the token ID).

3. **Parser** `providers/polymarket/pm_parser.h`: extract book updates
   and trades from Polymarket's JSON frames. Hand-rolled extraction as
   with the other providers.

4. **Execution adapter** `providers/polymarket/pm_executor.h`:
   - `submit_order` signs an EIP-712 order struct, POSTs to `/order`.
     EIP-712 signing requires an Ethereum private key — read from config,
     never log it.
   - `cancel_order` calls `DELETE /order/{id}`.
   - Fills arrive via the market-data WebSocket's trade stream filtered
     by the bot's `maker` address.

   Signing uses `libsecp256k1` or OpenSSL's EC primitives (OpenSSL is
   already linked under `ENABLE_BINANCE`). If this becomes painful,
   add a thin `ENABLE_POLYMARKET` flag that pulls in `libsecp256k1`
   via FetchContent.

5. **Settlement**: Polymarket resolves via an oracle. Add an
   `IExecutionAdapter::on_settlement(InstrumentId, double settlement_value)`
   hook so the provider can mark positions settled and compute final PnL.
   Update `Position::settled` and `settlement_value` accordingly.

6. **AMM price model (optional, v2)**: if adding LP / market-making on
   Polymarket's LP pools, model it as a new `IQuoteAdapter` interface.
   Do NOT shoehorn it into `IExecutionAdapter`. For v1, CLOB trading only.

7. **Register provider** via `REGISTER_PROVIDER("polymarket", ...)`.

8. **CMake flag** `ENABLE_POLYMARKET=ON`. Depends on OpenSSL and Boost.Beast.

9. **Tests**:
   - `tests/test_polymarket_parser.cpp` — recorded CLOB frames.
   - `tests/test_polymarket_signer.cpp` — EIP-712 signature matches a
     known reference vector.
   - Manual end-to-end against Polymarket's testnet / Amoy fork documented
     in `providers/polymarket/README.md`.

## Acceptance criteria

- `cmake -B build -DENABLE_POLYMARKET=ON` builds cleanly.
- A backtest against recorded Polymarket data runs through the full
  pipeline (data → strategy → order → fill → portfolio).
- The new `AssetClass::amm_outcome` / `binary_outcome` is the only
  instrument-specific special case; the engine core does not branch
  on it.
- EIP-712 signer reference vector test passes.

## Out of scope

- On-chain settlement verification (trust the oracle / Polymarket API).
- LP / market-making on AMM pools (separate `IQuoteAdapter` track).
- Automatic wallet management / funding flows.
