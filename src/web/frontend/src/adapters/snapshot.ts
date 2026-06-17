/* =========================================================================
   TrueTest — snapshot adapter.

   Maps the engine's SnapshotFrame (./wire.ts, faithful to dashboard_snapshot)
   onto the live cockpit's component prop shapes (./types.ts, the shapes the
   Claude Design components consume unchanged). ALL reconciliation lives here —
   field renames, the signed-qty → side+abs split, drawdown %→fraction, char
   sides → "long"/"buy", win-rate scale normalization, and synthesizing the
   per-strategy sparkline the engine doesn't emit. The components never change.
   ========================================================================= */
import type { SnapshotFrame, WireFill, WireStrategy } from "../wire";
import type { Account, Position, Lot, Book, Fill, Strategy, RiskLimit, Health, EquityPoint } from "../types";

export interface LiveData {
  halted: boolean;
  account: Account;
  positions: Position[];
  lots: Lot[]; // the SL/TP bracket viz — sourced from engine brackets
  book: Book;
  fills: Fill[];
  strategies: Strategy[];
  risk: RiskLimit[];
  health: Health;
  equityCurve: EquityPoint[];
}

// The engine emits win-rate as a percent (sub_analytics::win_rate() × 100);
// the components want a 0..1 fraction.
const pctToFrac = (v: number) => v / 100;

const longShort = (sideChar: string): "long" | "short" =>
  sideChar === "L" || sideChar === "l" ? "long" : "short";

const buySell = (sideChar: string): "buy" | "sell" =>
  sideChar === "B" || sideChar === "b" || sideChar === "BUY" ? "buy" : "sell";

const secondsOfDay = (tsMs: number) => Math.floor((tsMs / 1000) % 86400);

// Deterministic pseudo-sparkline from a seed (engine emits no per-strategy
// series). Bias by P&L sign so winners trend up, losers down.
function synthSpark(seed: string, bias: number, n = 40): number[] {
  let h = 2166136261;
  for (let i = 0; i < seed.length; i++) {
    h ^= seed.charCodeAt(i);
    h = Math.imul(h, 16777619);
  }
  const rnd = () => {
    h ^= h << 13;
    h ^= h >>> 17;
    h ^= h << 5;
    return ((h >>> 0) % 1e6) / 1e6 - 0.5;
  };
  const a: number[] = [];
  let v = 0;
  for (let i = 0; i < n; i++) {
    v += rnd() * 1.2 + bias * 0.18;
    a.push(v);
  }
  return a;
}

function adaptStrategy(s: WireStrategy): Strategy {
  return {
    name: s.name,
    pnl: s.pnl,
    win: pctToFrac(s.win_rate),
    pf: s.profit_factor,
    trades: s.trade_count,
    lots: s.open_lots,
    spark: synthSpark(s.name, s.pnl >= 0 ? 1 : -1),
  };
}

function adaptFill(f: WireFill, idx: number): Fill {
  return {
    id: f.ts_ms * 1000 + idx,
    t: secondsOfDay(f.ts_ms),
    sym: f.symbol,
    side: buySell(f.side),
    qty: f.qty,
    px: f.price,
    fee: f.fee,
    src: f.source === "exchange" ? "exchange" : "simulated",
  };
}

export function adaptSnapshot(f: SnapshotFrame): LiveData {
  const a = f.account;
  const equityDelta = a.equity - a.initial_balance;

  const account: Account = {
    equity: a.equity,
    equityDelta,
    equityPct: a.initial_balance ? (equityDelta / a.initial_balance) * 100 : 0,
    cash: a.cash,
    cashDelta: 0, // engine snapshot carries no period deltas yet
    realized: a.realized_pnl,
    realizedDelta: 0,
    unrealized: a.unrealized_pnl,
    unrealizedDelta: 0,
  };

  const positions: Position[] = f.positions.map((p) => {
    const side: "long" | "short" = p.qty >= 0 ? "long" : "short";
    const qty = Math.abs(p.qty);
    return { sym: p.symbol, side, qty, entry: p.avg_entry, mark: p.mark, upnl: p.unrealized, notional: p.mark * qty };
  });

  // Design "Open Lots" panel is the SL↔TP band viz → engine brackets carry
  // the stop/target. Brackets without both legs are skipped (nothing to draw).
  const lots: Lot[] = f.brackets
    .filter((b) => b.stop_loss != null && b.take_profit != null)
    .map((b) => ({
      id: "L-" + b.opener_order_id,
      strat: b.strategy_name,
      sym: b.symbol,
      side: longShort(b.side),
      entry: b.entry_price,
      sl: b.stop_loss as number,
      tp: b.take_profit as number,
      mark: b.mark,
      mgr: b.venue_managed ? "venue" : "engine",
    }));

  const book: Book = {
    bids: f.l2.bids.map((l) => ({ px: l.price, sz: l.size, cum: l.cum })),
    asks: f.l2.asks.map((l) => ({ px: l.price, sz: l.size, cum: l.cum })),
    bidVol: f.l2.cum_bid_size,
    askVol: f.l2.cum_ask_size,
    mid: f.l2.mid,
    spread: f.l2.best_ask - f.l2.best_bid,
  };

  const fills: Fill[] = f.recent_fills.map(adaptFill);

  const strategies: Strategy[] = f.strategies.map(adaptStrategy);

  const r = f.risk;
  const risk: RiskLimit[] = [
    { name: "Daily loss", used: r.daily_loss, limit: r.daily_loss_limit, unit: "$", inv: true },
    { name: "Max drawdown", used: r.max_drawdown_pct, limit: r.max_drawdown_limit, unit: "%", inv: true },
    { name: "Gross exposure", used: r.exposure, limit: r.exposure_limit, unit: "$" },
    { name: "Open orders", used: r.open_orders, limit: r.open_orders_limit, unit: "" },
  ];

  const h = f.health;
  const ringDrops =
    h.ring_drops_logging + h.ring_drops_risk + h.ring_drops_stats + h.ring_drops_observer + h.ring_drops_risk_stats + h.ring_drops_mm;
  const health: Health = {
    latAvg: Math.round(h.avg_tick_to_trade_us),
    latMin: Math.round(h.min_tick_to_trade_us),
    latMax: Math.round(h.max_tick_to_trade_us),
    ringDrops,
    provider: h.provider_present ? "OK" : "DOWN",
    questdb: h.questdb.active ? (h.questdb.connected ? "PERSISTING" : "DISCONNECTED") : "OFF",
    eventsPerSec: Math.round(h.rate_ev_per_sec),
    uptime: "—", // engine snapshot has no uptime string; filled by header meta instead
  };

  // Live equity sparkline/chart: engine trend tails. drawdown_tail is a
  // positive %, the chart wants a negative fraction.
  let peak = -Infinity;
  const equityCurve: EquityPoint[] = f.trend.equity_tail.map((eq, i) => {
    peak = Math.max(peak, eq);
    const dd = -(f.trend.drawdown_tail[i] ?? 0) / 100;
    return { i, eq, peak, dd };
  });

  return { halted: f.risk.halted, account, positions, lots, book, fills, strategies, risk, health, equityCurve };
}
