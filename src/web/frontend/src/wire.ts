/* =========================================================================
   TrueTest — WIRE types: the exact JSON the C++ serializers emit.

   These mirror src/web/snapshot_json.cpp (SnapshotFrame) and
   src/web/report_json.cpp (ResultsReport) — a faithful projection of the
   engine structs (dashboard_snapshot, AnalyticsReport), snake_case field
   names and engine conventions (signed position qty, drawdown as positive %,
   side as a single char, etc.). The adapters in ./adapters/ translate these
   into the component prop shapes in ./types.ts. Keep this file in lockstep
   with the C++ serializers; bump schema_version on breaking changes.
   ========================================================================= */

export interface WirePosition {
  symbol: string;
  qty: number; // signed: negative = short
  avg_entry: number;
  mark: number;
  unrealized: number;
}

export interface WireLot {
  opener_order_id: number;
  symbol: string;
  strategy_name: string;
  side: string; // 'L' | 'S'
  qty_open: number;
  entry_price: number;
  age_seconds: number;
}

export interface WireOpenOrder {
  order_id: number;
  symbol: string;
  strategy_name: string;
  side: string; // 'B' | 'S'
  type: string; // 'M' | 'L' | 'S' | 's'
  qty: number;
  price: number;
  age_seconds: number;
  status: string;
}

export interface WireFill {
  ts_ms: number;
  symbol: string;
  side: string; // 'B'/'S' or 'b'/'s'
  qty: number;
  price: number;
  fee: number;
  source: string; // "exchange" | "simulated" | "local"
}

export interface WireBracket {
  opener_order_id: number;
  strategy_name: string;
  symbol: string;
  side: string; // 'L' | 'S'
  qty: number;
  entry_price: number;
  stop_loss: number | null;
  take_profit: number | null;
  mark: number;
  venue_managed: boolean;
  venue_list_id: string;
  age_seconds: number;
}

export interface WireStrategy {
  name: string;
  pnl: number;
  trade_count: number;
  win_count: number;
  win_rate: number; // engine scale; adapter normalizes to 0..1
  profit_factor: number;
  total_win: number;
  total_loss: number;
  open_lots: number;
  armed_brackets: number;
}

export interface WireRisk {
  halted: boolean;
  daily_loss: number;
  daily_loss_limit: number;
  max_drawdown_pct: number;
  max_drawdown_limit: number;
  exposure: number;
  exposure_limit: number;
  open_orders: number;
  open_orders_limit: number;
}

export interface WirePerf {
  total_orders: number;
  total_fills: number;
  total_trades: number;
  win_rate: number;
  sharpe: number;
  sortino: number;
  profit_factor: number;
  avg_markout_bps: number;
  markout_samples: number;
}

export interface WireL2Level {
  price: number;
  size: number;
  cum: number;
}

export interface WireL2 {
  symbol: string;
  source: string;
  total_bid_levels: number;
  total_ask_levels: number;
  bids: WireL2Level[];
  asks: WireL2Level[];
  best_bid: number;
  best_ask: number;
  mid: number;
  spread_bps: number;
  microprice: number;
  imbalance: number;
  cum_bid_size: number;
  cum_ask_size: number;
}

export interface WireHealth {
  avg_tick_to_trade_us: number;
  min_tick_to_trade_us: number;
  max_tick_to_trade_us: number;
  tick_to_trade_samples: number;
  events_total: number;
  fills_total: number;
  orders_total: number;
  trades_total: number;
  ring_drops_logging: number;
  ring_drops_risk: number;
  ring_drops_stats: number;
  ring_drops_observer: number;
  ring_drops_risk_stats: number;
  ring_drops_mm: number;
  provider_present: boolean;
  provider_name: string;
  provider_state: number;
  rate_ev_per_sec: number;
  questdb: {
    active: boolean;
    connected: boolean;
    pending_lines: number;
    dropped_lines: number;
    fallback_lines: number;
    last_flush_age_ms: number;
    strict_mode: boolean;
  };
}

export interface WireTrend {
  equity_tail: number[];
  drawdown_tail: number[]; // positive %
  rate_tail: number[];
  equity_now: number;
  equity_change_pct: number;
  drawdown_now_pct: number;
  rate_now: number;
}

export interface SnapshotFrame {
  schema_version: number;
  account: { cash: number; equity: number; initial_balance: number; realized_pnl: number; unrealized_pnl: number };
  positions: WirePosition[];
  lots: WireLot[];
  open_orders: WireOpenOrder[];
  recent_fills: WireFill[];
  brackets: WireBracket[];
  strategies: WireStrategy[];
  risk: WireRisk;
  perf: WirePerf;
  l2: WireL2;
  health: WireHealth;
  trend: WireTrend;
}

export interface WireSubAnalytics {
  total_pnl: number;
  trade_count: number;
  win_count: number;
  win_rate: number; // 0..100
  profit_factor: number;
}

export interface WireTrade {
  order_id: number;
  side: "buy" | "sell";
  quantity: number;
  fill_price: number;
  commission: number;
  intended_price: number;
  ts_ms: number;
  pnl: number;
  symbol: string;
  strategy_name: string;
}

export interface ResultsReport {
  schema_version: number;
  initial_equity: number;
  final_equity: number;
  cumulative_return: number;
  annualized_return: number;
  sharpe_ratio: number;
  sortino_ratio: number;
  max_drawdown: number; // positive %
  calmar_ratio: number;
  rolling_sharpe: number;
  rolling_max_drawdown: number;
  win_rate: number; // 0..100
  profit_factor: number;
  total_trades: number;
  winning_trades: number;
  total_orders: number;
  total_fills: number;
  avg_win: number;
  avg_loss: number;
  largest_winner: number;
  largest_loser: number;
  time_in_market_pct: number;
  avg_holding_period_ms: number;
  avg_slippage: number;
  avg_slippage_signed: number;
  avg_adverse_slippage: number;
  avg_favorable_slippage: number;
  adverse_slippage_count: number;
  favorable_slippage_count: number;
  avg_tick_to_trade_ns: number;
  min_tick_to_trade_ns: number;
  max_tick_to_trade_ns: number;
  tick_to_trade_samples: number;
  buy_and_hold_return: number;
  strategy_vs_benchmark: number;
  alpha: number;
  beta: number;
  information_ratio: number;
  tracking_error: number;
  equity_curve: [number, number][]; // [ts_ms, equity]
  benchmark_equity_curve: [number, number][];
  trade_returns: number[];
  per_symbol: Record<string, WireSubAnalytics>;
  per_strategy: Record<string, WireSubAnalytics>;
  trades: WireTrade[];
}
