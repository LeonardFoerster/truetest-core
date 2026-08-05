# Cyrex — Orderflow & Market Structure Research Specs

**Purpose**: Codex-ready build specs for a Cryexc-class research surface inside TrueTest.  
**Reference product**: [Cryexc](https://cryexc.josedonato.com/) · launch app `/app` · architecture notes on [José Donato’s blog](https://josedonato.com/blog/dear-imgui-emscripten-trading-terminal-architecture/).  
**Status**: Specs remain unwired; the native desk v2 presentation shell and
explicit deterministic demo views now live under `src/ui/desk/`. No Cyrex
analytics/provider data plane is implemented yet.  
**Date**: 2026-08-03

---

## What this is

Each markdown file is a **standalone Codex instruction packet**: goal, data contracts, algorithms, UI wiring into TrueTest’s ImGui desk (primary) and optional web SPA, tests, acceptance criteria, and non-goals. Agents should implement **one feature file at a time**.

Naming note: directory is `cyrex` (project codename). Product reference remains **Cryexc**.

---

## Feature map

### Orderflow

| # | Feature | Spec | Priority |
|---|---------|------|----------|
| 1 | DOM / Depth of Market Ladder | [`orderflow/01-dom-ladder.md`](orderflow/01-dom-ladder.md) | P0 |
| 2 | Footprint Chart | [`orderflow/02-footprint-chart.md`](orderflow/02-footprint-chart.md) | P0 |
| 3 | Live Heatmap | [`orderflow/03-live-heatmap.md`](orderflow/03-live-heatmap.md) | P1 |
| 4 | Orderbook Heatmap | [`orderflow/04-orderbook-heatmap.md`](orderflow/04-orderbook-heatmap.md) | P1 |

### Market structure

| # | Feature | Spec | Priority |
|---|---------|------|----------|
| 5 | Volume Profile | [`market-structure/01-volume-profile.md`](market-structure/01-volume-profile.md) | P0 |
| 6 | Market Profile / TPO | [`market-structure/02-market-profile-tpo.md`](market-structure/02-market-profile-tpo.md) | P1 |
| 7 | Liquidation Heatmap | [`market-structure/03-liquidation-heatmap.md`](market-structure/03-liquidation-heatmap.md) | P1 |
| 8 | Market Correlation | [`market-structure/04-market-correlation.md`](market-structure/04-market-correlation.md) | P2 |
| 9 | Funding Rate Arbitrage | [`market-structure/05-funding-rate-arbitrage.md`](market-structure/05-funding-rate-arbitrage.md) | P2 |

Shared architecture constraints: [`00-architecture.md`](00-architecture.md).

---

## Recommended build order

```
DOM ladder → Volume profile → Footprint → Live heatmap → Orderbook heatmap
  → TPO → Liquidation tape (+ optional model heatmap) → Funding table → Correlation
```

Rationale: each step reuses the previous cold-path aggregator and desk panel patterns; heatmaps need history rings that DOM/footprint already force into existence.

---

## TrueTest seams (do not invent parallel engines)

| Seam | Role |
|------|------|
| Provider → `event` stream (`trade`/`tick`, `l2_snapshot`/`l2_update`, `funding`) | Sole venue ingress |
| `orderbook_registry` + L2 events | Book state |
| Cold-path **research aggregator** (new, off hot loop) | Footprint bins, depth history, profiles |
| `engine.snapshot_dashboard()` / extended research snapshot | UI read path |
| `src/ui/desk/` ImGui panels | Primary human UI (`ENABLE_IMGUI`) |
| `src/web/` snapshot JSON (optional) | Remote/read-only consumers |

**Hard rules** (from `AGENTS.md`):

- Zero heap / no JSON / no ImGui on the engine hot path.
- Research math runs on UI thread, observer worker, or dedicated cold worker — never inside strategy callbacks or publish path.
- Do not touch the live-safety freeze surface for these features.
- Register sources in `cmake/Sources.cmake` (no globs).

---

## Cryexc reference behaviors to copy (UX, not code)

From the public product + architecture write-up:

1. **Immediate-mode draw** — every frame reads current memory; no retained DOM tree of book levels.
2. **Producer/consumer split** — WS/engine fills rings; render loop drains; neither blocks the other.
3. **Dirty-flag caches** — footprint / TPO / volume profile recompute only when data version changes (full rebuild every frame is too slow).
4. **LOD / visibility culling** — zoomed-in: full bid/ask text; zoomed-out: color blocks only.
5. **Heatmap as texture (later)** — liquidity grid → GPU float texture + fragment shader; not thousands of ImDrawList rects at wide zoom.
6. **Multi-exchange aggregation (later)** — normalize trades/depth to a common book; first ship **single venue** (Binance futures golden path).

---

## How to use with Codex

```
Implement ONLY the feature described in:
  cyrex/orderflow/01-dom-ladder.md

Follow TrueTest AGENTS.md hot-path and layer rules.
Do not implement other cyrex features in the same change.
```

Each feature file ends with an **Acceptance checklist** Codex must satisfy before claiming done.
