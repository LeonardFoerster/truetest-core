# Codex Spec — Funding Rate Arbitrage

**ID**: `cyrex-structure-funding-arb`  
**Priority**: P2  
**Reference**: Cryexc funding rate views / arb comparison across venues  
**Depends on**: Funding + mark/index fields per venue/symbol

---

## 1. Goal

Build a **funding intelligence table** for perpetual futures:

- Current / predicted funding rate
- Annualized rate
- Time to next funding
- Mark vs index basis
- Cross-venue spreads for the same underlying (arb surface)

TrueTest already has partial `funding_event` portfolio cash deltas; this feature is **market data research**, not portfolio accounting.

---

## 2. User-visible behavior

### Primary table (sortable)

| Column | Description |
|--------|-------------|
| Symbol | e.g. BTCUSDT |
| Venue | binance / bitget / … |
| Funding | last or predicted rate (frac per interval) |
| Ann. % | annualized |
| Next | countdown to funding timestamp |
| Mark | mark price |
| Index | index price |
| Basis bps | (mark-index)/index × 1e4 |
| OI | open interest if available |
| Spread vs best | vs lowest funding venue for same asset |

### Views

1. **Single venue screener** — all symbols on Binance sorted by |funding|.
2. **Cross-venue arb matrix** — for selected asset, rates by venue; highlight max long/short carry.
3. **History sparkline** — funding over last N intervals (phase 1.5).

Alerts (optional): rate crosses threshold — desk toast only, not trading.

---

## 3. Data contracts

```cpp
struct FundingQuote {
  std::uint32_t symbol_id;
  std::string venue;
  std::int64_t ts_ms;
  double funding_rate;          // e.g. 0.0001 = 0.01% per interval
  double predicted_rate;        // if venue provides; else NaN
  std::int64_t next_funding_ms;
  int interval_hours;           // 8 for Binance classic, 1/4 for some venues
  double mark_price;
  double index_price;
  double open_interest;         // NaN if unknown
};

class FundingArbModel {
public:
  void upsert(const FundingQuote&);
  double annualize(const FundingQuote&) const;
  struct ArbRow { /* table fields + best_venue_for_long_carry */ };
  std::vector<ArbRow> screener() const;
  std::vector<ArbRow> cross_venue(std::string_view asset_key) const;
};
```

### Annualization

```
periods_per_year = 365.25 * 24 / interval_hours
ann = funding_rate * periods_per_year
// display as percent: ann * 100
```

Do **not** compound unless UI toggle “compounded APY” — default simple annualization (industry common for screeners).

### Basis

```
basis_bps = (mark - index) / index * 10_000
```

---

## 4. Provider wiring

| Venue | Sources (verify current endpoints) |
|-------|--------------------------------------|
| Binance USDM | premium index WS / REST `premiumIndex`, mark price stream |
| Bitget | analogous REST/WS |

Implementation rules:

1. Parse into `FundingQuote` in provider layer → push to **cold** research queue.
2. Avoid dual-producer on hot event rings (existing funding path risk — research path must be clean).
3. Portfolio `funding_event` cash application stays separate from this screener.

Multi-venue arb requires **both** providers enabled; single-venue screener works with Binance only.

---

## 5. UI wiring

| Item | Detail |
|------|--------|
| `DeskPanel::funding` | Structure page |
| Sortable ImGui table | click headers |
| Color | positive funding (longs pay) vs negative |
| Detail drawer | basis + countdown + raw JSON-free fields |
| Refresh | 1–5 s REST poll **or** WS update; show data age |

Empty: “No funding quotes — enable futures provider / mark stream”.

---

## 6. Arb logic (display-only)

For asset BTC:

```
best_to_long_perp  = venue with lowest (most negative) funding
best_to_short_perp = venue with highest funding
spread = high - low
```

Optional estimated carry:

```
est_8h = spread * notional  // user-entered notional in UI only
```

**Never** auto-place legs. Label “illustrative”.

---

## 7. Tests

```
TEST(FundingAnnualize, EightHourBinance)
TEST(FundingAnnualize, OneHourVenue)
TEST(FundingBasis, Bps)
TEST(FundingArb, PicksMinMaxVenue)
TEST(FundingScreener, SortByAbsRate)
TEST(FundingModel, UpsertReplacesSameVenueSymbol)
```

---

## 8. Acceptance checklist

- [ ] Rates match venue REST within tolerance on manual check.
- [ ] Annualization formula documented and tested.
- [ ] Countdown uses next_funding_ms correctly (timezone-safe ms epoch).
- [ ] Cross-venue section disabled cleanly with one venue.
- [ ] No trading side effects; cold path; gates green.
- [ ] Distinct from portfolio funding PnL accounting.

---

## 9. Non-goals

- Automated cash-and-carry execution.
- Spot-perp basis arb full stack (inventory, borrow).
- Predicting next funding with ML.

---

## 10. Codex task prompt

```
Implement cyrex/market-structure/05-funding-rate-arbitrage.md only.
FundingArbModel + desk screener table; Binance-first quotes on cold path.
Unit-test annualize, basis, min/max venue arb rows.
No order placement.
```
