    # UI Implementation Plan — TrueTest Trading Dashboard

This document contains step-by-step instructions for building a TradingView/TradeZella-inspired
frontend for the TrueTest backtesting engine. Follow each phase in order. Do not skip steps.

---

## Phase 0: Project Scaffolding

1. Create a new React + TypeScript project inside `web/` using Vite:
   ```bash
   cd web/
   npm create vite@latest . -- --template react-ts
   npm install
   ```
2. Install dependencies:
   ```bash
   npm install lightweight-charts tailwindcss @tailwindcss/vite
   ```
3. Configure Tailwind CSS via the Vite plugin (`@tailwindcss/vite`). Add `@import "tailwindcss"` to the main CSS file. Use a dark theme as the default and only theme.
4. Remove all Vite/React boilerplate (default App component, logos, sample CSS).
5. Keep the old `index.html` as `index.legacy.html` for reference, then replace it with the Vite entry point.
6. Confirm the dev server starts with `npm run dev`.

---

## Phase 1: WebSocket Connection Layer

Create `src/services/websocket.ts`:

1. Implement a `WebSocketService` class that:
   - Connects to `ws://<hostname>:8765` (same logic as the old UI).
   - Auto-reconnects with exponential backoff (initial 1s, max 10s).
   - Exposes an `onMessage(callback)` method for subscribing to parsed JSON messages.
   - Exposes a `send(command)` method for sending JSON commands to the engine (for Phase 5).
   - Tracks connection state (`connected`, `disconnected`, `reconnecting`) and exposes it reactively.
2. Create a React context (`src/contexts/WebSocketContext.tsx`) that:
   - Instantiates `WebSocketService` once on mount.
   - Provides connection state and the send/subscribe functions to all child components via `useWebSocket()` hook.

---

## Phase 2: State Management

Create `src/store/` with the following state slices (use React context + useReducer, no external state library):

### `MarketStore`
- Holds an array of OHLCV bar objects: `{ time, open, high, low, close, volume }`.
- Appends new bars from `type: "market"` WebSocket messages.
- Also stores the current symbol and timeframe string.

### `PortfolioStore`
- `cash: number`
- `positions: Array<{ symbol, qty, cost_basis, unrealized_pnl }>`
- `total_trades: number`
- Updated from `type: "portfolio"` messages.

### `FillStore`
- Holds the last 200 fills: `{ id, time, symbol, side, quantity, price, commission }`.
- Newest first. Appends from `type: "fill"` messages.

### `AnalyticsStore`
- `equity: number`, `cumulative_return: number`, `sharpe_ratio: number`
- `sortino_ratio: number`, `max_drawdown: number`, `win_rate: number`
- `profit_factor: number`, `avg_win: number`, `avg_loss: number`
- `equity_history: number[]` (for equity curve chart)
- `drawdown_history: number[]` (for drawdown chart)
- Updated from `type: "analytics"` messages.

### `OrderBookStore`
- `bids: Array<{ price, quantity }>` (sorted descending by price)
- `asks: Array<{ price, quantity }>` (sorted ascending by price)
- `spread: number`
- Updated from `type: "orderbook"` messages (requires engine-side addition, stub with empty arrays until available).

### `EngineStore`
- `status: "idle" | "running" | "paused" | "halted"`
- `strategy: string`
- `symbol: string`
- `timeframe: string`
- Updated from `type: "status"` messages.

Create a single `AppProvider` component that wraps all stores and the WebSocket context.

Create a message dispatcher (`src/store/dispatcher.ts`) that:
- Subscribes to WebSocket messages.
- Routes each message by `type` field to the appropriate store's dispatch.

---

## Phase 3: Layout Shell

Build the overall page structure in `src/App.tsx`. Use Tailwind CSS for all styling.
The entire UI must use a dark color scheme (dark backgrounds, light text, accent colors for buy/sell).

### Color palette:
- Background: `#0f1117` (main), `#161a25` (panels), `#1e222d` (elevated surfaces)
- Text: `#d1d4dc` (primary), `#787b86` (secondary/muted)
- Accent: `#2962ff` (primary blue), `#26a69a` (buy/green), `#ef5350` (sell/red)
- Borders: `#2a2e39`

### Layout structure (CSS Grid):
```
┌─────────────────────────────────────────────────────────────────┐
│ TopBar (h-12, full width)                                       │
├──────────────────────────────────────┬──────────────────────────┤
│ MainChart (flexible, takes           │ Sidebar (w-80)           │
│ remaining space)                     │ ┌──────────────────────┐ │
│                                      │ │ OrderBook (top half) │ │
│                                      │ ├──────────────────────┤ │
│                                      │ │ TradeEntry (bot half)│ │
│                                      │ └──────────────────────┘ │
├──────────────────────────────────────┴──────────────────────────┤
│ BottomPanel (h-64, resizable, tabbed)                           │
└─────────────────────────────────────────────────────────────────┘
```

Create skeleton components for each section (empty divs with correct sizing and background colors):
- `src/components/TopBar.tsx`
- `src/components/Chart/ChartPanel.tsx`
- `src/components/Sidebar/OrderBook.tsx`
- `src/components/Sidebar/TradeEntry.tsx`
- `src/components/BottomPanel/BottomPanel.tsx`

The sidebar should be collapsible via a toggle button. The bottom panel should be vertically
resizable via drag handle (implement a simple drag-to-resize, no library needed).

---

## Phase 4: Core Components

### 4.1 TopBar (`src/components/TopBar.tsx`)

Display in a single horizontal bar:
- **Connection indicator**: green/red dot + "Connected"/"Disconnected" text (from WebSocket context).
- **Symbol display**: show current symbol from EngineStore (read-only for now).
- **Strategy display**: show current strategy name from EngineStore (read-only for now).
- **Engine controls**: Play/Pause/Stop buttons. These send commands via WebSocket:
  - `{ "command": "start" }`
  - `{ "command": "pause" }`
  - `{ "command": "stop" }`
  - Grey out buttons based on current engine status.
- **Event counter**: total events received (from dispatcher).

### 4.2 Chart (`src/components/Chart/ChartPanel.tsx`)

Use the `lightweight-charts` library (by TradingView):

1. Initialize a chart instance with dark theme colors matching the palette above.
2. Add a **candlestick series** bound to `MarketStore` bars.
   - If the engine only sends `close` prices (current behavior), fall back to a **line series** instead.
   - When full OHLCV is available, switch to candlestick automatically.
3. Add a **volume histogram** series at the bottom of the chart (if volume data exists).
4. Add **SMA indicator overlays** as line series (when indicator values are included in market events).
5. Add **fill markers** using the `markers` API:
   - Buy fills: green upward triangle below the bar.
   - Sell fills: red downward triangle above the bar.
   - Tooltip shows quantity and price.
6. Chart must auto-scroll to the latest bar as new data arrives, but stop auto-scrolling if the user manually pans/zooms.
7. Show a crosshair with OHLCV legend in the top-left corner of the chart area.

### 4.3 Order Book (`src/components/Sidebar/OrderBook.tsx`)

Display a vertical order book ladder:
- Top half: asks (red), sorted price ascending (lowest ask at bottom, closest to spread).
- Bottom half: bids (green), sorted price descending (highest bid at top, closest to spread).
- Middle row: current spread value and percentage.
- Each row shows: price, quantity, cumulative quantity bar (horizontal bar fill behind the row, like TradingView).
- If no orderbook data is available from the engine yet, show a placeholder message: "Orderbook data not available — enable orderbook snapshots in the engine."

### 4.4 Trade Entry (`src/components/Sidebar/TradeEntry.tsx`)

A simple order entry form:
- Inputs: quantity (number), price (number, optional for market orders).
- Order type selector: Market / Limit.
- Two buttons: **Buy** (green) and **Sell** (red).
- On submit, send via WebSocket:
  ```json
  { "command": "order", "side": "buy|sell", "quantity": 100, "price": 50.25, "type": "limit" }
  ```
- Disable the form if the engine is not running or WebSocket is disconnected.
- Note: the engine does not support inbound commands yet. Build the UI anyway — it will be wired up when the engine adds bidirectional support.

### 4.5 Bottom Panel (`src/components/BottomPanel/BottomPanel.tsx`)

A tabbed panel with these tabs:

#### Tab: Positions
- Table columns: Symbol, Side, Quantity, Avg Cost, Current Price, Unrealized PnL, PnL %.
- PnL values colored green (positive) or red (negative).
- Data from `PortfolioStore.positions`.

#### Tab: Fills
- Table columns: Time, Symbol, Side, Quantity, Price, Commission, Total.
- Side column colored green (buy) or red (sell).
- Sortable by clicking column headers (client-side sort).
- Data from `FillStore`.

#### Tab: Orders
- Table columns: ID, Time, Symbol, Side, Type, Quantity, Price, Status.
- Placeholder for now — will be populated when the engine sends order state updates.

#### Tab: Analytics (TradeZella-inspired)
This tab should be the most visually rich. Layout as a grid of cards:

Row 1 — stat cards (4 columns):
- **Total Return**: percentage, colored green/red. Large font.
- **Sharpe Ratio**: number with 3 decimal places.
- **Max Drawdown**: percentage, always red.
- **Win Rate**: percentage with a small donut/ring chart inside the card.

Row 2 — charts (2 columns):
- **Equity Curve**: area chart (use lightweight-charts area series or a simple canvas). Green fill if overall positive, red if negative.
- **Drawdown Chart**: inverted area chart (values are negative percentages). Red fill.

Row 3 — additional stats (4 columns):
- **Profit Factor**
- **Avg Win / Avg Loss**
- **Sortino Ratio**
- **Total Trades**

All data from `AnalyticsStore`.

#### Tab: Journal
- A simple list of trades (each fill pair: entry + exit = one trade).
- Each trade row is expandable to show:
  - Entry/exit price, time, PnL.
  - A text area for user notes (stored in browser localStorage only).
  - Tag input for categorizing setups (e.g., "mean reversion", "breakout").
- This is a client-side-only feature, no engine support needed.

---

## Phase 5: Engine-Side Changes (C++)

These changes are needed to support the full UI. Implement in this order:

### 5.1 Bidirectional WebSocket

In `BacktestEngine/src/threading/ws_worker.h` (and related files):

1. Add a read handler to the WebSocket server that accepts incoming JSON messages from clients.
2. Parse incoming messages and dispatch commands:
   - `{ "command": "start" }` — begin/resume the backtest.
   - `{ "command": "pause" }` — pause event processing.
   - `{ "command": "stop" }` — stop and reset.
   - `{ "command": "order", ... }` — submit an order to the orderbook.
3. Commands should be pushed into a thread-safe command queue that the engine's main loop checks each iteration.
4. The engine should send a `type: "status"` message whenever its state changes:
   ```json
   { "type": "status", "data": { "state": "running", "strategy": "sma", "symbol": "BTCUSDT" } }
   ```

### 5.2 Richer Market Events

Modify the market event JSON (in `BacktestEngine/src/core/event_json.h`) to include full OHLCV:

```json
{
  "type": "market",
  "data": {
    "symbol": "BTCUSDT",
    "time": 1700000000,
    "open": 100.0,
    "high": 105.0,
    "low": 99.0,
    "close": 103.0,
    "volume": 1500.0
  }
}
```

### 5.3 Orderbook Snapshots

Add periodic orderbook depth snapshots (top 20 levels) broadcast via WebSocket:

```json
{
  "type": "orderbook",
  "data": {
    "bids": [{ "price": 100.0, "quantity": 50 }, ...],
    "asks": [{ "price": 100.5, "quantity": 30 }, ...],
    "spread": 0.5
  }
}
```

Emit these every N milliseconds (configurable, default 250ms) or on every orderbook change, whichever is less frequent.

### 5.4 State Snapshot on Connect

When a new WebSocket client connects, immediately send the full current state:
- Latest market bar.
- Current portfolio state.
- Current analytics snapshot.
- Current orderbook depth.
- Engine status.
- Last N fills (configurable, default 50).

This ensures the UI is never blank on connect.

### 5.5 Indicator Values in Market Events

When indicators (SMA, etc.) are computed by the strategy, include their values in the market event:

```json
{
  "type": "market",
  "data": {
    "symbol": "BTCUSDT",
    "time": 1700000000,
    "open": 100.0, "high": 105.0, "low": 99.0, "close": 103.0, "volume": 1500.0,
    "indicators": {
      "sma_20": 101.5,
      "sma_50": 99.8
    }
  }
}
```

---

## Phase 6: Polish and UX

1. **Responsive layout**: The sidebar should collapse on screens narrower than 1200px. The bottom panel should take more vertical space on small screens.
2. **Keyboard shortcuts**:
   - `Space` — toggle play/pause.
   - `Escape` — stop.
   - `B` — focus buy quantity input.
   - `S` — focus sell quantity input.
3. **Loading states**: Show skeleton loaders in panels before first data arrives.
4. **Toast notifications**: Brief popups for connection events, fill executions, risk halts.
5. **Number formatting**: All prices to 2 decimals, percentages to 2 decimals with `%` suffix, large numbers with comma separators.
6. **Animations**: Subtle transitions on PnL value changes (flash green/red briefly on update). Smooth chart scrolling.

---

## File Structure (Final)

```
web/
├── index.legacy.html              # old single-file UI (keep for reference)
├── index.html                     # Vite entry point
├── package.json
├── tsconfig.json
├── vite.config.ts
├── tailwind.config.ts
└── src/
    ├── main.tsx                   # React entry, wraps App in AppProvider
    ├── App.tsx                    # grid layout shell
    ├── index.css                  # tailwind imports + global dark styles
    ├── services/
    │   └── websocket.ts           # WebSocketService class
    ├── contexts/
    │   └── WebSocketContext.tsx    # WS provider + useWebSocket hook
    ├── store/
    │   ├── AppProvider.tsx         # combines all store providers
    │   ├── dispatcher.ts          # routes WS messages to stores
    │   ├── MarketStore.tsx
    │   ├── PortfolioStore.tsx
    │   ├── FillStore.tsx
    │   ├── AnalyticsStore.tsx
    │   ├── OrderBookStore.tsx
    │   └── EngineStore.tsx
    ├── components/
    │   ├── TopBar.tsx
    │   ├── Chart/
    │   │   └── ChartPanel.tsx
    │   ├── Sidebar/
    │   │   ├── OrderBook.tsx
    │   │   └── TradeEntry.tsx
    │   └── BottomPanel/
    │       ├── BottomPanel.tsx
    │       ├── PositionsTab.tsx
    │       ├── FillsTab.tsx
    │       ├── OrdersTab.tsx
    │       ├── AnalyticsTab.tsx
    │       └── JournalTab.tsx
    └── utils/
        └── format.ts              # number/date formatting helpers
```

---

## Execution Order

1. Phase 0 — scaffolding and dev server running.
2. Phase 1 — WebSocket connection working (verify with existing engine).
3. Phase 2 — state management wired to incoming messages.
4. Phase 3 — layout shell visible with colored placeholder panels.
5. Phase 4.2 — chart rendering real data (this is the highest-impact visual).
6. Phase 4.1 — top bar with connection status and controls.
7. Phase 4.5 — bottom panel tabs (Positions, Fills, Analytics).
8. Phase 4.3 — order book sidebar.
9. Phase 4.4 — trade entry form.
10. Phase 5 — engine-side enhancements (bidirectional WS, richer events).
11. Phase 6 — polish pass.

Test after each phase. Run `npm run dev` and connect to a running engine instance to verify
data flows correctly. If the engine is not running, the UI should gracefully show
"Disconnected — reconnecting..." and render empty/skeleton states.
