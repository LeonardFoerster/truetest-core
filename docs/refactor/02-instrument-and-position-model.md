# 02 — Instrument registry and generalised position model

## Goal

Introduce first-class **`Instrument`** objects and a **generalised `Position`**
model that supports spot, margin, perpetual futures, CFDs, and AMM outcomes.

Today, symbols are `std::string` everywhere and `position = { qty, cost_basis }`.
That is fine for spot-long-only. It breaks the moment you add Polymarket
(binary outcomes) or MetaTrader (leveraged CFDs) or Binance futures (perps
with funding). Fix the model **before** adding those providers.

## Context

- `execution/portfolio.h` defines `struct position { double qty; double cost_basis; }`
  and `portfolio::can_afford(side, qty, price)` which only checks cash for
  buys and held quantity for sells.
- `core/event.h` uses `std::string symbol` on every event type — high cardinality,
  allocation per event.
- There is no central instrument metadata. Tick size, lot size, price decimals,
  contract multipliers are all implicit.
- `Price` (types/price.h) is fixed-point with a hardcoded `SCALE = 10000`.
  Some crypto pairs need more decimals; instrument metadata should drive this.

## Instructions

1. **Create `types/instrument.h`**:

   ```cpp
   enum class AssetClass : uint8_t { spot, margin, perpetual, future, cfd, option, amm_outcome };
   enum class MarginModel : uint8_t { none, cross, isolated };
   enum class SettlementModel : uint8_t { cash, physical, binary };

   struct Instrument {
       InstrumentId id;              // internal uint32_t
       std::string symbol;           // "BTCUSDT", "EURUSD", "0xabc..."
       std::string venue;            // "binance", "metatrader", "polymarket"
       AssetClass asset_class;
       MarginModel margin_model;
       SettlementModel settlement;
       int price_decimals;           // e.g. 2 for BTCUSDT
       int qty_decimals;              // e.g. 8 for BTCUSDT
       double tick_size;              // smallest price increment
       double lot_size;               // smallest qty increment
       double contract_multiplier = 1.0;  // e.g. 100 for ES futures
       double min_notional = 0.0;
       double max_leverage = 1.0;     // 1.0 = spot
       std::string quote_currency;    // "USDT", "USD"
       std::string base_currency;     // "BTC", "EUR"
   };

   using InstrumentId = uint32_t;
   ```

2. **Create `types/instrument_registry.h/.cpp`**:

   ```cpp
   class InstrumentRegistry {
   public:
       InstrumentId register_instrument(Instrument inst);
       const Instrument& get(InstrumentId id) const;
       const Instrument* find_by_symbol(std::string_view symbol) const;
       std::vector<InstrumentId> all() const;
   private:
       std::vector<Instrument> instruments_;
       std::unordered_map<std::string, InstrumentId> by_symbol_;
   };
   ```

   The registry is owned by the engine (one per run). Providers populate it
   on `open()` via `IProvider::register_instruments(registry)`. Add that
   method to `IProvider` with a default empty implementation.

3. **Generalise `position`** in `execution/portfolio.h`:

   ```cpp
   struct Position {
       InstrumentId instrument;
       double qty = 0.0;              // signed: negative = short
       double avg_entry = 0.0;
       double realized_pnl = 0.0;
       double unrealized_pnl = 0.0;

       // Leverage / margin
       double leverage = 1.0;
       double margin_used = 0.0;      // collateral locked

       // Perpetuals
       double funding_accrued = 0.0;

       // Binary outcome (Polymarket)
       bool settled = false;
       double settlement_value = 0.0;
   };
   ```

   The portfolio stores `unordered_map<InstrumentId, Position>`. Keep a
   deprecation-friendly `position_open(const std::string& symbol)` that looks
   up via the registry.

4. **Routing fills**: `portfolio::on_fill` must receive the `Instrument` (or
   its id) to update leverage / margin correctly. Update `fill_event` to
   carry `InstrumentId instrument_id` alongside `symbol` during the migration.

5. **Short selling** becomes natural once `qty` is signed. Update `can_afford`:
   - Spot: buys need `cash >= qty*price`, sells need `qty <= position.qty` (no shorting).
   - Margin / perp / CFD: both directions allowed up to `margin_used + order_margin <= cash * leverage`.
   - Binary outcome: buys need `cash >= qty * price`, sells only allowed if a position exists.

6. **Funding for perpetuals**: add `portfolio::apply_funding(InstrumentId, double rate_bps)`
   that decrements unrealised PnL by `qty * rate_bps/10000`. The Binance
   perpetual provider (future task) will call it on each funding event.

7. **Per-instrument event path**: the engine currently indexes by symbol
   in several hashmaps (`execution_adapters_`, orderbook registry, etc.).
   Route those through `InstrumentId` internally; keep `symbol` only for
   human-readable logging and the external event shape.

8. **Tests**:
   - `tests/test_instrument_registry.cpp` — register, lookup by id, lookup by symbol, duplicate rejection.
   - `tests/test_position_model.cpp` — spot long, spot sell exceeds qty, margin leveraged long, perp short with funding.
   - Extend `tests/test_portfolio.cpp` to cover signed qty and leverage paths.

## Acceptance criteria

- `Position::qty` is signed; short positions work in tests.
- `InstrumentRegistry` is the only source of tick / lot / decimals information.
- `can_afford` respects leverage for margin/perp instruments.
- All existing tests pass (spot behaviour must be bit-identical under
  `leverage=1.0, margin_model=none`).
- Golden regression tests unchanged.

## Out of scope

- Do not wire the registry into providers other than a stub `register_instruments`.
  Each provider task will populate it.
- Do not implement actual Binance perpetual funding ingestion here —
  only the portfolio-side `apply_funding` hook.
- Do not change `Price` scale behaviour — that is
  [13-price-qty-unification.md](13-price-qty-unification.md).
