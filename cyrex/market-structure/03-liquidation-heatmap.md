# Codex Spec — Liquidation Heatmap

**ID**: `cyrex-structure-liquidation`  
**Priority**: P1  
**Reference**: Cryexc Liquidation Heatmap + live liquidation tape  
**Depends on**: Liquidation feed (venue) + optional OI/leverage model

---

## 1. Goal

Two-layer feature:

1. **Liquidation tape / clusters (P1a — ship first)**: real forced liquidations from exchange streams, aggregated by price/time.
2. **Estimated cascade heatmap (P1b — optional)**: model of where leveraged positions would liquidate if price reaches level — **explicitly labeled ESTIMATE**.

Cryexc-style cascade maps are not pure ground truth; TrueTest must never present estimates as venue facts.

---

## 2. User-visible behavior

### P1a — Live liquidations

- Scatter / bars: X time, Y price, size ∝ notional liquidated.
- Side color: long liq (price down) vs short liq (price up) per venue semantics.
- Side panel: rolling totals (1m/5m/1h), largest prints, symbol filter.
- Optional overlay dots on footprint/live heatmap.

### P1b — Cascade heatmap (estimate)

- Horizontal bands at prices with estimated liquidatable notional.
- Intensity from model; tooltip shows assumptions (leverage tiers, OI share).
- Badge: `MODEL` / `ESTIMATE` always visible.

---

## 3. Data inputs

### Real liquidations (Binance futures golden path)

Stream: `!forceOrder@arr` or symbol force order (confirm current Binance combined stream in provider layer).

Normalize:

```cpp
struct CyrexLiquidation {
  std::int64_t ts_ms;
  std::uint32_t symbol_id;
  double price;
  double qty;
  double notional;     // price * qty (or quote qty if provided)
  bool side_sell;      // true: long liquidated (market sell)
  const char* venue;
};
```

### Model inputs (P1b)

| Input | Source |
|-------|--------|
| Open interest | REST/WS mark endpoint |
| Mark / index | funding/mark streams |
| Leverage distribution | **assumed** buckets (e.g. 5×/10×/25×/50×/100× weights) — config file |

---

## 4. Algorithms

### 4.1 Tape aggregation

```cpp
class LiquidationTapeModel {
  void on_liq(const CyrexLiquidation&);
  // rings + clusters: merge liqs within dt_ms and same price bin
  struct Cluster { double price; double notional; int count; bool dominant_sell; };
};
```

### 4.2 Heatmap of realized liqs

Same as trade heatmap: bins over time×price of notional — reuse grid ideas from live heatmap but qty = liq notional.

### 4.3 Cascade estimate (optional, honest)

Naive educational model (document limitations):

```
For each leverage L in tiers:
  // longs liquidate roughly when price ~ entry * (1 - 1/L + fee_buffer)
  // without true entry distribution, approximate using:
  // place synthetic mass at mark * (1 ± k/L) weighted by OI * tier_weight
```

Better approach if only OI available:

- Show **leverage liquidation lines** relative to **current mark** (where a position opened at mark would liq), not a full heatmap.
- Or integrate third-party style density only with clear disclaimer.

**Codex default for P1b**: implement **relative leverage bands** at `mark * (1 ± 1/L)` with OI-scaled thickness — not a fake precise Coinglass clone.

---

## 5. Provider wiring

| Task | Detail |
|------|--------|
| Extend Binance parser | Parse forceOrder payloads → cold DTO |
| **Do not** dual-produce hot EventRing from WS thread without sole-producer design | Prefer cold research queue (see memory-check notes on funding dual-producer) |
| Symbol filter | Active research symbol + optional “all USDT-M” aggregate |

Layer rule: venue parse in `providers/binance/*`; research model must not include Binance headers.

---

## 6. UI wiring

| Panel | Content |
|-------|---------|
| `DeskPanel::liquidations` | Tape table + cluster chart |
| Overlay toggle | Footprint / live heatmap |
| Estimate subpanel | Leverage bands with `ESTIMATE` badge |

Colors: longs liquidated → down color spikes; shorts liquidated → up color spikes.

---

## 7. Tests

```
TEST(LiqTape, AggregatesClusterInTimePrice)
TEST(LiqTape, SideTotals)
TEST(LiqHeatmap, NotionalBins)
TEST(LiqEstimate, LeverageBandsAroundMark)  // if P1b
TEST(Parser, ForceOrderFixture)            // provider-level
```

---

## 8. Acceptance checklist

- [ ] Real force-order prints appear with correct side/price/qty on fixture + live shadow.
- [ ] No SPSC dual-producer regression (research queue sole consumer design).
- [ ] Estimate UI never unlabeled.
- [ ] Overlay optional and off by default if noisy.
- [ ] Gates green.

---

## 9. Non-goals

- Claiming precise liquidation magnets without position-level data.
- Cross-venue liq aggregation without normalized contracts (phase 2).
- Auto-trading on liq spikes.

---

## 10. Codex task prompt

```
Implement cyrex/market-structure/03-liquidation-heatmap.md.
Ship P1a (real liquidation tape + cluster heatmap) first.
Optional P1b leverage bands labeled ESTIMATE only.
Cold-path ingest; no EventRing dual-producer.
```
