# Codex Spec — Orderbook Heatmap

**ID**: `cyrex-orderflow-ob-heatmap`  
**Priority**: P1  
**Reference**: Cryexc orderbook heatmap overlaid on footprint / candles — historical depth walls, spoof trails  
**Depends on**: `BookHistoryRing` from `03-live-heatmap.md`, footprint time axis from `02`

---

## 1. Goal

Render **historical resting liquidity** aligned under (or behind) the price chart / footprint so users see where size sat over minutes–hours: walls, pulled liquidity, and absorption.

Difference vs Live Heatmap:

| | Live Heatmap | Orderbook Heatmap |
|--|--------------|-------------------|
| Horizon | seconds–minutes | minutes–hours |
| Column interval | 50–200 ms | 500 ms–5 s (adaptive) |
| Alignment | own panel | **overlay** on footprint/candles |
| Downsample | rarely | mandatory for old columns |

---

## 2. User-visible behavior

- Toggle “Depth heatmap” on footprint (or dedicated chart panel).
- Semi-transparent liquidity behind footprint cells or candle bodies.
- Opacity and colormap independent of footprint volume colors.
- Tooltip on hover: time, price, bid qty, ask qty, age of sample.
- Controls: lookback, opacity, show bid-only / ask-only / total, min qty filter (hide noise).

Cryexc UX: walls appear as bright streaks; sudden disappearances (spoofs) leave truncated streaks — no special detector required if history is faithful.

---

## 3. Logic

### 3.1 Shared ring with multi-resolution (recommended)

```cpp
struct MultiResBookHistory {
  BookHistoryRing fine;   // e.g. 100ms, 600 cols  (~1 min)
  BookHistoryRing medium; // e.g. 1s,   1800 cols (~30 min)
  BookHistoryRing coarse; // e.g. 5s,   1440 cols (~2 h)
};
```

On each fine sample, also accumulate into medium/coarse by max or average qty per bin (max preserves walls better for viz).

### 3.2 Alignment to footprint bars

```
for each footprint bar [t0, t1):
  columns = history.columns_overlapping(t0, t1)
  // display: either average column, or last column in bar, or max-hold
  display_qty[bin] = max_over(columns, bin)  // recommended for walls
```

### 3.3 World → pixel

Reuse footprint camera:

```
x = time_to_x(col.ts)
y = price_to_y(bin)
```

Draw **before** footprint numbers so text stays readable (z-order).

### 3.4 Min filter

Skip cell if `qty < max(min_abs, percentile_floor)` to reduce noise.

---

## 4. Render

Phase 1: ImDrawList rects with global alpha `opacity * color_a(qty)`.  
Phase 2: same GPU texture path as live heatmap; footprint drawn on top with ImGui.

Pseudo:

```cpp
draw_orderbook_heatmap(draw_list, camera, history, opacity);
draw_footprint_cells(...);  // existing
draw_overlays(...);
```

---

## 5. Wiring

| File / component | Change |
|------------------|--------|
| `BookHistoryRing` | Shared; multi-res wrapper |
| `FootprintPanel` | Checkbox + opacity slider; call heatmap draw first |
| Research store | Single history owner for live + overlay consumers |
| Persistence (optional) | Not MVP; memory only |

Engine: still only provides L2 snapshots for sampling — no change to matching engine.

---

## 6. Memory budget

Example: 150 bins × 1800 medium columns × 4 bytes × 2 sides ≈ 2.1 MB. Acceptable cold path. Document limits in model header; fail soft by reducing cols.

---

## 7. Tests

```
TEST(MultiResHistory, CoarseAggregatesFine)
TEST(ObHeatmapAlign, MaxHoldPerFootprintBar)
TEST(ObHeatmapAlign, MinQtyFilterHidesNoise)
TEST(ObHeatmap, MemoryCapEvictsOldest)
```

---

## 8. Acceptance checklist

- [ ] Heatmap lines up with footprint time axis (no systematic drift).
- [ ] Large resting orders leave visible horizontal trails.
- [ ] Toggle/opacity work; default opacity doesn’t hide footprint text.
- [ ] Shared history with live heatmap (no double-sample of L2).
- [ ] Gates green; no hot-path work.

---

## 9. Non-goals

- Reconstructing full historical MBO from REST (exchange often lacks it).
- Automatic spoof labels.
- Disk persistence of depth rings.

---

## 10. Codex task prompt

```
Implement cyrex/orderflow/04-orderbook-heatmap.md only.
Reuse BookHistoryRing; add multi-res history and footprint overlay draw.
Unit-test alignment (max-hold per bar) and filters.
```
