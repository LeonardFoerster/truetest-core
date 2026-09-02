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
  win_count?: number; // additive in schema v1; absent in older fixtures
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

/** v2 — process memory from /proc + in-process pool/ring footprints. */
export interface WireMemPool {
  name: string;
  blocks: number;
  slot_size: number;
  bytes: number;
  in_use: number;
  capacity_slots: number;
  grow_count: number;
}

export interface WireMemRing {
  name: string;
  capacity: number;
  element_bytes: number;
  bytes: number;
}

export interface WireMemory {
  available: boolean;
  rss_bytes: number;
  vm_bytes: number;
  peak_rss_bytes: number;
  heap_bytes: number;
  pool_bytes_total: number;
  ring_bytes_total: number;
  pools: WireMemPool[];
  rings: WireMemRing[];
}

export interface WireDebugRing {
  name: string;
  size: number;
  hwm: number;
  capacity: number;
  drops: number;
}

export interface WireDebugPool {
  name: string;
  blocks: number;
  block_size: number;
  capacity: number;
  in_use: number;
  grow_count: number;
}

export interface WireDebugError {
  name: string;
  msg: string;
}

export interface WireDebugStage {
  name: string;
  calls: number;
  avg_ns: number;
  min_ns: number;
  max_ns: number;
}

/** v2 — engine introspection (workers, rings, pools, mode). */
export interface WireDebug {
  target: string;
  mode: string;
  has_binance: boolean;
  has_questdb: boolean;
  has_debug: boolean;
  has_live_data: boolean;
  preset: string;
  worker_count: number;
  cpu_pin: boolean;
  spin_policy: string;
  event_count: number;
  pending_orders: number;
  open_orders_cache: number;
  armed_brackets: number;
  rings: WireDebugRing[];
  pools: WireDebugPool[];
  errors: WireDebugError[];
  stages?: WireDebugStage[];
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
  /** schema_version >= 2 */
  memory?: WireMemory;
  /** schema_version >= 2 */
  debug?: WireDebug;
}

export interface WireSubAnalytics {
  total_pnl: number;
  trade_count: number;
  win_count: number;
  win_rate: number; // 0..100
  total_win: number;
  total_loss: number;
  profit_factor: number;
  profit_factor_valid: boolean;
  profit_factor_unbounded: boolean;
  profit_factor_reason: string;
}

export interface WireTrade {
  order_id: string; // uint64 decimal string; never cross JS Number precision
  fill_id: string; // uint64 decimal string; "0" requires venue_execution_id
  venue_execution_id: string;
  side: "buy" | "sell";
  quantity: number;
  fill_price: number;
  commission: number;
  commission_currency: string;
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
  gross_realized_pnl: number;
  realized_pnl: number;
  funding_pnl: number;
  unrealized_pnl: number;
  total_commission: number;
  reconciliation_residual: number;
  accounting_reconciled: boolean;
  accounting_reconciliation_reason: string;
  valuation_complete?: boolean;
  valuation_reason?: string;
  portfolio_time_series_valid?: boolean;
  portfolio_time_series_reason?: string;
  ambiguous_portfolio_mark_sequences_rejected?: number;
  return_observation_basis?: string;
  equity_curve_sample_stride?: number;
  cumulative_return: number;
  annualized_return: number;
  annualized_return_valid?: boolean;
  annualized_return_reason?: string;
  annualized_return_basis?: string;
  sharpe_ratio: number;
  sharpe_ratio_valid?: boolean;
  sharpe_ratio_reason?: string;
  sortino_ratio: number;
  sortino_ratio_valid?: boolean;
  sortino_ratio_reason?: string;
  max_drawdown: number; // positive %
  calmar_ratio: number;
  calmar_ratio_valid?: boolean;
  calmar_ratio_reason?: string;
  rolling_sharpe: number;
  rolling_max_drawdown: number;
  win_rate: number; // 0..100
  total_win: number;
  total_loss: number;
  profit_factor: number;
  profit_factor_valid: boolean;
  profit_factor_unbounded: boolean;
  profit_factor_reason: string;
  total_trades: number;
  winning_trades: number;
  total_orders: number;
  total_fills: number;
  duplicate_fill_replays_ignored: number;
  conflicting_fill_replays_rejected: number;
  missing_fill_identities_rejected: number;
  invalid_fill_payloads_rejected: number;
  unreconciled_funding_events_rejected: number;
  duplicate_funding_replays_ignored: number;
  conflicting_funding_replays_rejected: number;
  late_fill_events_rejected: number;
  late_funding_events_rejected: number;
  late_market_events_rejected?: number;
  duplicate_market_marks_ignored?: number;
  conflicting_market_marks_rejected?: number;
  avg_win: number;
  avg_loss: number;
  largest_winner: number;
  largest_loser: number;
  time_in_market_pct: number;
  time_in_market_valid?: boolean;
  time_in_market_reason?: string;
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
  benchmark_valid?: boolean;
  benchmark_reason?: string;
  benchmark_symbol?: string;
  benchmark_equity_curve_sample_stride?: number;
  benchmark_curve_observation_basis?: string;
  alpha: number;
  beta: number;
  information_ratio: number;
  tracking_error: number;
  equity_curve: [number, number][]; // [ts_ms, equity]
  benchmark_equity_curve: [number, number][];
  trade_returns: number[];
  trade_rows_kind: "physical_fill_legs";
  trade_returns_kind: "closing_fill_legs";
  per_symbol: Record<string, WireSubAnalytics>;
  per_strategy: Record<string, WireSubAnalytics>;
  trades: WireTrade[];
}
