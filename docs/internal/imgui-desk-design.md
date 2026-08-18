# ImGui Strategy Desk — Design Note

**Status**: Active (v4 workspace shell: MARKET/RESEARCH/OPERATIONS/DIAGNOSTICS; Research data wiring pending)  
**Date**: 2026-08-03 (v4 workspace redesign: 2026-08-18)  
**Scope**: Personal attended client for Develop → Research → Monitor → Execute  
**Non-scope**: Multi-tenant SaaS inside the engine; in-desk C++ strategy IDE

## v4 redesign summary (2026-08-18)

v3 shipped five fully-defined workspaces (Orderflow/Liquidity/Structure/
Markets/Operations) but kept only `Monitor` (a sixth, minimal page) active in
`desk_pages` — a deliberate interim state (see the v3 delivery checklist
below). v4 replaces that shape with four top-level operator workspaces and
retires `Monitor` (its content is redistributed: symbol-filtered positions/
activity moved to MARKET, all-symbol activity/health/risk stayed in
OPERATIONS):

- **MARKET** — the former Orderflow/Liquidity/Structure/Markets pages, now
  reachable as an internal view switcher (Footprint · Liquidity · Structure ·
  Cross-market) under one workspace tab instead of four separate top-level
  entries. Each subview keeps its own dockspace/geometry/panel set —
  `DeskPage` stays the fine-grained layout unit; `DeskWorkspace` is the new,
  coarser grouping the operator actually navigates (`desk_layout_model.h`).
- **OPERATIONS** — the former Operations page, unchanged in content, plus two
  strips drawn above its dockspace: the trimmed Account strip (7 metrics —
  see below) and the previously-unwired `draw_safety_strip` (Safety Status:
  halt/pause/kill-hook/provider/stream, deliberately separate from the
  dockable Risk panel's measured limits).
- **DIAGNOSTICS** — new page hosting the existing (previously unwired) Debug
  panel (`panels/debug_panel.cpp`) as its own top-level home instead of
  engineering telemetry having no page at all.
- **RESEARCH** — new page (`panels/research_workspace_panel.cpp`), Setup /
  Report / Monte Carlo / Replay tabs. No isolated backtest-launcher,
  resolved-config preview, or AnalyticsReport/MC-report seam exists in the
  desk yet, so every tab renders an honest NOT WIRED / UNAVAILABLE state
  rather than reimplementing config precedence or inventing report data —
  see "Research: current vs future wiring" below.

Layout bumped to **v4** (`truetest_desk_v4.ini`) — v3's Monitor-only layout
cannot be silently reinterpreted as the new shape.

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

Top-level navigation (`DeskWorkspace` in `desk_layout_model.h`), left to
right in the chrome: **MARKET · RESEARCH · OPERATIONS · DIAGNOSTICS**.
`DeskPage` remains the actual dockspace/geometry/panel-assignment unit —
`desk_workspace_of(DeskPage)` maps each page to its workspace, and
`desk_workspace_pages(DeskWorkspace)` returns the page(s) reachable from a
workspace (Market has four; the other three have exactly one each).

| Workspace | Subview / page | Primary surface | Secondary surfaces |
|-----------|-----------------|-----------------|--------------------|
| MARKET | Footprint (`orderflow`) | Footprint/orderflow canvas | Linked watchlist, DOM, selected context, symbol-filtered activity blotter |
| MARKET | Liquidity | Historical L2 heatmap | DOM, liquidation clusters and tape |
| MARKET | Structure | TPO / market profile | Volume profile and session context |
| MARKET | Cross-market (`markets`) | Rolling correlation | Funding intelligence |
| OPERATIONS | `operations` | Equity/drawdown | Strategies, risk, system health, all-symbol activity; Account + Safety Status strips above the dockspace |
| DIAGNOSTICS | `diagnostics` | Debug (build/threading/rings/pools/engine-state/stage-timings/memory/subsystem errors) | — (single dominant pane) |
| RESEARCH | `research` | Setup/Report/Monte Carlo/Replay tabs, capability-gated NOT WIRED content | — |

The default Market/Footprint layout is optimized for a 2560×1440 single
monitor: 14% watchlist, 68% primary canvas, 18% DOM/context rail, and a
20%-high collapsible activity blotter under the watchlist/canvas. Diagnostics
and Research use a single dominant pane (no left/right/bottom splits).
Versioned persistence uses `truetest_desk_v4.ini`; temporary focus mode
snapshots and restores the normal ImGui layout while using separate
transient dockspaces.

A compact **Market metric band** (mid/last, spread bps, microprice,
imbalance, update rate, queue position) renders above the dockspace whenever
a MARKET subview is active and a snapshot exists — deliberately excluding
account/build metrics, which stay in Operations/Diagnostics.

### MIXED SOURCES

The desk can simultaneously show a real/live footprint (via
`set_live_footprint_source`) alongside sibling research surfaces still
published from the demo fixture. `research_presentation_has_mixed_sources()`
(`research_views.h`) classifies each surface's `DeskDataState` into `demo` /
`real` / `none` and flags the aggregate as mixed only when both a demo and a
real surface are simultaneously present — a `MIXED SOURCES` badge then
appears in global chrome. This is in addition to, never a replacement for,
each panel's own per-surface provenance header; one live surface can never
make its demo siblings read as live.

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

### Safety vs Risk (OPERATIONS)

The Risk panel (`panels/status_panels.cpp::draw_risk_panel`) and the Safety
Status strip (`draw_safety_strip`) are deliberately separate surfaces, not
one merged "risk & safety" panel:

- **Risk** owns *measured limits*: drawdown/limit, exposure/limit, open
  orders/limit gauges. An absent limit renders "no limit configured", never
  0%/"safe".
- **Safety Status** owns the terminal-halt / pause / kill-hook / provider /
  stream distinction: `HALT SET — RESTART` vs `HALT not set` (never "clear
  halt"), strategies blocked-by-halt vs paused vs enabled, whether the kill
  hook exists at all, and provider/stream lifecycle. Roadmap safety states
  (DMS countdown, external watchdog, reconciler detail, ambiguous kill
  result) are not inferred from these fields — they simply have no row until
  a trustworthy seam exists.

### Capability model

`desk_capabilities.h` provides a small, pure, cold `DeskCapabilities` struct
(`derive_desk_capabilities(...)`) answering "is X actually available right
now" (snapshot, pause/flatten/kill hooks, debug telemetry, QuestDB active,
a research surface present) from data the desk already holds — never
inferred, never defaulted to available. Fields for roadmap seams that do not
exist yet (`research_report_available`, `research_launcher_available`,
`research_resolved_config_available`) stay explicitly `false`. This is not
an engine state machine; it exists to stop re-deriving the same presence
checks ad hoc across panels (currently consumed by the RESEARCH workspace
panel; not yet threaded through every panel that could use it).

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

## 9a. DIAGNOSTICS role

DIAGNOSTICS hosts the existing Debug panel content (build/target flags,
threading, rings, pools, engine-state counters, stage timings — build-
dependent on `ENABLE_DEBUG`, "N/A — build with ENABLE_DEBUG" when absent —
process memory, pool/ring memory estimates, subsystem error strings) as its
own top-level workspace rather than leaving it unreachable from any page.
It is intentionally less polished than MARKET/OPERATIONS for a non-developer
operator, but never implies health from the absence of error text.

## 9b. RESEARCH: current vs future wiring

RESEARCH (`panels/research_workspace_panel.cpp`) is deliberately built as a
capability-gated shell, not a partial implementation dressed up as
complete:

| Tab | State today | What's missing |
|-----|-------------|-----------------|
| Setup | Reference-only description of the CLI/JSON config surface (`--config`/`--preset`/`--dry-run`/`--dump-config`) | No resolved-configuration preview and no in-desk backtest launch — see seams below |
| Report | UNAVAILABLE | No `AnalyticsReport` source is ever wired to `research_fn_`; the desk's research callback only ever publishes market-structure `ResearchPresentation` data |
| Monte Carlo | NOT WIRED | No campaign setup/trial/seed/aggregate/distribution/drill-down surface exists |
| Replay | NOT WIRED | `--replay` remains CLI-only; no in-desk control to trigger or browse a replay run |

**Seams a future session would need** (do not build ad hoc without design
review, per the redesign brief's launch-boundary guidance):

1. A cold-path call into the existing dry-run/resolved-config resolver,
   invokable without starting an engine run, to power the Setup tab's
   resolved-configuration preview.
2. An isolated `engine_backtest` child-process launch API — a heavy backtest
   must never run on the attached shadow/live engine event loop. If this
   already exists elsewhere in the tree, integrate it; if not, this redesign
   deliberately does **not** invent a new process-orchestration subsystem to
   fill the gap.
3. An `AnalyticsReport`/MC-report publication path into `research_fn_` (or a
   sibling callback), following the same immutable-snapshot pattern as
   `ResearchPresentation`.

## 10. Shortcuts

| Key | Action |
|-----|--------|
| `Ctrl+K` | Command palette (includes all four workspaces, Market subviews, reset layout, focus primary, layout lock, density, demo toggle) |
| `F1` | Help overlay |
| `F11` | Focus/restore primary surface |
| `P` | Pause / resume (bare key; suppressed while typing, palette open, or a confirm modal is open) |
| `F` | Flatten (confirm modal) |
| `K` | Kill switch (confirm modal) |
| `Esc` | Cancel confirm / close help |

Top chrome carries two rows: **WORKSPACE** (MARKET · RESEARCH · OPERATIONS ·
DIAGNOSTICS) and, only while MARKET is active, **MARKET VIEW** (Footprint ·
Liquidity · Structure · Cross-market). Selecting MARKET from another
workspace returns to whichever Market subview was last active
(`last_market_view_`), not always Footprint.

## 11. Delivery checklist

| Phase | Deliverable |
|-------|-------------|
| 1 | Five-workspace v2 shell, versioned docking, command palette, typography and deterministic demo fixtures |
| 2 | Cold `ResearchStore` + public tape + integer tick/session/camera primitives |
| 3 | DOM/footprint/volume profile wiring |
| 4 | Book-history heatmaps, TPO and liquidation wiring |
| 5 | Funding/correlation sources and interactive detail |
| 6 (**this redesign, 2026-08-18**) | v4: four-workspace shell (MARKET/RESEARCH/OPERATIONS/DIAGNOSTICS) replacing the v3 Monitor-only shape; Safety Status and Debug panels wired to real pages; capability model + mixed-source provenance badge; RESEARCH shell honestly NOT WIRED pending the seams in §9b |
| 7 | Research report/backtest launcher and MC review (needs §9b seams) |

---

*Pairs with plan session 2026-08-02 ImGui Strategy Desk.*
