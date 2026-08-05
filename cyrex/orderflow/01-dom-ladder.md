# Codex Spec — DOM / Depth of Market Ladder

**ID**: `cyrex-orderflow-dom`  
**Priority**: P0 (first ship)  
**Reference**: Cryexc “DOM / Ladder” — bid/ask size, sold/bought, delta, volume columns  
**TrueTest baseline**: `draw_l2_panel` in `src/ui/desk/panels/market_panels.cpp` + `dashboard_snapshot::l2`

---

## 1. Goal

Build a professional **Depth of Market ladder** that shows live book depth centered on mid/last, with short-window trade aggression at each price level. This is the simplest Cryexc-class view and the UI pattern seed for all later orderflow panels.

---

## 2. User-visible behavior (match Cryexc intent)

Columns (left → right, configurable):

| Column | Meaning |
|--------|---------|
| Bid size | Resting bid qty at price |
| Bid orders (optional) | Count if available; omit if L2 size-only |
| Price | Center axis; highlight mid / last |
| Ask size | Resting ask qty |
| Bought | Aggressive buy qty at this price in window |
| Sold | Aggressive sell qty at this price in window |
| Delta | Bought − Sold |
| Total | Bought + Sold |

UX details:

- Vertical scroll center-locks on **last trade** or **mid** (toggle).
- Row height fixed (~18–22 px); depth drawn as horizontal bar behind size (existing `depth_bar` helper).
- Imbalance meter above ladder (already partially present).
- Flash row on trade hit (fade 200–400 ms).
- Controls: tick multiple (1×, 2×, 5×, 10× tick), levels (20/50/100), window (1s/5s/30s/session).

Empty state: if `l2.source == none`, show “enable `--depth-stream` / wait for book” (same honesty as current panel).

---

## 3. Data inputs

| Source | Event / field | Use |
|--------|---------------|-----|
| L2 | `l2_snapshot_event` / `l2_update_event` or snapshot `dashboard_snapshot::l2` | Resting depth |
| Public trades | `tick` / trade tape (not strategy fills) | Bought/Sold/Delta |
| Instrument | tick size | Price binning / grouping |

**Do not** use strategy fill rows (`dashboard_snapshot::fill_row`) as market tape — those are own fills.

---

## 4. Logic (cold path)

### 4.1 Price grouping

```
bin = floor(price / (tick * multiple) + eps) * (tick * multiple)
```

Aggregate bid/ask sizes that fall into the same bin when multiple > 1.

### 4.2 Trade window ring

Maintain per-symbol ring of recent public trades (capacity e.g. 8192):

```cpp
struct DomTradeHit {
  std::int64_t ts_ms;
  double price;
  double qty;
  bool aggressive_buy; // !is_buyer_maker on most crypto venues
};

// On each trade:
//   bin = price_to_bin(price)
//   if aggressive_buy: bought[bin] += qty
//   else:              sold[bin]  += qty
// Expire hits older than window_ms on UI tick (or lazy expire when reading).
```

### 4.3 Ladder assembly (each UI frame or dirty)

```
levels = merge unique bins from (bids, asks, recent trade bins)
sort by price descending
for each level:
  bid_qty, ask_qty from book
  bought, sold from window aggregates
  delta = bought - sold
center_index = bin closest to last_trade or mid
```

### 4.4 Max size for bar scale

`max_size = max(all visible bid/ask sizes)` — same as current L2 panel.

---

## 5. Module design

```
src/research/cyrex/dom_model.h
  class DomModel {
    void on_book(const CyrexBookSnapshot&);
    void on_trade(const CyrexTrade&);
    void set_tick_multiple(int);
    void set_window_ms(int);
    DomLadderView build_view(int max_levels) const;
  };

src/ui/desk/panels/cyrex/dom_panel.cpp
  void draw_dom_panel(const DomLadderView&, DomPanelState&);
```

Wire feed:

1. **MVP**: desk copies `snap.l2` each tick; trades from a new cold `PublicTapeBuffer` filled by observer/research worker.
2. **Better**: ResearchAggregator receives book+trades and owns `DomModel`.

Extend existing `draw_l2_panel` **or** replace it with `draw_dom_panel` behind the same window name `DeskPanel::l2` to avoid layout churn. Prefer **enhance in place** then rename window title to “DOM”.

---

## 6. UI wiring (ImGui)

File touch list:

| File | Change |
|------|--------|
| `market_panels.cpp` / new `dom_panel.cpp` | Render ladder table |
| `desk_app.cpp` | Call enhanced panel; keep dock slot |
| `desk_theme.h` | Reuse colors; add flash alpha helper if needed |
| `cmake/Sources.cmake` | New cpp |
| `tests/test_cyrex_dom_model.cpp` | Model tests |

Table flags: `ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit`.

Scroll-to-center:

```cpp
if (center_dirty) {
  ImGui::SetScrollY(center_row * row_h - avail_h * 0.5f);
  center_dirty = false;
}
```

---

## 7. Tests (mandatory)

```
TEST(DomModel, GroupsTicksByMultiple)
TEST(DomModel, AggressiveBuyIncrementsBought)
TEST(DomModel, AggressiveSellIncrementsSold)
TEST(DomModel, ExpiresTradesOutsideWindow)
TEST(DomModel, DeltaIsBoughtMinusSold)
TEST(DomModel, CentersOnLastTradePrice)
```

Use synthetic books and trades only.

---

## 8. Acceptance checklist

- [ ] Resting bid/ask match engine L2 for same symbol within one UI tick.
- [ ] Bought/Sold update from **public** trades, not own fills.
- [ ] Tick multiple regroups levels correctly.
- [ ] Window expiry works.
- [ ] No allocations on engine hot path; no freeze-file edits.
- [ ] Unit tests green; desk shows ladder under `--desk` + depth stream.
- [ ] Gate scripts pass.

---

## 9. Non-goals

- Full L3 order count without venue MBO.
- Multi-exchange aggregated ladder (phase 2).
- Order entry from ladder click.

---

## 10. Codex task prompt

```
Implement cyrex/orderflow/01-dom-ladder.md only.
Enhance TrueTest ImGui L2 into a Cryexc-style DOM ladder with trade
bought/sold/delta window. Cold-path DomModel + unit tests first, then UI.
Follow cyrex/00-architecture.md and AGENTS.md.
```
