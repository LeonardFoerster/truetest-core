# ImGui Strategy Desk — Design Note

**Status**: Active (workspace shell v2; research data wiring pending)  
**Date**: 2026-08-03  
**Scope**: Personal attended client for Develop → Research → Monitor → Execute  
**Non-scope**: Multi-tenant SaaS inside the engine; in-desk C++ strategy IDE

---

## 1. Goals

1. Replace the embedded React SPA as the **human** cockpit for personal use.
2. Cover the strategy lifecycle without a browser: configure/run research, review reports, monitor shadow/live, operate pause/flatten/kill.
3. Stay off the hot path; consume only existing seams.
4. Leave a clean SaaS boundary: orchestrator stays outside `engine_*`.

## 2. Process & thread model

```
engine event loop (hot) ──lock──► snapshot_dashboard()
                                        ▲
UI thread (desk) ──poll 10–30 Hz────────┘
              ──input──► operator_actions (pause / flatten / kill)
              ──cold───► AnalyticsReport (research mode)
```

- **In-process** desk (same binary as `engine_shadow` / `engine_live`; later `engine_backtest` for research viewer).
- UI runs on its **own thread** (same pattern as `TabbedDashboard`).
- No ImGui / GLFW / OpenGL / JSON on the engine event loop.
- Research runs (Phase 4): **spawn** isolated `engine_backtest` child; Monitor/Execute attach to the current process (**hybrid launch**).

## 3. Sacred seams

| Seam | Role |
|------|------|
| `engine.snapshot_dashboard()` → `ui::dashboard_snapshot` | Existing account, execution, safety and engine-health data |
| optional immutable `ResearchPresentation` handle | Cold, already-aggregated orderflow/market-structure views |
| `ui::operator_actions` | Pause / flatten / kill (shared with rich TUI) |
| `Analytics::generate_report()` | Research / post-run |
| Strategy registry | Name + `--param` only (no runtime code load) |

**Freeze surface**: desk must not bypass the live-safety freeze. The authoritative path list is `scripts/check-live-safety-freeze.sh`; controls call public APIs already used by the TUI.

Large footprint/heatmap/profile grids never enter `dashboard_snapshot`. The
desk polls an optional research callback for a shared immutable presentation
handle; without one it renders an honest unavailable state. A deterministic
local fixture can be enabled manually and is persistently labeled `DEMO DATA`.

## 4. Workspaces

| Workspace | Primary surface | Secondary surfaces |
|-----------|-----------------|--------------------|
| Orderflow | Footprint/orderflow canvas | Linked watchlist, DOM, selected context, compact tabbed activity blotter |
| Liquidity | Historical L2 heatmap | DOM, liquidation clusters and tape |
| Structure | TPO / market profile | Volume profile and session context |
| Markets | Rolling correlation | Funding intelligence; detail opens later from an explicit selection |
| Operations | Equity/drawdown | Strategies, risk, system health and all-symbol activity |

The default Orderflow layout is optimized for a 2560×1440 single monitor:
14% watchlist, 68% primary canvas, 18% DOM/context rail, and a 20%-high
collapsible activity blotter under the watchlist/canvas. Versioned persistence
uses `truetest_desk_v2.ini`; temporary focus mode snapshots and restores the
normal ImGui layout while using separate transient dockspaces.

## 5. Presentation contract

- Every research surface independently exposes `unavailable`, `demo`, `live`,
  `stale`, or `error` status plus source, age, and version. A partial publication
  therefore cannot label an absent view `LIVE`.
- Research views are read-only and never place orders or interpret own fills as
  the public trade tape.
- The linked desk context carries symbol, venue, interval, session, and future
  shared time/price camera bounds. The v2 shell currently links symbol
  selection; camera synchronization lands with the real research store.
- Dense heatmaps/footprints use custom draw lists, viewport-oriented LOD, and a
  hard 12,000-cell presentation budget. Profiles, correlation, liquidation
  overlays, and tables are also capped or clipped. Frame-owned ImDrawList
  objects are not cached.
- Existing snapshot values are recomposed rather than recalculated. Unknown
  Operations state is shown as unavailable, never as zero or safe.

Honest series only — no unlabeled synthetic sparklines.

## 6. Control policy

- Confirm modals for flatten and kill (TUI parity).
- Halt is terminal: UI may acknowledge, never auto-clear.
- Kill absent (e.g. backtest) → toast “unavailable”.
- No HTTP control routes.

## 7. Build / deps

| Item | Choice |
|------|--------|
| Flag | `ENABLE_IMGUI` (default OFF for CI) |
| UI | Dear ImGui + ImPlot; bundled OFL IBM Plex Sans/Mono |
| Platform | GLFW + OpenGL3 (system packages) |
| Define | `HAS_IMGUI_DESK` on wired binaries |
| CLI | `--desk` starts desk; mutually exclusive with rich TUI |

Optional `ENABLE_WEB` remains for **API-only** snapshot proxy (monorepo); SPA is not required for personal use.

## 8. Layering

```
src/ui/desk/     ImGui presentation (cold)
src/ui/          snapshot types, operator_actions, format helpers
src/web/         optional HTTP serializers (no desk dependency)
src/engine/      no ImGui includes
```

## 9. SaaS boundary (later)

- Engine stays single-run compute; no tenants/auth in desk or engine.
- SaaS orchestrator spawns processes; remote viewers are read-only snapshot consumers.
- Personal desk stays attended single-operator.

## 10. Delivery checklist

| Phase | Deliverable |
|-------|-------------|
| 1 | Five-workspace v2 shell, versioned docking, command palette, typography and deterministic demo fixtures |
| 2 | Cold `ResearchStore` + public tape + integer tick/session/camera primitives |
| 3 | DOM/footprint/volume profile wiring |
| 4 | Book-history heatmaps, TPO and liquidation wiring |
| 5 | Funding/correlation sources and interactive detail |
| 6 | Research report/backtest launcher and MC review |

---

*Pairs with plan session 2026-08-02 ImGui Strategy Desk.*
