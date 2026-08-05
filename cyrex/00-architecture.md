# Cyrex Architecture — Shared Codex Constraints

**Audience**: Any agent implementing a file under `cyrex/`.  
**Reference**: [Cryexc architecture](https://josedonato.com/blog/dear-imgui-emscripten-trading-terminal-architecture/) · [Cryexc app](https://cryexc.josedonato.com/app)  
**TrueTest desk design**: `docs/internal/imgui-desk-design.md`

---

## 1. Product split

| Concern | Cryexc (reference) | TrueTest (this repo) |
|---------|--------------------|----------------------|
| Live public feeds | Browser WS → WASM | Provider stack (`src/providers/*`) → engine events |
| Research math | C++ in WASM render loop | Cold-path C++ aggregators (new under `src/analytics/cyrex/` or `src/research/`) |
| UI | ImGui → WebGL canvas | ImGui desk (`src/ui/desk/`) primary; React SPA optional later |
| Execution | None (analysis only) | Existing engine; research pages are **read-only** |

Cyrex features are **research/monitor panels**. They must not submit orders, clear halt, or bypass risk.

---

## 2. Process model (mandatory)

```
Provider threads ──► engine event loop (HOT, zero-alloc)
                         │
                         │ publish_event / orderbook apply
                         │
         ┌───────────────┼────────────────────────────┐
         │               │                            │
         ▼               ▼                            ▼
   strategies      snapshot_dashboard()        ResearchAggregator
   (hot)           under engine lock           (COLD worker or UI)
                         │                            │
                         └──────────┬─────────────────┘
                                    ▼
                          UI thread 10–30 Hz
                          ImGui panels (desk)
```

### Rules

1. **No** footprint maps, heatmap textures, correlation matrices, or profile rebuilds on the engine event loop.
2. ResearchAggregator may:
   - Subscribe via a **dedicated SPSC** of lightweight DTOs (not full `shared_ptr<event>` if that dual-produces existing rings), **or**
   - Be fed from an existing observer/logging worker path, **or**
   - Rebuild from a **locked snapshot** of book + recent trades copied under the same lock as `snapshot_dashboard()`.
3. Prefer **copy-out under lock, compute unlocked** (same pattern as desk snapshot).
4. UI never blocks the engine; drop research frames under load (fail soft for viz, fail closed only for safety).

---

## 3. Suggested module layout

```
src/research/cyrex/                 # OR src/analytics/cyrex/
  types.h                           # price_bin, side_qty, grid dims
  trade_tape_window.h               # ring of recent public trades
  book_history_ring.h               # time × price depth columns
  footprint_model.h / .cpp
  volume_profile_model.h / .cpp
  tpo_model.h / .cpp
  live_heatmap_model.h / .cpp
  orderbook_heatmap_model.h / .cpp
  liquidation_model.h / .cpp
  correlation_model.h / .cpp
  funding_arb_model.h / .cpp
  research_snapshot.h               # POD grids for UI

src/ui/desk/panels/cyrex/
  dom_panel.cpp
  footprint_panel.cpp
  ...

tests/test_cyrex_*.cpp              # pure model unit tests first
```

Register every new `.cpp` in `cmake/Sources.cmake`. Gate ImGui panels with `HAS_IMGUI_DESK`.

---

## 4. Data contracts (canonical DTOs)

Use fixed-point friendly doubles for UI; keep internal bins on integer ticks when possible.

```cpp
// Conceptual — implement in research/cyrex/types.h

struct CyrexTrade {
  std::int64_t ts_ms;
  double price;
  double qty;
  bool is_buyer_maker; // true => aggressive sell (hit bid)
  std::uint32_t symbol_id;
};

struct CyrexBookLevel {
  double price;
  double qty;
};

struct CyrexBookSnapshot {
  std::int64_t ts_ms;
  std::uint32_t symbol_id;
  // top-N or full local book depending on stream
  std::vector<CyrexBookLevel> bids; // high -> low
  std::vector<CyrexBookLevel> asks; // low -> high
};

struct PriceAxis {
  double tick;          // e.g. 0.1 for BTCUSDT
  double min_price;     // inclusive floor of active window
  int    n_bins;
  int price_to_bin(double px) const;
  double bin_to_price(int bin) const;
};
```

**Tick size**: resolve from instrument metadata at startup (`instrument_spec_cache` / exchange filters). Do not hardcode 0.1.

---

## 5. Snapshot extension strategy

Do **not** dump multi-MB grids into `dashboard_snapshot` every 100 ms by default.

Preferred:

1. `ResearchStore` owned by desk/UI thread or cold worker.
2. Desk panels call `research_store.view_footprint(symbol)` etc.
3. Optional: thin `dashboard_snapshot` fields for “last trade / mid / L2 top-20” only (already exist as `l2`).

If web SPA needs parity later: add `/api/research/{view}` that serializes **already aggregated** grids (cold path, allow-listed JSON only in web layer).

---

## 6. UI conventions (ImGui desk)

| Pattern | Detail |
|---------|--------|
| Window names | Add `DeskPanel` enum values + `desk_window_names.h` entries |
| Dock pages | Prefer new `DeskPage::orderflow` and `DeskPage::structure` (or Analysis sub-dock) |
| Theme | Reuse `ui/desk/desk_theme.h` (`up`/`down`/`accent`/`tx_*`) |
| Charts | ImPlot for series; custom `ImDrawList` for footprint cells / heatmap quads |
| Controls | Symbol, tick multiple, session TZ, lookback — local panel state, not engine config |
| Empty states | Match existing “No L2 depth / enable --depth-stream” honesty |

Existing L2 panel reference: `src/ui/desk/panels/market_panels.cpp` (`draw_l2_panel`).

---

## 7. Performance budgets (desk, cold path)

| View | Target recompute | Render |
|------|------------------|--------|
| DOM | every UI tick from snapshot | table + bars < 1 ms |
| Footprint | dirty-flag on trade batch | LOD draw 4–6 ms full layout (Cryexc ballpark) |
| Heatmaps | append column on interval; GPU texture optional phase 2 | avoid >10k ImDrawList rects |
| Profiles | dirty-flag on trade/bar | histogram once |
| Correlation / funding | 1–5 s poll | tables/heat matrix |

Version counters:

```cpp
std::uint64_t data_version = 0; // bump on ingest
std::uint64_t draw_version = 0; // last rebuilt cache
if (data_version != draw_version) rebuild();
```

---

## 8. Provider / feed prerequisites

| Need | TrueTest today | Action |
|------|----------------|--------|
| Public trades / tape | tick/trade path via Binance | Ensure research path sees **public** trades, not only strategy fills |
| L2 depth | `l2_snapshot`/`l2_update`, `--depth-stream` | Required for DOM + heatmaps |
| Liquidations | not first-class everywhere | Add forceOrder (or venue equiv) as cold events |
| Funding | `funding_event` partial | Extend to mark/index/predicted rate for arb table |
| Multi-symbol | strategy multi-sym | Correlation needs N mids |
| Multi-venue | Binance golden; Bitget landing | Phase multi-venue **after** single-venue correctness |

---

## 9. Testing strategy (all features)

1. **Pure model tests** with synthetic trades/books (no ImGui, no network).
2. **Golden fixtures** for footprint bins / profile POC after fixed trade lists.
3. **No hotpath alloc tests** broken: research code must not be linked into hot publish path.
4. Gate scripts after `src/` edits:  
   `./scripts/check-hotpath-json.sh && ./scripts/check-layer-deps.sh && ./scripts/check-live-safety-freeze.sh`
5. Optional visual: manual `engine_shadow --desk --depth-stream ...`.

---

## 10. Non-goals (global)

- Cloning Cryexc WASM/Emscripten browser packaging.
- Multi-tenant SaaS research service inside `engine_*`.
- Spoof/iceberg ML classifiers in MVP (velocity coloring is enough).
- Live order placement from research panels.
- Expanding JSON hotpath allow-list.

---

## 11. Codex preamble (paste into every feature task)

```
You are implementing one Cyrex research feature in TrueTest core.

Read first:
- cyrex/00-architecture.md
- the specific feature markdown under cyrex/
- AGENTS.md (hot path, layers, Sources.cmake)
- docs/internal/imgui-desk-design.md

Constraints:
- Cold-path only for research math
- Primary UI: src/ui/desk/ under HAS_IMGUI_DESK
- Single-venue Binance futures first unless feature says otherwise
- Unit-test models before wiring UI
- Do not modify live-safety freeze files
```
