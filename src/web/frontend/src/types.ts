/* =========================================================================
   TrueTest — web UI data model.

   These types are the *contract* the live cockpit and backtest report render
   against. They mirror the engine's in-memory state:
     - live  ⟵ engine::snapshot_dashboard(dashboard_snapshot&)   (SnapshotFrame)
     - report ⟵ Analytics::generate_report() → AnalyticsReport     (ResultsReport)
   For now they are populated by the bundled fixture (./data.ts). When the C++
   `snapshot_json` / `report_json` serializers land, the data layer swaps the
   fixture for the WS/REST feed and these shapes stay fixed.
   ========================================================================= */

export type Side = "long" | "short";
export type FillSide = "buy" | "sell";
export type Manager = "engine" | "venue";

export interface SymbolMeta {
  px: number;
  tick: number;
  color: string;
  short: string;
}

export interface Account {
  equity: number | null;
  equityDelta: number | null;
  equityPct: number | null;
  cash: number;
  cashDelta: number | null;
  realized: number | null;
  realizedDelta: number | null;
  unrealized: number | null;
  unrealizedDelta: number | null;
}

export interface EquityPoint {
  i: number;
  eq: number;
  peak: number;
  dd: number;
}

export interface Position {
  sym: string;
  side: Side;
  qty: number;
  entry: number;
  mark: number | null;
  upnl: number | null;
  notional: number | null;
}

export interface Lot {
  id: string;
  strat: string;
  sym: string;
  side: Side;
  entry: number;
  sl: number;
  tp: number;
  mark: number;
  mgr: Manager;
}

export interface BookLevel {
  px: number;
  sz: number;
  cum: number;
}

export interface Book {
  bids: BookLevel[];
  asks: BookLevel[];
  bidVol: number;
  askVol: number;
  mid: number;
  spread: number;
}

export interface Fill {
  id: number;
  t: number | null;
  sym: string;
  side: FillSide;
  qty: number;
  px: number;
  fee: number;
  src: "exchange" | "simulated" | "unknown";
  isNew?: boolean;
}

export interface Strategy {
  name: string;
  pnl: number;
  win: number;
  pf: number;
  trades: number;
  lots: number;
  spark: number[];
}

export interface RiskLimit {
  name: string;
  used: number | null;
  limit: number;
  unit: "$" | "%" | "";
  inv?: boolean;
}

export interface Health {
  latAvg: number;
  latMin: number;
  latMax: number;
  ringDrops: number;
  provider: string;
  questdb: string;
  eventsPerSec: number;
  uptime: string;
}

export interface BacktestHeadline {
  totalReturn: number;
  sharpe: number;
  sortino: number;
  calmar: number;
  maxDD: number;
  winRate: number;
  profitFactor: number;
  totalTrades: number;
  finalEquity: number;
  startEquity: number;
  cagr: number;
  avgTrade: number;
}

export interface BacktestBench {
  alpha: number;
  beta: number;
  infoRatio: number;
  vsBH: number;
  bhReturn: number;
}

export interface Backtest {
  name: string;
  range: string;
  bars: string;
  universe: string;
  headline: BacktestHeadline;
  bench: BacktestBench;
}

export interface BTPoint {
  i: number;
  v: number;
  dd?: number;
}

export interface BTMarker {
  i: number;
  kind: "entry" | "exit";
  side: Side;
  v: number;
}

export interface BlotterRow {
  id: number;
  day: number;
  sym: string;
  strat: string;
  side: Side;
  qty: number;
  fill: number;
  intended: number;
  slipBps: number;
  comm: number;
  pnl: number;
}

export interface HistBin {
  x0: number;
  x1: number;
  c: number;
}

export interface BreakdownRow {
  key: string;
  pnl: number;
  win: number;
  pf: number;
  trades: number;
}

export interface TTData {
  SYMS: Record<string, SymbolMeta>;
  account: Account;
  equityCurve: EquityPoint[];
  positions: Position[];
  lots: Lot[];
  book: Book;
  fills: Fill[];
  strategies: Strategy[];
  risk: RiskLimit[];
  health: Health;
  backtest: Backtest;
  btCurve: { strat: BTPoint[]; bench: BTPoint[] };
  btMarkers: BTMarker[];
  blotter: BlotterRow[];
  hist: HistBin[];
  bySymbol: BreakdownRow[];
  byStrategy: BreakdownRow[];
  fmtClock: (s: number) => string;
  rnd: () => number;
  gauss: () => number;
  buildBook: (mid: number, tick: number) => Book;
}

export type ConnStatus = "live" | "loading" | "empty" | "degraded" | "disconnected" | "halted";
export type Mode = "live" | "backtest";
