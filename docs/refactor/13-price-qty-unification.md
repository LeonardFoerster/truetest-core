# 13 — Unified price and quantity representation

## Goal

Eliminate the **double / int64 / Price / scaled-integer** mix for prices
and quantities. Today:

- `Price` (types/price.h) is fixed-point `int64_t` scaled by `10000`.
- `order_event::price_` / `fill_price_` / `quantity_` are all `double`.
- The orderbook uses `quantity` = `uint64_t` scaled by a configurable
  `qty_scale = 1e8`.
- `portfolio` stores cash and positions in `double`.

Values are converted at every boundary: `Price::from_double(o.get_price())`
on order submit, `our_trade_info.quantity_ / qty_scale_` on fill emission,
etc. Each conversion is a precision hazard. In a real-money system, that
accumulates rounding error over millions of events.

## Context

Instrument decimals come from the registry introduced in
[02-instrument-and-position-model.md](02-instrument-and-position-model.md).
Use them as the single source of truth for scale.

## Instructions

1. **Define `types/decimal.h`**:

   ```cpp
   // Fixed-point decimal, scale is implicit from the carrying type context
   // (e.g. per-Instrument price_decimals / qty_decimals).
   // Internally int64_t; range is ±9.2e18 / scale.
   class Decimal {
   public:
       constexpr Decimal() : raw_(0) {}
       constexpr explicit Decimal(int64_t raw) : raw_(raw) {}

       static Decimal from_double(double d, int decimals);
       static Decimal from_string(std::string_view s, int decimals);

       int64_t raw() const { return raw_; }
       double to_double(int decimals) const;
       std::string to_string(int decimals) const;

       // Arithmetic — same scale required
       Decimal operator+(Decimal o) const { return Decimal(raw_ + o.raw_); }
       Decimal operator-(Decimal o) const { return Decimal(raw_ - o.raw_); }
       Decimal operator-() const { return Decimal(-raw_); }
       // Multiplication / division take a second "decimals" parameter; see notes.

       // Comparisons
       bool operator==(Decimal o) const { return raw_ == o.raw_; }
       bool operator<(Decimal o) const  { return raw_ < o.raw_; }
       // etc.
   private:
       int64_t raw_;
   };
   ```

   Notes:
   - Addition / subtraction requires both operands on the same scale.
     The scale is not carried by the type — it is carried by the field
     owner. Assert at the call site.
   - Multiplication (price × qty) produces a larger scale: provide
     `mul(Decimal a, int a_dec, Decimal b, int b_dec, int result_dec)`
     helpers or a `Notional` type with its own scale.

2. **Retire `Price`.** `Decimal` supersedes it. Orderbook uses
   `Decimal` for price levels. The scale comes from the `Instrument`
   bound to the orderbook instance.

3. **Change `order_event` and `fill_event`** to carry `Decimal price`,
   `Decimal quantity`. The `InstrumentId` already attached (from task 02)
   tells consumers what scale to interpret.

4. **Update orderbook** to index `price_level` by `Decimal`. Remove
   `quantity` (the `uint64_t`) and replace with `Decimal`.

5. **Portfolio cash and PnL as `Decimal`** with an implicit scale of
   `quote_currency.decimals` (e.g. 4 for USD, 6 for USDC, 8 for BTC).
   Store the quote-currency instrument id on each portfolio entry.

6. **Boundary conversions** (double → Decimal) only happen at:
   - Data ingestion from CSV / WebSocket text frames.
   - Strategy signals (strategies may still work in `double` and convert
     once before submitting).
   - Reporting / analytics output.

   Everywhere else: **Decimal only**.

7. **Tests**:
   - `tests/test_decimal.cpp` — boundary conversions, arithmetic, rounding.
   - Extend `tests/test_orderbook.cpp` to use `Decimal` end-to-end.
   - Golden regression: results must match to within the representable
     precision of the instrument. Regenerate the golden files and note
     the expected drift.

## Acceptance criteria

- `rg 'Price::from_double|from_double.*price' BacktestEngine/src` returns 0.
- No fill event carries `double` price or quantity.
- The `qty_scale` engine_config field is removed (or reinterpreted as a
  default for instruments that do not specify decimals).
- Golden regression test is regenerated and the diff is tiny (precision
  improvement in the least significant decimal).

## Out of scope

- Arbitrary-precision / bignum arithmetic. `int64_t` with 9-decimal
  precision handles every realistic trading use case.
- Currency conversion (FX). Track balances in native quote currency;
  cross-currency reporting is a later concern.
- Changing analytics output precision. Keep that `double` — reporting
  is not the hot path.
