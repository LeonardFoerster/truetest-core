/* =========================================================================
   TrueTest — report adapter.

   Maps the engine's ResultsReport (./wire.ts, faithful to AnalyticsReport)
   onto the Backtest Review component prop shapes (./types.ts). Reconciles:
   max_drawdown %→negative fraction, win_rate 0..100→0..1, [ts,equity] pair
   curves → indexed {i,v,dd} series (recomputing drawdown), trades→blotter
   (buy/sell→long/short, slippage bps), trades→entry/exit markers, and builds
   the P&L histogram + per-symbol/strategy breakdowns the engine doesn't
   pre-aggregate for the chart.
   ========================================================================= */
import type { ResultsReport, WireSubAnalytics } from "../wire";
import type { Backtest, BTPoint, BTMarker, BlotterRow, HistBin, BreakdownRow } from "../types";

export interface ReportData {
  backtest: Backtest;
  btCurve: { strat: BTPoint[]; bench: BTPoint[] };
  btMarkers: BTMarker[];
  blotter: BlotterRow[];
  hist: HistBin[];
  bySymbol: BreakdownRow[];
  byStrategy: BreakdownRow[];
}

const frac = (v: number) => (v > 1 ? v / 100 : v);

function breakdown(m: Record<string, WireSubAnalytics>): BreakdownRow[] {
  return Object.entries(m)
    .map(([key, sa]) => ({ key, pnl: sa.total_pnl, win: frac(sa.win_rate), pf: sa.profit_factor, trades: sa.trade_count }))
    .sort((a, b) => b.pnl - a.pnl);
}

function stratSeries(curve: [number, number][]): BTPoint[] {
  let peak = -Infinity;
  return curve.map(([, v], i) => {
    peak = Math.max(peak, v);
    return { i, v, dd: (v - peak) / peak };
  });
}

// Nearest curve index for a trade timestamp (curve is time-ascending).
function nearestIndex(curve: [number, number][], tsMs: number): number {
  if (curve.length === 0) return 0;
  let lo = 0,
    hi = curve.length - 1;
  while (lo < hi) {
    const m = (lo + hi) >> 1;
    if (curve[m][0] < tsMs) lo = m + 1;
    else hi = m;
  }
  return lo;
}

function histogram(pnls: number[]): HistBin[] {
  const bins = new Array(13).fill(0);
  const lo = -2500,
    hi = 2500,
    w = (hi - lo) / bins.length;
  for (const p of pnls) {
    let idx = Math.floor((p - lo) / w);
    idx = Math.max(0, Math.min(bins.length - 1, idx));
    bins[idx]++;
  }
  return bins.map((c, i) => ({ x0: lo + i * w, x1: lo + (i + 1) * w, c }));
}

export function adaptReport(r: ResultsReport): ReportData {
  const avgTrade = r.total_trades ? (r.final_equity - r.initial_equity) / r.total_trades : 0;

  const backtest: Backtest = {
    name: "Backtest run",
    range: "—",
    bars: "—",
    universe: Object.keys(r.per_symbol).join(" · ") || "—",
    headline: {
      totalReturn: r.cumulative_return,
      sharpe: r.sharpe_ratio,
      sortino: r.sortino_ratio,
      calmar: r.calmar_ratio,
      maxDD: -Math.abs(r.max_drawdown) / 100,
      winRate: frac(r.win_rate),
      profitFactor: r.profit_factor,
      totalTrades: r.total_trades,
      finalEquity: r.final_equity,
      startEquity: r.initial_equity,
      cagr: r.annualized_return,
      avgTrade,
    },
    bench: {
      alpha: r.alpha,
      beta: r.beta,
      infoRatio: r.information_ratio,
      vsBH: r.strategy_vs_benchmark,
      bhReturn: r.buy_and_hold_return,
    },
  };

  const strat = stratSeries(r.equity_curve);
  const bench: BTPoint[] = r.benchmark_equity_curve.map(([, v], i) => ({ i, v }));
  const btCurve = { strat, bench };

  // A readable subset of trades projected onto the curve as entry/exit marks.
  const step = Math.max(1, Math.floor(r.trades.length / 14));
  const btMarkers: BTMarker[] = [];
  for (let k = 0; k < r.trades.length; k += step) {
    const t = r.trades[k];
    const i = nearestIndex(r.equity_curve, t.ts_ms);
    btMarkers.push({
      i,
      kind: t.side === "buy" ? "entry" : "exit",
      side: t.side === "buy" ? "long" : "short",
      v: strat[i]?.v ?? r.equity_curve[i]?.[1] ?? 0,
    });
  }

  const blotter: BlotterRow[] = r.trades.map((t) => ({
    id: t.order_id,
    day: 0,
    sym: t.symbol,
    strat: t.strategy_name,
    side: t.side === "buy" ? "long" : "short",
    qty: t.quantity,
    fill: t.fill_price,
    intended: t.intended_price,
    slipBps: t.intended_price ? ((t.fill_price - t.intended_price) / t.intended_price) * 1e4 : 0,
    comm: t.commission,
    pnl: t.pnl,
  }));

  const hist = histogram(r.trades.map((t) => t.pnl));

  return {
    backtest,
    btCurve,
    btMarkers,
    blotter,
    hist,
    bySymbol: breakdown(r.per_symbol),
    byStrategy: breakdown(r.per_strategy),
  };
}
