# Codex Spec — Live Heatmap

**ID**: `cyrex-orderflow-live-heatmap`  
**Priority**: P1  
**Reference**: Cryexc “Live heatmap / orderbook heatmap” — real-time depth intensity, velocity coloring, trade clustering  
**Depends on**: L2 stream + price axis helpers from DOM/footprint

---

## 1. Goal

Show a **scrolling heatmap of liquidity**: X = time, Y = price, color = resting size (and optionally rate of change). This is the “Bookmap-style” live depth tape Cryexc advertises as Live Heatmap.

Distinguish from **Orderbook Heatmap** (`04`):  
- **Live Heatmap** = short rolling window, high temporal resolution (e.g. 50–200 ms columns), focuses on **current session microstructure**.  
- **Orderbook Heatmap** = longer history aligned under price chart / footprint candles.

Implement shared `BookHistoryRing`; specialize views.

---

## 2. User-visible behavior

- Full-window or docked panel heatmap.
- Mid/last price line overlaid.
- Trades plotted as dots/crosses sized by qty (cluster).
- Color scale: viridis or mono-amber on dark theme (Cryexc uses continuous ramp); legend with min/max qty.
- Optional **velocity mode**: color by Δqty since previous column (adds green/red for add/cancel).
- Controls: column interval, price tick multiple, depth range (bps or fixed bins around mid), follow mid (yes/no), log scale qty.

---

## 3. Data structure

```cpp
class BookHistoryRing {
public:
  void configure(PriceAxis axis, int max_columns, int column_interval_ms);
  // Called on timer or when book version changes enough:
  void sample(std::int64_t ts_ms, std::span<const CyrexBookLevel> bids,
              std::span<const CyrexBookLevel> asks);
  void on_trade(const CyrexTrade&); // for overlay points

  struct Column {
    std::int64_t ts_ms;
    // qty_bid[bin] + qty_ask[bin]  or signed: +bid -ask
    std::vector<float> bid_qty; // size = axis.n_bins
    std::vector<float> ask_qty;
    std::vector<float> net_qty; // bid-ask or total liquidity
  };

  std::span<const Column> columns() const;
  std::uint64_t version() const;
};
```

### Sampling rules

1. Maintain latest local book from L2 snapshots/updates (can reuse engine book via snapshot copy).
2. Every `column_interval_ms`, **or** on significant book change (optional), snapshot quantities into bins around mid:
   - `window_bins` above and below mid (e.g. ±150 ticks).
3. When mid drifts, **slide PriceAxis** and shift bin arrays (or rebuild axis and accept reset — MVP rebuild is OK with flash).
4. Cap `max_columns` (e.g. 600 → 60s at 100ms). Drop oldest.

### Velocity column (optional mode)

```
vel[bin] = net_qty[t][bin] - net_qty[t-1][bin]
```

Color bipolar: adds warm, cancels cool (or green/red).

---

## 4. Render strategies

### Phase 1 — ImDrawList rects (ship first)

```cpp
for (col, x) in visible_columns:
  for (bin, y) in visible_bins:
    c = color_map(qty[col][bin], vmin, vmax)
    draw->AddRectFilled(ImVec2(x0,y0), ImVec2(x1,y1), c);
```

Budget: keep visible cells < ~15k. If more, increase bin size or skip columns.

### Phase 2 — GPU texture (Cryexc experiment)

- Pack `columns × bins` into `GL_R32F` texture.
- Draw one textured quad; fragment shader maps value → colormap.
- Upload only dirty column strips (`glTexSubImage2D`).
- Wire via existing OpenGL3 ImGui backend in desk.

Codex: implement Phase 1 completely; leave Phase 2 behind `// TODO` interface `HeatmapRenderer`.

---

## 5. Trade overlay

Keep short ring of trades in window:

```cpp
draw trade at (time_to_x(ts), price_to_y(px))
  radius ∝ log(qty)
  color = aggressor buy/sell
```

---

## 6. Wiring

| Component | Role |
|-----------|------|
| Cold worker / UI tick | `BookHistoryRing::sample` from copied L2 |
| `LiveHeatmapModel` | owns ring + color scale stats (EMA max) |
| `draw_live_heatmap_panel` | camera, controls, draw |
| DeskPage orderflow | secondary or full-bleed slot |

**Snapshot policy**: do not put full ring in `dashboard_snapshot`. Keep in research store.

Feed requirement: venue L2 (`--depth-stream`). Synthetic MM book may be labeled “SYNTH” like existing L2 panel.

---

## 7. Tests

```
TEST(BookHistoryRing, SamplesIntoCorrectBins)
TEST(BookHistoryRing, DropsOldColumns)
TEST(BookHistoryRing, VelocityIsDiffOfColumns)
TEST(BookHistoryRing, MidSlideKeepsAxisConsistent) // if implemented
TEST(ColorScale, LogAndLinearMap)
```

---

## 8. Acceptance checklist

- [ ] Heatmap advances in time with live L2.
- [ ] Liquidity walls visible as bright horizontal bands.
- [ ] Trade dots align with tape/footprint times.
- [ ] Follow-mid keeps action centered.
- [ ] No engine hot-path sampling; gates green.
- [ ] Performance: UI tick stays interactive on BTC L2 (manual).

---

## 9. Non-goals

- Spoof/iceberg classifier ML.
- Multi-venue aggregated depth heatmap (phase 2).
- Historical multi-hour ring at 50ms (use orderbook heatmap downsampling).

---

## 10. Codex task prompt

```
Implement cyrex/orderflow/03-live-heatmap.md only.
BookHistoryRing + LiveHeatmapModel + ImGui panel (ImDrawList Phase 1).
Sample L2 off hot path; unit-test binning and ring eviction.
```
