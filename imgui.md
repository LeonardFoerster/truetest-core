# TrueTest ImGui Cockpit — Codex Implementation Brief

**Document type:** Agent / Codex build instructions (authoritative product + engineering brief)  
**Status:** Living spec for branch `imgui`  
**Last updated:** 2026-08-02  
**Audience:** Codex, Grok Build, Claude Code, human implementers  

**Goal:** Build a **single personal ImGui cockpit** that is simultaneously:

1. **Trading UI** — live/shadow market, positions, book, fills, operator controls  
2. **Dashboard** — risk, health, latency, engine internals, session status  
3. **Analysis platform** — backtest tear sheets, trade blotter, Monte Carlo, run comparison  

…and acts as the **primary human interface to the entire engine** without a browser.

This is **not** a SaaS product UI. It is an attended, single-operator, high-density desk for research → shadow → live.

---

## 0. How to use this document (Codex instructions)

When implementing:

1. **Read this file fully** before coding.  
2. **Read** `docs/internal/imgui-desk-design.md`, `AGENTS.md`, and existing `src/ui/desk/*`.  
3. **Never invent parallel data paths.** Only use sacred seams (§3).  
4. **Ship vertical slices** (§12). Each slice must leave the binary usable.  
5. **Prefer binding C++ structs** (`dashboard_snapshot`, `AnalyticsReport`) over JSON for in-process desk.  
6. **Run gates after any `src/` edit:**  
   `./scripts/check-hotpath-json.sh && ./scripts/check-layer-deps.sh && ./scripts/check-live-safety-freeze.sh`  
7. **Do not edit freeze-surface files** unless human CCB + `LIVE_SAFETY_CCB_APPROVED` token. Desk must call public APIs already used by the TUI.  
8. **Honest UI only:** no unlabeled synthetic sparklines; label demo/fixture data.  
9. **Build flag:** `ENABLE_IMGUI=ON` → `HAS_IMGUI_DESK`. CI default stays OFF.  
10. **CLI:** `--desk` starts the cockpit; mutually exclusive with rich ncurses TUI when both exist.

### Build & smoke

```bash
cmake -B build-imgui -DENABLE_IMGUI=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-imgui -j --target engine_shadow engine_live

./build-imgui/engine_shadow \
  --provider synthetic --strategy sma --desk --no-tui --status-format off --seed 1
# Desk stays open on final snapshot until window closed (batch path).

# Streaming / shadow (when provider available):
./build-imgui/engine_shadow --provider binance ... --desk --no-tui
```

### Definition of “usable”

An operator can, **without a browser and without memorizing every CLI flag**:

- See mode (BACKTEST / SHADOW / LIVE), halt, pause, data freshness  
- Monitor book, positions, brackets, fills, equity, risk, strategies  
- Pause / flatten / kill (with confirms)  
- Configure and run a backtest, view equity + trade list + metrics  
- Run MC (when ready), inspect trial distributions, re-run a seed  
- Export/report artifacts with seed + flags + build identity  

---

## 1. Product vision

### 1.1 North-star daily loop

```
Develop  →  Research  →  Monitor (shadow)  →  Execute (live, attended)
   │            │              │                      │
 config      tear sheet    ticker/book/pos         pause/flat/kill
 spawn BT    blotter/MC    risk/health             mode badges
```

### 1.2 Modes (one app, four workspaces)

| Mode | Binary context | Primary data | Controls |
|------|----------------|--------------|----------|
| **Monitor** | `engine_shadow` / `engine_live` in-process | `dashboard_snapshot` @ 10–30 Hz | pause / flatten / kill |
| **Execute** | same + safety chrome | same | same + LIVE arming UX |
| **Research** | post-run or load file | `AnalyticsReport` / `--output` JSON | none (analysis only) |
| **Develop** | launcher UI | spawns `engine_backtest` / MC child | start / cancel job |

**Hybrid process model (mandatory):**

- Shadow/live desk = **in-process** UI thread on running engine.  
- Backtest/MC from Develop = **spawn isolated child** (same pattern as monorepo backend), then open Research on the artifact.  
- Do **not** reconfigure a live engine mid-session for research unless carefully designed; prefer spawn.

### 1.3 What we steal from pro tools (and what we refuse)

| Source | Steal | Refuse |
|--------|-------|--------|
| **Bloomberg** | Density, multi-panel launchpad, keyboard primacy | Enterprise permissions, NX catalog |
| **TradingView** | Chart layouts, equity tear sheets, markers | Social feed, cloud identity as core |
| **Bookmap** | Optional liquidity viz later | Heatmap as MVP gate |
| **IBKR TWS** | OMS seriousness, presets, confirmations | Hostile defaults |
| **Binance/Bybit futures** | Mark/funding/liq, reduce-only, brackets | Casino chrome, deposit nags |
| **QuantConnect / Jupyter** | Reproducibility, seed, tear sheets | Cloud-only research |

### 1.4 Operator questions the UI must always answer

1. What **mode** am I in, and can this action move **real money**?  
2. What is the **market** doing, and is data **fresh**?  
3. What am I **in**, what’s **working**, what’s my free **risk**?  
4. What are **strategies** trying to do next?  
5. What **stops** me if wrong (SL/TP, risk, kill, DMS, halt)?  
6. If broken, what was the **last safe truth** and how do I **restart**?  
7. Can I **prove** a research run (seed, config, build, data path)?

---

## 2. Non-negotiable engineering constraints

### 2.1 Sacred seams (only allowed I/O)

```
UI thread (ImGui)
  ──poll──► snapshot_fn → ui::dashboard_snapshot     (live)
  ──cold──► Analytics::generate_report() / file      (research)
  ──input─► ui::operator_actions                     (ops)
  ──spawn─► engine_backtest / MC child               (develop)
```

| Seam | Path | Notes |
|------|------|-------|
| Live snapshot | `engine.snapshot_dashboard()` → `src/ui/dashboard_snapshot.h` | Sole live path |
| Builder | `src/engine/dashboard_snapshot_builder.*` | Cold path fill under lock |
| Ops | `src/ui/operator_actions.h` | pause / flatten / kill |
| Report | `src/analytics/analytics.h` `AnalyticsReport` | Research |
| MC | `src/simulation/monte_carlo_types.h` + reporter JSON | Campaigns |
| Registry | `src/strategy/strategy_registry.h` | Names + params only |
| Optional HTTP | `src/web/*` API-only | Remote observers; desk does not depend on it |

### 2.2 Layering

```
src/ui/desk/     ImGui presentation only (cold)
src/ui/          snapshot types, operator_actions, format helpers
src/web/         optional serializers (no ImGui includes)
src/engine/      NO imgui / glfw / opengl headers
```

### 2.3 Hot path / safety (from AGENTS.md)

| Rule | Desk implication |
|------|------------------|
| Zero heap on hot path | UI never runs on event loop; poll snapshot only |
| No JSON on hot path | In-process desk uses structs; JSON only cold load of files |
| Freeze surface (10 files) | No casual edits; controls via public APIs |
| `TT_TARGET` compile gate | LIVE orders only in `engine_live`; UI must not spoof |
| Halt write-once | Banner only; **never** “resume trading” |
| Kill / DMS fail-closed | Confirm once; no retry loops; toast on failure |
| Web remains read-only | Never add HTTP order routes for “convenience” |
| SPSC sole producer | Desk never pushes market/order rings |

### 2.4 Tech stack (locked)

| Piece | Choice |
|-------|--------|
| UI | Dear ImGui **docking** (`v1.91.8-docking` or newer docking tag) |
| Charts | ImPlot |
| Window | GLFW + OpenGL3 |
| Theme | `src/ui/desk/desk_theme.h` institutional dark quant palette |
| Format | `src/ui/desk/format_scale.h` (signed qty, win-rate %, DD, sides) |
| Prefs | `truetest_desk.ini` (gitignore) + later named layouts |
| CMake | `ENABLE_IMGUI`, `tt_wire_imgui_desk` on shadow/live (+ backtest for research/develop) |

---

## 3. Engine capability map (what exists today)

### 3.1 Live: `dashboard_snapshot` fields

**Header:** `src/ui/dashboard_snapshot.h`  
**Builder:** `src/engine/dashboard_snapshot_builder.cpp`  
**Wire (optional):** `src/web/snapshot_json.cpp` schema_version **2**

| Block | Key fields | Desk status (2026-08-02) | Honesty notes |
|-------|------------|--------------------------|---------------|
| Account | cash, equity, initial_balance, realized_pnl, unrealized_pnl | **Ready** | realized may be equity−initial; prefer true closed PnL later |
| Positions | symbol, qty (signed), avg_entry, mark, unrealized | **Ready** | multi-symbol marks may be incomplete for non-focus symbols |
| Lots | opener_order_id, strategy, side L/S, qty, entry, age | **Ready** | |
| Open orders | id, symbol, strategy, side B/S, type M/L/S/s, qty, px, status, age | **Ready** | |
| Fills | ts, symbol, side, qty, px, fee, source | **Ready** (cap ~64 recent) | |
| Brackets | SL/TP optional, mark, venue_managed, venue_list_id | **Ready** + rail UI | |
| Strategies | pnl, trades, win_rate %, PF, lots, armed_brackets | **Ready** | |
| Risk | halted, daily_loss, limits, exposure, open_orders | **Partial** | `daily_loss` may be hard-coded 0 in builder |
| Perf | orders/fills/trades, WR, sharpe, markout | **Partial** | sortino/PF may be 0 live |
| L2 | bids/asks top-N, mid, spread_bps, microprice, imbalance, source | **Ready** | |
| Queue | avg_bps, shadow drain counts | **Missing panel** | in struct; not in JSON; TUI Debug has it |
| Health | latency, flow totals, ages, ring drops, provider, QuestDB | **Partial** | many flow fields need ConsoleDashboard merge |
| Trend | equity_tail, drawdown_tail (N≈60), rate_tail | **Ready** equity/dd | rate_tail often empty |
| Debug | target, mode, preset, rings, pools, errors, stages | **Partial** | needs dedicated Debug panel |
| Memory | RSS/heap/pools/rings | **Partial** | |

**Critical gap:** ncurses Health reads `ConsoleDashboard` atomics; ImGui `DeskApp` stores `data_` but does not yet merge it. Fix before trusting Health.

### 3.2 Research: `AnalyticsReport`

**Header:** `src/analytics/analytics.h`

| Block | Fields | Desk Research |
|-------|--------|---------------|
| Returns | initial/final equity, returns, Sharpe/Sortino/Calmar, rolling | **To build** |
| Series | equity_curve, trade_returns, benchmark_equity_curve | **To build** |
| Trades | `trade_record[]` full blotter | **To build** |
| Breakdown | per_symbol, per_strategy | **To build** |
| Slippage / latency | avg signed/adverse, tick-to-trade | **To build** |
| Open inventory | open_positions, realized/unrealized | **To build** (+ ensure report_json emits) |

### 3.3 Monte Carlo

| Type | Fields | Desk |
|------|--------|------|
| `TrialResult` | trial_id, seed_used, equity, pnl, max_dd, sharpe, trades, WR, PF | Phase 5 |
| `McAggregate` | mean/median/p5/p95, trials[], wall_time | Phase 5 |
| CLI | `--monte-carlo --mc-trials --mc-model gbm --thread-preset inline` | spawn from Develop |

### 3.4 Operator controls

```cpp
// src/ui/operator_actions.h
pause_toggle; pause_state; flatten; kill(deadline) → bool
```

Wired in `main.inc` to:

- `engine::set_pause_all` / `is_pause_all`  
- `engine::request_flatten`  
- `provider->get_kill_switch()->cancel_all_and_flatten` (when present)

### 3.5 Strategies (registry names)

Built-ins include (non-exhaustive):  
`sma`, `ma-crossover`, `mean-reversion`, `breakout`, `coiled-spring`, `structure-continuation`, `adaptive-hybrid`, `larry_connor`, `hedge-demo`.

**Important:** Strategies are **compiled C++** via `REGISTER_STRATEGY`. Desk selects name + `--param`; it is **not** a full IDE. Code authoring stays in editor + rebuild.

### 3.6 Binaries

| Binary | Live orders | Desk role |
|--------|-------------|-----------|
| `engine_backtest` | Impossible | Research + Develop spawn target |
| `engine_shadow` | Impossible | Primary Monitor |
| `engine_live` | Allowed (gated) | Execute + Monitor |

---

## 4. Professional feature checklist × TrueTest mapping

Legend: **E** Essential · **A** Advanced · **X** Expert  
**Engine:** Ready / Partial / Missing / Out-of-scope  

### 4.1 Market data / tape / book / charts

| Feature | Tier | Engine support | Desk action |
|---------|------|----------------|-------------|
| L1 / mid / mark strip | E | Ready (L2 + marks) | Keep/improve ticker strip |
| Multi-symbol watchlist | E | Partial (positions + L2 focus symbol) | Extend snapshot multi-mark / last trade |
| L2 ladder + cum size | E | Ready | Keep; depth bars; depth N config |
| Book imbalance | A | Ready (`l2.imbalance`) | Shown; optional histogram |
| Liquidity heatmap | X | Missing MBO history | Out of MVP; optional later module |
| Trade tape (market) | E | Partial (fills tape exists; market trades thin) | Prefer fills; add trade events if exposed |
| Fills tape aggressor color | E | Ready | Ready |
| Funding / mark vs index | E (perps) | Partial (funding PnL not on snapshot) | Surface funding when available |
| OHLC multi-TF charts | E | Partial (bar provider exists; not snapshot series) | Phase B: bar history buffer cold path or QuestDB query |
| Equity/DD live chart | E | Ready (`trend.*`) | Ready ImPlot |
| Order/fill/SL markers on price chart | E | Partial | Research + later live overlay |
| Synthetic vs venue badge | E | Ready (`l2_source`) | Ready |
| Feed age / stale red | E | Partial (ages often 0 without ConsoleDashboard) | **P0 fix** |

### 4.2 OMS / order management

| Feature | Tier | Engine support | Desk action |
|---------|------|----------------|-------------|
| Working orders blotter | E | Ready | Ready |
| Cancel / modify from UI | A | **Not exposed** as safe public operator_actions | **Do not invent** without design; strategies + exits own most flow |
| Manual order ticket | A | Possible via engine APIs — **high risk** | Phase C only; default **strategy-driven** |
| Ladder click-to-trade | A | Same | Optional later; live arming required |
| Order state machine clarity | E | status string on open_order_row | Improve labels |
| Reject reason surface | E | Partial | Surface risk rejections in event/toast |
| Parent-child brackets | A | bracket_row + venue_list_id | Rail UI ready; tree optional |
| Reconciler status | E live | Provider reconciler (freeze) | Read-only status panel when hooks exist |

**Personal-desk policy:** TrueTest is **strategy-first**. Manual ticket is Advanced/Expert, not MVP. MVP ops = pause / flatten / kill + platform SL/TP via CLI/`DefaultExitPolicy`.

### 4.3 Positions / lots / PnL / brackets

| Feature | Tier | Engine | Desk |
|---------|------|--------|------|
| Positions table | E | Ready | Ready |
| Lots breakdown | A | Ready | Ready |
| Realized vs unrealized | E | Partial | Fix account strip semantics |
| Fees on fills | E | Ready | Ready |
| Funding line | E perps | Partial | Add when portfolio exposes |
| SL/TP rail | E | Ready | Ready |
| Flatten symbol / all | E | flatten all | Ready (all); per-symbol later |
| Liq distance / leverage | E futures | FuturesRiskCheck exists; **not on snapshot** | Extend snapshot read-only |
| Exit policy badge | E | CLI `--exit-policy` | Show from debug/config strip |

### 4.4 Risk / halt / safety

| Feature | Tier | Engine | Desk |
|---------|------|--------|------|
| Mode badge BACKTEST/SHADOW/LIVE | E | Ready | Ready |
| HALT banner terminal | E | Ready | Ready — no clear button |
| Daily loss / DD / exposure gauges | E | Partial | Fix daily_loss; show limits |
| Kill switch confirm | E | Ready when provider supplies | Ready |
| Pause strategies | E | Ready | Ready |
| DMS countdown | E live | Provider DMS | **Add** read-only countdown when API exists |
| Pre-trade fail reason | E | RiskManager / FuturesRiskCheck | Toast + log panel |
| Safety subsystem row (KS/DMS/reconciler/watchdog) | E | Scattered | New **Safety** panel (read-only) |
| Arm LIVE ritual | E | Process + docs | UI checklist before enabling ops on live |

### 4.5 Strategy monitoring

| Feature | Tier | Engine | Desk |
|---------|------|--------|------|
| Per-strategy PnL / WR / PF / lots | E | Ready | Ready |
| Pause all | E | Ready | Ready |
| Per-strategy pause | A | May need engine API | Later |
| Param inspector (read-only) | E | CLI params known at start | Develop + chrome |
| Signal/intent stream | E | Not in snapshot | Optional event ring / ConsoleDashboard events |
| Shadow divergence | E shadow | ShadowTracker | New panel Phase B |

### 4.6 Research / backtest / MC

| Feature | Tier | Engine | Desk |
|---------|------|--------|------|
| Config form (provider, path, strategy, seed, fees, SL/TP, realism) | E | Full CLI | Develop mode Phase 4 |
| One-click re-run same seed | E | CLI | Develop |
| Equity + underwater + metrics board | E | AnalyticsReport | Research Phase 3 |
| Full trade blotter sort/filter | E | trades[] | Research Phase 3 |
| Benchmark overlay | A | benchmark_equity_curve | Research |
| Per-symbol / per-strategy tables | A | maps on report | Research |
| MC distributions + trial table | E if MC | McAggregate | Phase 5 |
| MC trial → re-run seed | E | seed_used | Phase 5 |
| Compare two runs | E | files | Phase 5+ |
| Walk-forward / grid | A/X | **Core gaps** | Do **not** fake in UI |
| Export JSON/CSV | E | export paths | Buttons in Research |

### 4.7 Session ops / connectivity / latency

| Feature | Tier | Engine | Desk |
|---------|------|--------|------|
| Latency HUD tick→trade | E | Ready | Health |
| Ring drops / pool in-use | E | Partial | Debug panel |
| Provider connection state | E | Partial | Health |
| QuestDB persist health | A | Partial | Health |
| Structured log viewer | E | ConsoleDashboard events | Wire event ring into desk |
| Run tag / persist / output path | E | CLI | Develop + status chrome |
| Build / git / TT_TARGET about | E | truetest_version.h | Menu About |
| Thread preset / pin display | A | debug_view | Health/Debug |

### 4.8 Layout / UX chrome

| Feature | Tier | Status |
|---------|------|--------|
| Dockable panels + saved layout | E | Docking ImGui + `truetest_desk.ini` |
| Named layouts (Monitor / Market Focus / Diagnostics) | E | Ready; immediate reset, saved custom layouts preserved |
| Dark theme + high-contrast | E | theme + toggle |
| Hotkeys P/F/K/F1 + help | E | Ready; extend |
| Command palette | A | Later |
| Virtualized long tables | E | ImGui clipper on blotters |
| UI never on engine thread | E | Already |
| Colorblind-safe (shape+color for side) | E | Use L/S text not only green/red |

---

## 5. Target information architecture

### 5.1 Global chrome (always visible)

```
┌─ Menu: Desk | View | Ops | Research | Help ──────────────── FPS · ev/s ─┐
├─ [BACKTEST|SHADOW|LIVE] [mode] [HALTED?] [PAUSED?]  [Pause][Flatten][Kill] ┤
├─ Account: Equity | Cash | Realized | Unrealized | SessionΔ | Funding?     ┤
├─ Ticker / Watchlist strip                                                 ┤
├─ (optional) Safety strip: Risk | KS | DMS | Reconciler | Watchdog         ┤
└─ Dockspace ───────────────────────────────────────────────────────────────┘
```

### 5.2 Default Monitor layout (dock seed)

| Region | Panels |
|--------|--------|
| Left | Equity & Drawdown |
| Center | Positions (primary) |
| Right top | Order Book (L2) |
| Right bottom | Fills |
| Bottom left | Lots & Brackets · Open Orders (tabs) |
| Bottom right | Strategies · Risk · Health · Debug |

### 5.3 Research workspace

| Panel | Content |
|-------|---------|
| Metrics board | Sharpe, Sortino, Calmar, max DD, WR, PF, trades, time-in-market |
| Equity chart | strategy + optional benchmark + DD subplot |
| Trade blotter | full `trades[]` virtualized, filters, export |
| Breakdown | per_symbol / per_strategy tables |
| Slippage / latency | report blocks |
| Run meta | seed, flags, binary version, data path, git sha |

### 5.4 Develop workspace

| Panel | Content |
|-------|---------|
| Config form | provider, path/synthetic params, strategy multi-select, `--param`, seed, balance, fee, SL/TP, exit-policy, realism flags, thread preset |
| Job queue | local history of runs (dir of meta+report) |
| Progress | child process status, cancel |
| Actions | Run backtest · Run MC · Open last report · Copy CLI |

### 5.5 Execute / Live extras

| Panel | Content |
|-------|---------|
| Pre-flight checklist | testnet?, token?, size caps, DMS, attended |
| Panic card | Flatten → Kill → (documented order) |
| Typed confirm | type `KILL` / `FLATTEN` for live destructive ops |

---

## 6. Existing code to extend (do not rewrite blindly)

| Path | Role |
|------|------|
| `src/ui/desk/desk_app.{h,cpp}` | Main app, dock, panels, ops |
| `src/ui/desk/desk_theme.h` | Palette + panel chrome helpers |
| `src/ui/desk/format_scale.h` | Number/side formatting |
| `src/ui/operator_actions.h` | Shared with TUI |
| `src/ui/dashboard_snapshot.h` | Live DTO |
| `src/engine/dashboard_snapshot_builder.*` | Fill snapshot (extend carefully) |
| `src/bin/main.inc` | `--desk` wiring, snap_fn, ops, batch hold-open |
| `cmake/Dependencies.cmake` | `tt_wire_imgui_desk` |
| `docs/internal/imgui-desk-design.md` | Short design note |
| Ncurses reference | `src/ui/panels/*`, `tabbed_dashboard.cpp` for parity |
| SPA reference (visual only) | `src/web/frontend/src/live.tsx`, `backtest.tsx`, `styles.css` |

### 6.1 Suggested file split as the desk grows

Keep files under ~500–800 LOC; extract panels:

```
src/ui/desk/
  desk_app.{h,cpp}           # lifecycle, dock, chrome
  desk_theme.h
  format_scale.h
  panels/
    account_strip.cpp
    ticker_panel.cpp
    equity_panel.cpp
    positions_panel.cpp
    book_panel.cpp
    fills_panel.cpp
    brackets_panel.cpp
    orders_panel.cpp
    strategy_panel.cpp
    risk_panel.cpp
    health_panel.cpp
    debug_panel.cpp
    safety_panel.cpp
  research/
    report_view.cpp          # AnalyticsReport UI
    blotter.cpp
    metrics_board.cpp
  develop/
    run_config_form.cpp
    job_launcher.cpp         # spawn/wait/cancel child
    run_history.cpp
  mc/
    mc_view.cpp
```

Register new `.cpp` in `tt_wire_imgui_desk` (explicit Sources, no globs).

---

## 7. Data honesty fixes (do these early)

Before claiming “full dashboard,” fix these **engine/UI contract** issues:

| ID | Fix | Why |
|----|-----|-----|
| H1 | Merge ConsoleDashboard stats into snapshot **or** DeskApp Health (events, ring drops, rate, ages) | Health is wrong today |
| H2 | Populate `risk.daily_loss` from RiskManager cold accessor | Gauge lies |
| H3 | Live sortino/PF: fill or mark N/A | Avoid fake zeros as skill |
| H4 | True realized vs equity−initial | Account strip truth |
| H5 | Funding PnL on account when available | Futures honesty |
| H6 | Multi-symbol marks for positions | Portfolio truth |
| H7 | `report_to_json` open_positions / realized-unrealized if file path used | Research parity |
| H8 | Expose queue_view in Debug panel | Shadow realism |

Any builder change: cold path only; tests for snapshot (`tests/test_dashboard_snapshot.cpp` if present); no freeze file edits without CCB.

---

## 8. Control & safety UX specification

### 8.1 Hotkeys (baseline)

| Key | Action |
|-----|--------|
| `P` | Pause / resume all strategies |
| `F` | Flatten (confirm) |
| `K` | Kill switch (confirm) |
| `Esc` | Cancel modal / close help |
| `F1` | Help overlay |
| `1`–`4` | Switch workspace Monitor / Research / Develop / Execute (when implemented) |

### 8.2 Confirm policy

| Action | Backtest | Shadow | Live |
|--------|----------|--------|------|
| Pause | OK | OK | OK |
| Flatten | Confirm | Confirm | Confirm + optional type `FLATTEN` |
| Kill | N/A toast | Confirm if hook | Confirm + type `KILL` |
| Clear halt | **Forbidden always** | | |

### 8.3 Mode chrome rules

- LIVE badge: high-contrast, unmistakable (e.g. red dim background).  
- Testnet/demo vs mainnet must appear if known from provider config.  
- If kill hook is null → disable Kill button + tooltip “provider has no kill switch”.  
- After kill/halt: show **restart instructions**, not Resume Trading.

### 8.4 What never ships in this desk

- Multi-tenant auth, billing, social trading, leaderboards  
- HTTP order entry routes  
- Auto-resume after halt/kill/DMS  
- Soft “disable all risk”  
- Fake walk-forward/grid the core cannot run  
- Full C++ IDE inside ImGui as MVP  
- SaaS concerns inside `engine_*`  

---

## 9. Implementation phases (Codex execution order)

Each phase ends with: build `engine_shadow` with `ENABLE_IMGUI`, manual smoke, gate scripts.

### Phase A — Trustworthy Monitor (current → solid)

**Goal:** Daily shadow/synthetic desk that does not lie.

1. H1–H3 honesty fixes (Health + risk + N/A metrics).  
2. Dedicated **Debug** panel: rings, pools, queue, memory, stage timings.  
3. **Safety** strip: halt, pause, kill availability, provider state.  
4. Virtualize fills/orders tables (`ImGuiListClipper`).  
5. Named layout presets + “Reset layout”.  
6. About dialog: version, git sha, `TT_TARGET`, feature flags.  

**Done when:** Health matches ncurses TUI on same run; Debug useful under load.

### Phase B — Research cockpit

**Goal:** No browser for post-run analysis.

1. Load `AnalyticsReport` from live engine after run **and** from `--output` JSON file.  
2. Metrics board + equity/DD ImPlot + benchmark overlay.  
3. Full trade blotter (sort, filter symbol/strategy/side, export CSV).  
4. Per-symbol / per-strategy tables.  
5. Slippage & latency section.  
6. Run meta panel (seed, resolved flags, data path, binary id).  

**Done when:** Synthetic SMA run viewed entirely in desk equals CLI summary metrics.

### Phase C — Develop launcher

**Goal:** Personal experiment loop without Node backend.

1. Config form covering essential CLI flags (see §10).  
2. Spawn `engine_backtest` with headless flags:  
   `--no-pin --status-format off --no-tui --output <path> --seed …`  
3. Progress + cancel (SIGTERM child).  
4. On success → switch to Research with report.  
5. Local run history directory (`~/.truetest/runs/` or `./runs/`).  
6. “Copy CLI” button for reproducibility.  

**Done when:** Full loop config → run → blotter without leaving desk.

### Phase D — Monte Carlo

1. MC config (trials, model, params, inline threads).  
2. Spawn campaign; parse `mc_aggregate_v1`.  
3. Histograms (PnL, Sharpe, DD); trial table; **re-run this seed** as single backtest.  
4. Document L2/realism limits in UI help (no false precision).  

### Phase E — Live Execute polish

1. Pre-flight checklist UI.  
2. Typed confirms for flatten/kill on live.  
3. DMS/reconciler/watchdog read-only status when available.  
4. Futures risk board (notional, leverage, liq distance) via snapshot extension.  
5. Panic card ordered actions.  

### Phase F — Depth & charts (Advanced)

1. Multi-symbol watchlist with last/mark.  
2. Optional OHLC chart from cold bar buffer / recording.  
3. Shadow divergence panel.  
4. Command palette.  
5. Multi-window / multi-monitor detach (GLFW).  

### Phase G — Retirement of SPA presentation

1. Desk is default human path.  
2. Keep optional API-only `--web` for monorepo proxy.  
3. Archive or stop shipping React SPA as primary.  
4. Update `docs/reference/05-web-ui.md` → desk + API split.  

---

## 10. Develop mode — CLI surface to expose in the form

Map form fields → flags (from `docs/reference/04-flags.md` / `main.inc`). Minimum set:

| Form field | Flag / meaning |
|------------|----------------|
| Provider | `--provider` local \| synthetic \| binance… |
| Data path | `--path` / provider path |
| Synthetic params | `--mc-params` or synthetic knobs |
| Strategy | `--strategy` (multi) |
| Params | `--param k=v` list |
| Seed | `--seed` |
| Balance | initial balance flag as used by engine |
| Fee model | `--fee` / fee value |
| Exit policy / SL / TP | `--exit-policy`, `--sl`, `--tp` |
| Realism | latency/impact/queue/fill flags (show “active in backtest only”) |
| Thread preset | `--thread-preset` (MC: force `inline` if parallel) |
| Output | `--output` path |
| MC toggle | `--monte-carlo --mc-trials N --mc-model gbm` |
| Persist | `--persist --run-tag` (optional) |

Always append for children: `--no-pin --status-format off --no-tui`.

---

## 11. Panel implementation checklist (copy into PRs)

### Monitor (Essential)

- [x] Mode chrome + halt/pause badges  
- [x] Account strip  
- [x] Ticker strip  
- [x] Equity & DD (ImPlot)  
- [x] Positions  
- [x] Lots & brackets rail  
- [x] Open orders  
- [x] L2 book  
- [x] Fills (+ filter)  
- [x] Strategies  
- [x] Risk gauges  
- [x] Health (honest availability labels + desk-local telemetry merge)  
- [x] Ops pause/flatten/kill + confirm  
- [x] Debug panel complete  
- [x] Safety subsystem strip  
- [x] ConsoleDashboard merge  
- [x] Named Monitor / Market Focus / Diagnostics layouts + immediate reset  
- [x] About dialog with configure-time build identity  
- [ ] Event/log viewer  

### Research (Essential)

- [ ] Metrics board from AnalyticsReport  
- [ ] Full equity + benchmark  
- [ ] Trade blotter virtualized  
- [ ] Breakdown tables  
- [ ] Load from file + from live engine  
- [ ] Export  

### Develop (Essential for “all-in-one”)

- [ ] Config form  
- [ ] Spawn backtest  
- [ ] History  
- [ ] Jump to Research  

### MC (Essential if claiming analysis platform)

- [ ] Campaign UI  
- [ ] Distributions  
- [ ] Trial drill-down  

### Live (Essential if claiming execute)

- [ ] Pre-flight  
- [ ] Typed confirms  
- [ ] Futures risk telemetry  
- [ ] DMS/reconciler display  

---

## 12. Coding standards for desk work

1. **C++23**, match project style; no new dependencies without human approval (ImGui/ImPlot/GLFW already approved for this effort).  
2. **No** `nlohmann/json` on hot path; file load only in Research cold path.  
3. Prefer **ImGui tables** + clipper for long lists.  
4. **ImPlot** for series; dark plot style matching `desk_theme`.  
5. Format via `format_scale.h` only — do not re-invent win-rate scales.  
6. Side labels: text **LONG/SHORT/BUY/SELL** + color (a11y).  
7. All destructive actions go through confirm helpers, not inline.  
8. Tests: prefer pure format/helpers unit tests; UI smoke manual; optional headless GLFW skip.  
9. Do not expand `engine.cpp` for UI convenience — use builder / public APIs.  
10. German conventional commits when committing via project skill (`xx:yyy`); keep desk commits thematic.

---

## 13. Acceptance criteria — “Full cockpit”

### Must (personal daily usable)

1. One binary session with `--desk` shows trustworthy Monitor for synthetic and shadow.  
2. Operator can pause, flatten, kill (when available) with safe confirms.  
3. Halt is terminal and clearly communicated.  
4. Research: open a completed backtest and see equity, metrics, full trade list.  
5. Develop: launch a backtest from the UI and land in Research.  
6. Reproducibility fields visible (seed, strategy, params, build).  
7. No browser/Node required for the personal loop.  
8. Gates green; freeze surface untouched (or CCB if unavoidable).  
9. Headless engine still works without display (`ENABLE_IMGUI=OFF` or no `--desk`).  

### Should (pro personal)

10. MC campaign + trial seed re-run.  
11. Debug + Safety panels complete.  
12. Named layouts; multi-monitor optional.  
13. Shadow divergence view.  

### Could (specialist)

14. Manual order ticket with live arming.  
15. OHLC multi-TF from history.  
16. Bookmap-style heatmap module.  

---

## 14. Testing & verification ritual

```bash
# Gates (always)
./scripts/check-hotpath-json.sh
./scripts/check-layer-deps.sh
./scripts/check-live-safety-freeze.sh

# Default CI tree (ImGui off)
cmake --preset linux-tests && cmake --build --preset linux-tests -j
ctest --test-dir out/build/linux-tests --output-on-failure

# Desk build
cmake -B build-imgui -DENABLE_IMGUI=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-imgui -j --target engine_shadow

# Number parity: same seed CLI vs desk Research metrics
./build-imgui/engine_shadow --provider synthetic --strategy sma --seed 424242 \
  --no-pin --status-format off --no-tui --output /tmp/tt_report.json
# Then open /tmp/tt_report.json in Research mode and compare key metrics
```

Manual checklist per PR:

- [ ] Window opens; dock layout sane  
- [ ] Snapshot updates (or final frame on batch)  
- [ ] HALTED banner if forced  
- [ ] Flatten/kill confirm cancel works  
- [ ] No crash on empty book / flat book  
- [ ] Closing window stops desk cleanly  

---

## 15. Codex prompt template (paste to start a slice)

```text
You are implementing the TrueTest ImGui cockpit per /imgui.md and
docs/internal/imgui-desk-design.md on branch imgui.

Constraints:
- Sacred seams only: dashboard_snapshot, operator_actions, AnalyticsReport, spawn child.
- No freeze-surface edits without CCB. No hot-path ImGui.
- ENABLE_IMGUI / HAS_IMGUI_DESK. Explicit CMake sources.
- Honest metrics; no fake charts.

Current slice: <PHASE A|B|C|… item list>
Read existing src/ui/desk/* and ncurses panels for parity.
Implement, build engine_shadow with ENABLE_IMGUI=ON, run gate scripts,
summarize what was done and what remains from imgui.md §11 checklist.
```

---

## 16. Reference links (in-repo)

| Topic | Path |
|-------|------|
| Agent rules | `AGENTS.md` |
| Design note | `docs/internal/imgui-desk-design.md` |
| Flags | `docs/reference/04-flags.md` |
| Web (legacy human UI) | `docs/reference/05-web-ui.md` |
| Strategy SDK | `docs/reference/07-strategy-development.md` |
| Prod / safety | `docs/governance/01-prod.md` |
| Performance | `docs/architecture/04-performance.md` |
| Snapshot type | `src/ui/dashboard_snapshot.h` |
| Report type | `src/analytics/analytics.h` |
| Desk code | `src/ui/desk/` |

---

## 17. Summary for implementers

TrueTest already has the **hard parts of a pro desk in the engine**: coherent snapshots, lots/brackets, L2, risk halt, analytics tear sheets, MC, compile-time live gating, and operator hooks.

The ImGui cockpit’s job is to **surface all of that honestly**, with Bloomberg/TWS density and TradingView-quality research charts, while remaining:

- personal and attended  
- fail-closed  
- off the hot path  
- free of SaaS and social clutter  

**Basics first (Monitor + Ops + honest Health), then Research, then Develop, then MC, then Live polish.**  
Complex tasks (manual OMS ticket, heatmaps, multi-TF OHLC) are real but **after** the daily loop is trustworthy.

When in doubt: answer the seven operator questions (§1.4). If a feature does not help answer them, do not build it yet.

---

*End of Codex brief. Update this file when phases complete or engine seams change.*
