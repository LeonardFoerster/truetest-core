# Codex Spec — Footprint Chart

**ID**: `cyrex-orderflow-footprint`  
**Priority**: P0  
**Reference**: Cryexc footprint — bid/ask volume per price level per bar, CVD subplot, multi-exchange aggregation (later)  
**Depends on**: Public trade tape + tick size (`01-dom-ladder` tape buffer is ideal)

---

## 1. Goal

Render a **footprint chart**: for each time bucket (bar), show volume traded at each price level split into bid-side (passive/aggressive semantics) and ask-side, with imbalance highlighting and an optional **CVD** (cumulative volume delta) subplot.

Cryexc insight to copy: dirty-flag caches + LOD (text only when row tall enough). Full rebuild of 200 levels every frame was a multi-ms bottleneck; recompute only when trades arrive.

---

## 2. User-visible behavior

### Chart body

- X axis: time bars (1s / 5s / 15s / 1m / 5m selectable).
- Y axis: price bins (tick × multiple).
- Each cell: left number = sell/aggressor sell volume, right = buy volume (or bid/ask stack — pick one convention and document it).
- Cell background intensity ∝ total volume at that level in the bar.
- Imbalance highlight when `buy / (buy+sell) > threshold` (default 70%) or inverse.
- POC mark per bar (price bin with max volume).
- Last price / mid line.

### Subplot

- CVD line: running sum of `(aggressive_buy_qty - aggressive_sell_qty)`.
- Optional size-bucket CVD (small / medium / large trades) — phase 1.5.

### Controls

- Interval, tick multiple, imbalance %, show numbers, session reset, symbol.

---

## 3. Aggression convention (document in code)

For Binance-style trades with `m` (buyer is maker):

| `is_buyer_maker` | Aggressor | Footprint side |
|------------------|-----------|----------------|
| true | Seller | Sell volume at price |
| false | Buyer | Buy volume at price |

**Always label UI “Buy / Sell aggressor volume”** — never “bid/ask book”.

---

## 4. Data model

```cpp
struct FootprintLevel {
  double buy_qty = 0;
  double sell_qty = 0;
  double total() const { return buy_qty + sell_qty; }
  double delta() const { return buy_qty - sell_qty; }
};

struct FootprintBar {
  std::int64_t start_ms = 0;
  std::int64_t end_ms = 0;
  double open = 0, high = 0, low = 0, close = 0;
  // sparse map: bin_index -> level  OR dense vector over [min_bin, max_bin]
  std::unordered_map<int, FootprintLevel> levels; // MVP; densify if hot
  int poc_bin = 0;
  double bar_delta = 0;
  double bar_volume = 0;
};

class FootprintModel {
public:
  void configure(double tick, int tick_multiple, int interval_ms, int max_bars);
  void on_trade(const CyrexTrade& t);
  void reset_session();
  const std::vector<FootprintBar>& bars() const;
  std::uint64_t version() const;
  double cvd() const;
private:
  void ensure_bar(std::int64_t ts_ms);
  void recompute_poc(FootprintBar&);
  std::uint64_t version_ = 0;
  double cvd_ = 0;
};
```

### Ingest algorithm

```
on_trade(t):
  bar = ensure_bar(t.ts_ms)   // floor(ts / interval) * interval
  bin = price_to_bin(t.price)
  if t.aggressive_buy: bar.levels[bin].buy_qty += t.qty
  else:                bar.levels[bin].sell_qty += t.qty
  update OHLC from t.price
  bar.bar_delta += aggressive ? +qty : -qty
  bar.bar_volume += qty
  cvd_ += aggressive ? +qty : -qty
  recompute_poc(bar)  // or lazy on read
  ++version_
  drop bars older than max_bars from front
```

---

## 5. Render logic (ImGui / ImDrawList)

Cryexc patterns:

1. **Viewport culling**: only bars and price bins intersecting camera.
2. **LOD**:
   - `row_px >= 14`: draw bid/sell text + split bars.
   - `row_px >= 6`: colored rects, no text.
   - `row_px < 6`: single intensity pixel/row strip.
3. **Dirty cache**: when `version_` unchanged and camera unchanged, reuse precomputed pixel layout structs.

Pseudo-draw:

```cpp
for (bar in visible_bars) {
  x0, x1 = time_to_x(bar.start), time_to_x(bar.end)
  for (bin, lvl in bar.levels) {
    if (!price_visible(bin)) continue;
    y0, y1 = price_to_y(bin), price_to_y(bin+1)
    // background total intensity
    // left half sell color width ∝ sell/(buy+sell)
    // right half buy color
    if (row_h >= 14) draw_text(sell), draw_text(buy);
  }
  if (bar.poc_bin) mark_poc(...);
}
// CVD: ImPlot::PlotLine in subplot
```

Color: `theme::up()` / `theme::down()` with alpha from volume percentile within bar.

---

## 6. Wiring into TrueTest

### Feed path

```
public trades ──► ResearchAggregator / FootprintModel (cold)
UI thread ──poll version──► FootprintPanel draws bars()
```

Do **not** put `unordered_map` updates in strategy `on_trade`.

### Desk integration

| Item | Action |
|------|--------|
| `DeskPanel::footprint` | New enum + window name |
| `DeskPage::orderflow` | New page **or** Analysis primary slot |
| `desk_layout_model` | Default dock: footprint primary, CVD bottom |
| `desk_app.cpp` | `draw_footprint_panel(model)` when page active |
| Sources.cmake | model + panel |

### Optional web

Serialize last N bars as dense JSON only via web allow-listed path — **not** required for MVP.

---

## 7. History / replay

MVP: live from process start.  
Phase 2: seed from REST aggTrades / historical trades (Cryexc loads Binance history into local storage). Use existing provider backfill hooks if present; otherwise leave stub `FootprintModel::load_history(span<CyrexTrade>)`.

---

## 8. Tests

```
TEST(Footprint, AssignsTradeToCorrectBarAndBin)
TEST(Footprint, BuyerMakerCountsAsSellAggressor)
TEST(Footprint, PocIsMaxVolumeBin)
TEST(Footprint, CvdIsRunningDelta)
TEST(Footprint, DropsBarsBeyondMax)
TEST(Footprint, TickMultipleMergesBins)
```

Golden: fixed 20-trade list → expected buy/sell matrix for one bar.

---

## 9. Acceptance checklist

- [ ] Live trades update footprint within one UI frame of model ingest.
- [ ] Aggression side matches documented convention.
- [ ] POC and CVD correct vs unit golden.
- [ ] LOD skips text when zoomed out (manual or row_h unit test).
- [ ] Dirty version avoids full rebuild when idle (assert recompute counter).
- [ ] Hot path clean; gates green.

---

## 10. Non-goals

- Multi-exchange footprint merge (sum bins across venues — phase 2).
- Unfinished auction imbalance statistics pack.
- Drawing tools / trendlines (Cryexc has them; defer).

---

## 11. Codex task prompt

```
Implement cyrex/orderflow/02-footprint-chart.md only.
Build FootprintModel (cold) + ImGui footprint panel with CVD subplot.
Use public trade aggression convention; dirty-flag + LOD.
Unit tests and golden fixture required before UI polish.
```
