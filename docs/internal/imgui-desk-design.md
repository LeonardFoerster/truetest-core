# TrueTest Trading Command Center

## Purpose

The ImGui desk is an attended, real-time operations console for a running
crypto trading or market-making engine. It answers, without page navigation:

- What target, provider, and update state am I operating?
- What is the account worth, what is its total/settled/unrealized PnL, and how
  much marked exposure exists?
- Which instruments, positions, working orders, brackets, and fills need
  attention now?
- Is the snapshot stale, halted, paused, or otherwise unavailable?

It is not a research workspace, footprint/TPO terminal, Monte Carlo frontend,
or generic developer diagnostics application. Those older desk concepts are
intentionally absent from the command-center presentation.

## Hierarchy

One fixed command center fills the window:

```text
status / attended controls
account metric strip
market watch | selected instrument + reserved chart seam | account / risk
positions | open orders | protection | recent fills
provider and snapshot-health strip
```

At 1920×1080 and 2560×1440 the three operational regions remain visible at
once. At narrower widths, the central region uses normal horizontal and table
scrolling rather than silently discarding the account/risk rail. Tables are
dense and numeric; the only rounded elements are small state badges and
confirmation surfaces.

The central **chart surface reserved** child is an intentional component seam:
a future chart receives selected-symbol history through a new cold-path view
model input and replaces that child. The current desk never fabricates candles
or history merely to occupy the space.

## Data boundary

```text
engine state
  -> fixed-capacity projection capture (engine producer, allocation-free)
    -> DashboardSnapshotBuilder (reader-side rich materialization)
    -> CommandCenterViewModel (pure projection + formatting)
      -> ImGui panels
```

Panels do not access an engine, provider, venue API, or orderbook directly.
The producer projection captures a bounded union of known orderbook symbols,
marks, positions, and cached working orders; the reader-side builder
materializes `market_rows` from that immutable projection.
Best bid/ask and L2 measures use `orderbook::get_external_order_infos()` so
the desk never presents the engine's own resting orders as venue market data.

The old single-symbol L2 snapshot remains for the ncurses dashboard. The
ncurses TUI deliberately does not render the command center's multi-symbol
Market Watch; its existing single-symbol ladder is retained rather than
creating an untested second ncurses layout.

## Accounting and availability policy

`equity` and marked aggregate values are available only when every non-zero
portfolio position has a symbol-specific mark. A last mark for another symbol
is never reused. When marking is incomplete, equity, total PnL, aggregate
unrealized PnL, gross exposure, and effective leverage render as unavailable.

The account metrics are:

- **Total PnL** = `equity - initial_balance` when equity is fully marked.
- **Unrealized PnL** = sum of `mark * quantity - cost_basis` for fully marked
  positions.
- **Realized PnL** (the portfolio's settled/net component) =
  `cash + sum(open cost_basis) - initial_balance`. It is the settled remainder
  of the portfolio accounting identity, so it includes fees and funding that
  the portfolio has already settled; it is not a misleading `equity - initial`
  duplicate. Thus `total = settled + unrealized` whenever all positions are
  marked.

`position_row.avg_entry` is fee-adjusted break-even derived from cost basis;
it is not raw fill VWAP and is labelled **Break-even** in the desk.

Numeric zero is rendered only when it is an actual observable zero. Explicit
availability flags cover equity/PnL/exposure/drawdown/queue data. `0` daily
loss, missing limits, a provider being open, and the age of a cached snapshot
are never falsely painted as a healthy venue-data state. In particular,
`generated_at` is labelled **snapshot/update age**, not market-data age;
provider last-event/fill ages are unavailable until the engine supplies them.

Queue position and markout are currently aggregate across observable adapters
or samples. The selected-instrument panel labels them **(ALL)** and only shows
them when data is available. It must not imply they are per-symbol measures.

## Controls and safety

The only working controls use `ui::operator_actions`, unchanged from the
frozen startup integration:

- Pause/Resume invokes the supplied hook immediately and shows its current
  state when the supplied state hook is available.
- Flatten always opens a confirmation modal before invoking its supplied hook.
- Kill always opens a strong confirmation modal and passes the existing five
  second deadline to its supplied hook.

The desk has no Clear Halt control. A halt renders as **HALTED — RESTART
REQUIRED** and remains terminal. Missing callbacks render disabled buttons and
an explicit unavailable tooltip; the desk never invents local success.

`DeskTradeActions` is a separate, typed future seam for close/partial-close,
SL/TP changes, cancel, and amend intents. It is deliberately empty in normal
startup and currently visible controls say **Not wired yet**. It cannot bypass
`operator_actions`, call an engine/provider from a panel, or mutate UI state
as though an order was accepted.

The removed research/demo/footprint setters are not part of the command-center
API or frozen startup wiring. `DeskApp` consumes only the dashboard snapshot and
the existing attended operator-action seam.

## Lifecycle and thread ownership

`DeskApp::start()` and `DeskApp::run()` execute on the same
application-main/platform-owner thread. GLFW, ImGui, and OpenGL initialization,
event polling, rendering, and teardown never migrate to the engine thread.
Engine work runs in a `std::jthread`; `request_stop()` is the only cross-thread
desk lifecycle operation.

Closing a streaming desk ends the render loop but does not request an engine or
provider stop. The application-main thread joins the engine thread and waits for
the stream's existing shutdown path. A batch desk retains the terminal snapshot
until the operator closes the window.

## Health semantics

The bottom strip exposes provider lifecycle, snapshot/update age, UI-derived
event rate (when ConsoleDashboard atomics exist), tick-to-trade latency when
samples exist, and aggregate ring drops. A connected provider is a connection
state, not proof that market data is fresh. Stale snapshot updates, provider
errors, halted state, and unavailable metrics use explicit adverse/unknown
visual treatment. LIVE uses red danger treatment; green is reserved for
positive PnL, buy/long direction, and similar directional meaning.

## Legacy removal

The former MARKET/RESEARCH/OPERATIONS/DIAGNOSTICS workspace hierarchy,
seven-page layouts, per-page dockspaces, command palette, demo research,
footprint camera/panel state, liquidity/footprint/TPO/correlation panels, and
ImPlot dependency were removed from `IMGUI_DESK_SOURCES` and their obsolete
tests were removed. `FootprintLiveSource` and its presentation bridge remain as
research/support code outside the current `IMGUI_DESK_SOURCES` and startup seam;
they are not polled or rendered by the command center.

## Verification focus

Pure command-center tests cover projection, sorting, marking availability,
position notional/uPnL percentages, order distance, bracket distance,
formatting, and the default-disabled future action seam. Snapshot tests cover
multi-symbol market rows and the marked PnL accounting identity. Rendering is
kept thin so it can be compiled separately from those tests.
