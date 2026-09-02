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

export const SNAPSHOT_SCHEMA_VERSION = 3 as const;
// Browser/server clocks may differ slightly. Larger future timestamps are
// rejected because treating them as fresh can keep a dead feed green.
export const SNAPSHOT_FUTURE_TOLERANCE_MS = 5_000;

export type WireMetric = number | null;

export interface WireAccount {
  cash: number;
  initial_balance: number;
  equity: WireMetric;
  equity_available: boolean;
  total_pnl: WireMetric;
  total_pnl_available: boolean;
  realized_pnl: WireMetric;
  realized_pnl_available: boolean;
  unrealized_pnl: WireMetric;
  unrealized_pnl_available: boolean;
}

export interface WirePosition {
  symbol: string;
  qty: number; // signed: negative = short
  avg_entry: number;
  mark: WireMetric;
  mark_available: boolean;
  unrealized: WireMetric;
  unrealized_available: boolean;
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
  trigger_price: WireMetric;
  trigger_price_available: boolean;
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
  source: "exchange" | "simulated" | "unknown";
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
  mark: WireMetric;
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
  daily_loss: WireMetric;
  daily_loss_available: boolean;
  daily_loss_limit: number;
  max_drawdown_pct: WireMetric;
  max_drawdown_available: boolean;
  max_drawdown_limit: number;
  exposure: WireMetric;
  exposure_available: boolean;
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
  equity_now: WireMetric;
  equity_available: boolean;
  equity_change_pct: WireMetric;
  equity_change_available: boolean;
  drawdown_now_pct: WireMetric;
  drawdown_now_available: boolean;
  rate_now: number;
}

/** v3 — queue-position metric with explicit zero-vs-unavailable semantics. */
export interface WireQueue {
  avg_bps: number | null;
  available: boolean;
  submitted_with_queue: number;
  filled_after_drain: number;
  blocked_at_eos: number;
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
  schema_version: typeof SNAPSHOT_SCHEMA_VERSION;
  generated_at_ms: number | null;
  generated_at_available: boolean;
  account: WireAccount;
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
  /** schema_version >= 3 */
  queue: WireQueue;
  /** schema_version >= 2 */
  memory?: WireMemory;
  /** schema_version >= 2 */
  debug?: WireDebug;
}

export class SnapshotContractError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "SnapshotContractError";
  }
}

type JsonObject = Record<string, unknown>;

function contractFailure(path: string, expected: string): never {
  throw new SnapshotContractError(
    `Rejected SnapshotFrame: ${path} must be ${expected}`,
  );
}

function objectValue(value: unknown, path: string): JsonObject {
  if (typeof value !== "object" || value === null || Array.isArray(value))
    contractFailure(path, "an object");
  return value as JsonObject;
}

function arrayValue(value: unknown, path: string): unknown[] {
  if (!Array.isArray(value)) contractFailure(path, "an array");
  return value;
}

function finiteNumber(value: unknown, path: string): number {
  if (typeof value !== "number" || !Number.isFinite(value))
    contractFailure(path, "a finite number");
  return value;
}

function booleanValue(value: unknown, path: string): boolean {
  if (typeof value !== "boolean") contractFailure(path, "a boolean");
  return value;
}

function stringValue(value: unknown, path: string): string {
  if (typeof value !== "string") contractFailure(path, "a string");
  return value;
}

function nullableFiniteNumber(value: unknown, path: string): number | null {
  if (value === null) return null;
  return finiteNumber(value, path);
}

function metricPair(
  object: JsonObject,
  valueKey: string,
  availabilityKey: string,
  path: string,
): void {
  const available = booleanValue(
    object[availabilityKey],
    `${path}.${availabilityKey}`,
  );
  const value = nullableFiniteNumber(object[valueKey], `${path}.${valueKey}`);
  if (available !== (value !== null))
    contractFailure(
      `${path}.${valueKey}/${availabilityKey}`,
      "a consistent number/true or null/false pair",
    );
}

function numericFields(object: JsonObject, keys: string[], path: string): void {
  for (const key of keys) finiteNumber(object[key], `${path}.${key}`);
}

function finiteNumberArray(value: unknown, path: string): void {
  arrayValue(value, path).forEach((item, index) =>
    finiteNumber(item, `${path}[${index}]`),
  );
}

function validateSnapshotShape(frame: JsonObject): void {
  metricPair(frame, "generated_at_ms", "generated_at_available", "snapshot");
  const generatedAt = frame.generated_at_ms;
  if (generatedAt !== null) {
    const timestamp = finiteNumber(generatedAt, "snapshot.generated_at_ms");
    if (timestamp < 0)
      contractFailure("snapshot.generated_at_ms", "a Unix timestamp at or after epoch");
    if (timestamp > Date.now() + SNAPSHOT_FUTURE_TOLERANCE_MS) {
      contractFailure(
        "snapshot.generated_at_ms",
        `no more than ${SNAPSHOT_FUTURE_TOLERANCE_MS} ms in the future`,
      );
    }
  }

  const account = objectValue(frame.account, "account");
  numericFields(account, ["cash", "initial_balance"], "account");
  metricPair(account, "equity", "equity_available", "account");
  metricPair(account, "total_pnl", "total_pnl_available", "account");
  metricPair(account, "realized_pnl", "realized_pnl_available", "account");
  metricPair(account, "unrealized_pnl", "unrealized_pnl_available", "account");

  arrayValue(frame.positions, "positions").forEach((value, index) => {
    const path = `positions[${index}]`;
    const position = objectValue(value, path);
    stringValue(position.symbol, `${path}.symbol`);
    numericFields(position, ["qty", "avg_entry"], path);
    metricPair(position, "mark", "mark_available", path);
    metricPair(position, "unrealized", "unrealized_available", path);
  });

  arrayValue(frame.lots, "lots");
  arrayValue(frame.open_orders, "open_orders").forEach((value, index) => {
    const path = `open_orders[${index}]`;
    const order = objectValue(value, path);
    stringValue(order.symbol, `${path}.symbol`);
    stringValue(order.strategy_name, `${path}.strategy_name`);
    stringValue(order.side, `${path}.side`);
    stringValue(order.type, `${path}.type`);
    stringValue(order.status, `${path}.status`);
    numericFields(order, ["order_id", "qty", "price", "age_seconds"], path);
    metricPair(order, "trigger_price", "trigger_price_available", path);
  });

  arrayValue(frame.recent_fills, "recent_fills").forEach((value, index) => {
    const path = `recent_fills[${index}]`;
    const fill = objectValue(value, path);
    stringValue(fill.symbol, `${path}.symbol`);
    stringValue(fill.side, `${path}.side`);
    numericFields(fill, ["ts_ms", "qty", "price", "fee"], path);
    const source = stringValue(fill.source, `${path}.source`);
    if (source !== "exchange" && source !== "simulated" && source !== "unknown")
      contractFailure(`${path}.source`, "exchange, simulated, or unknown");
  });

  arrayValue(frame.brackets, "brackets").forEach((value, index) => {
    const path = `brackets[${index}]`;
    const bracket = objectValue(value, path);
    stringValue(bracket.strategy_name, `${path}.strategy_name`);
    stringValue(bracket.symbol, `${path}.symbol`);
    stringValue(bracket.side, `${path}.side`);
    stringValue(bracket.venue_list_id, `${path}.venue_list_id`);
    booleanValue(bracket.venue_managed, `${path}.venue_managed`);
    numericFields(
      bracket,
      ["opener_order_id", "qty", "entry_price", "age_seconds"],
      path,
    );
    nullableFiniteNumber(bracket.stop_loss, `${path}.stop_loss`);
    nullableFiniteNumber(bracket.take_profit, `${path}.take_profit`);
    nullableFiniteNumber(bracket.mark, `${path}.mark`);
  });

  arrayValue(frame.strategies, "strategies").forEach((value, index) => {
    const path = `strategies[${index}]`;
    const strategy = objectValue(value, path);
    stringValue(strategy.name, `${path}.name`);
    numericFields(
      strategy,
      ["pnl", "trade_count", "win_count", "win_rate", "profit_factor",
       "total_win", "total_loss", "open_lots", "armed_brackets"],
      path,
    );
  });

  const risk = objectValue(frame.risk, "risk");
  booleanValue(risk.halted, "risk.halted");
  metricPair(risk, "daily_loss", "daily_loss_available", "risk");
  metricPair(risk, "max_drawdown_pct", "max_drawdown_available", "risk");
  metricPair(risk, "exposure", "exposure_available", "risk");
  numericFields(
    risk,
    ["daily_loss_limit", "max_drawdown_limit", "exposure_limit",
     "open_orders", "open_orders_limit"],
    "risk",
  );

  const perf = objectValue(frame.perf, "perf");
  numericFields(
    perf,
    ["total_orders", "total_fills", "total_trades", "win_rate", "sharpe",
     "sortino", "profit_factor", "avg_markout_bps", "markout_samples"],
    "perf",
  );

  const l2 = objectValue(frame.l2, "l2");
  stringValue(l2.symbol, "l2.symbol");
  stringValue(l2.source, "l2.source");
  numericFields(
    l2,
    ["total_bid_levels", "total_ask_levels", "best_bid", "best_ask", "mid",
     "spread_bps", "microprice", "imbalance", "cum_bid_size", "cum_ask_size"],
    "l2",
  );
  for (const side of ["bids", "asks"] as const) {
    arrayValue(l2[side], `l2.${side}`).forEach((value, index) => {
      const level = objectValue(value, `l2.${side}[${index}]`);
      numericFields(level, ["price", "size", "cum"], `l2.${side}[${index}]`);
    });
  }

  const health = objectValue(frame.health, "health");
  numericFields(
    health,
    ["avg_tick_to_trade_us", "min_tick_to_trade_us", "max_tick_to_trade_us",
     "tick_to_trade_samples", "events_total", "fills_total", "orders_total",
     "trades_total", "ring_drops_logging", "ring_drops_risk",
     "ring_drops_stats", "ring_drops_observer", "ring_drops_risk_stats",
     "ring_drops_mm", "provider_state", "rate_ev_per_sec"],
    "health",
  );
  booleanValue(health.provider_present, "health.provider_present");
  stringValue(health.provider_name, "health.provider_name");
  const questdb = objectValue(health.questdb, "health.questdb");
  booleanValue(questdb.active, "health.questdb.active");
  booleanValue(questdb.connected, "health.questdb.connected");
  booleanValue(questdb.strict_mode, "health.questdb.strict_mode");
  numericFields(
    questdb,
    ["pending_lines", "dropped_lines", "fallback_lines", "last_flush_age_ms"],
    "health.questdb",
  );

  const trend = objectValue(frame.trend, "trend");
  finiteNumberArray(trend.equity_tail, "trend.equity_tail");
  finiteNumberArray(trend.drawdown_tail, "trend.drawdown_tail");
  finiteNumberArray(trend.rate_tail, "trend.rate_tail");
  metricPair(trend, "equity_now", "equity_available", "trend");
  metricPair(trend, "equity_change_pct", "equity_change_available", "trend");
  metricPair(trend, "drawdown_now_pct", "drawdown_now_available", "trend");
  finiteNumber(trend.rate_now, "trend.rate_now");

  const queue = objectValue(frame.queue, "queue");
  const queueAvailable = booleanValue(queue.available, "queue.available");
  const averageQueue = nullableFiniteNumber(queue.avg_bps, "queue.avg_bps");
  if (queueAvailable !== (averageQueue !== null))
    contractFailure("queue.avg_bps/available", "a consistent number/true or null/false pair");
  numericFields(
    queue,
    ["submitted_with_queue", "filled_after_drain", "blocked_at_eos"],
    "queue",
  );

  if (frame.memory !== undefined) objectValue(frame.memory, "memory");
  if (frame.debug !== undefined) objectValue(frame.debug, "debug");
}

/**
 * Establish the version boundary before an untyped WebSocket/fixture payload
 * reaches the adapter. Field shapes remain compile-time checked by
 * SnapshotFrame; incompatible versions are rejected instead of being
 * optimistically cast and rendered.
 */
export function parseSnapshotFrame(value: unknown): SnapshotFrame {
  if (typeof value !== "object" || value === null || Array.isArray(value)) {
    throw new SnapshotContractError("Rejected SnapshotFrame: expected a JSON object");
  }

  const version = (value as { schema_version?: unknown }).schema_version;
  if (version !== SNAPSHOT_SCHEMA_VERSION) {
    throw new SnapshotContractError(
      `Rejected SnapshotFrame schema_version ${String(version)}; expected ${SNAPSHOT_SCHEMA_VERSION}`,
    );
  }

  const frame = value as JsonObject;
  validateSnapshotShape(frame);
  return frame as unknown as SnapshotFrame;
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
