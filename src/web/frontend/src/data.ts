/* =========================================================================
   TrueTest — data model + light simulated feed.
   Ported from the Claude Design prototype (data.js). Realistic quant/crypto
   session data; pure in-page generators, no backend. This is the offline
   fixture the UI renders against until the live WS/REST feed is wired.
   ========================================================================= */
import type { TTData, Book, BlotterRow, HistBin } from "./types";

const SYMS: TTData["SYMS"] = {
  BTCUSDT: { px: 71248.5, tick: 0.5, color: "#f7931a", short: "₿" },
  ETHUSDT: { px: 3842.18, tick: 0.01, color: "#627eea", short: "Ξ" },
  SOLUSDT: { px: 177.94, tick: 0.01, color: "#14f195", short: "◎" },
};

// deterministic-ish PRNG so first paint is stable
let _s = 0x2f6e2b1;
function rnd(): number {
  _s ^= _s << 13;
  _s ^= _s >>> 17;
  _s ^= _s << 5;
  return ((_s >>> 0) % 1e6) / 1e6;
}
function gauss(): number {
  return (rnd() + rnd() + rnd() + rnd() - 2) / 2;
}

// ---- Account ----
const account = {
  equity: 1284316.42,
  equityDelta: 14208.13,
  equityPct: 1.118,
  cash: 642880.1,
  cashDelta: -38120.0,
  realized: 47312.88,
  realizedDelta: 6140.2,
  unrealized: 9893.55,
  unrealizedDelta: -2204.6,
};

// ---- Equity curve (intraday) + drawdown ----
function buildEquity(n: number, start: number) {
  const pts = [];
  let v = start;
  let peak = start;
  for (let i = 0; i < n; i++) {
    v += gauss() * 1400 + 240;
    peak = Math.max(peak, v);
    pts.push({ i, eq: v, peak, dd: (v - peak) / peak });
  }
  return pts;
}
const equityCurve = buildEquity(120, 1238900);
// pin last to account.equity-ish
(function () {
  const last = equityCurve[equityCurve.length - 1];
  const scale = account.equity / last.eq;
  equityCurve.forEach((p) => {
    p.eq *= scale;
    p.peak *= scale;
  });
})();

// ---- Positions ----
const positions = [
  { sym: "BTCUSDT", side: "long" as const, qty: 8.42, entry: 70180.25, mark: 71248.5 },
  { sym: "ETHUSDT", side: "long" as const, qty: 142.0, entry: 3798.4, mark: 3842.18 },
  { sym: "SOLUSDT", side: "short" as const, qty: 1200.0, entry: 182.1, mark: 177.94 },
].map((p) => {
  const dir = p.side === "long" ? 1 : -1;
  const upnl = (p.mark - p.entry) * p.qty * dir;
  return { ...p, upnl, notional: p.mark * p.qty };
});

// ---- Open lots / brackets ----
const lots = [
  { id: "L-4471", strat: "MOM-XBT", sym: "BTCUSDT", side: "long" as const, entry: 70180.25, sl: 69240.0, tp: 72600.0, mark: 71248.5, mgr: "engine" as const },
  { id: "L-4468", strat: "MR-ETH", sym: "ETHUSDT", side: "long" as const, entry: 3798.4, sl: 3742.0, tp: 3930.0, mark: 3842.18, mgr: "venue" as const },
  { id: "L-4459", strat: "BASIS-SOL", sym: "SOLUSDT", side: "short" as const, entry: 182.1, sl: 187.4, tp: 171.0, mark: 177.94, mgr: "engine" as const },
  { id: "L-4455", strat: "MOM-XBT", sym: "BTCUSDT", side: "long" as const, entry: 70910.0, sl: 70050.0, tp: 73100.0, mark: 71248.5, mgr: "engine" as const },
];

// ---- Order book (L2) — top 10 each side ----
function buildBook(mid: number, tick: number): Book {
  const bids = [];
  const asks = [];
  let cb = 0;
  let ca = 0;
  for (let i = 0; i < 10; i++) {
    const bsz = Math.abs(gauss() * 4 + 6) + 0.4;
    const asz = Math.abs(gauss() * 4 + 5.6) + 0.4;
    cb += bsz;
    ca += asz;
    bids.push({ px: mid - tick * (i + 1) * (mid > 1000 ? 8 : 1), sz: bsz, cum: cb });
    asks.push({ px: mid + tick * (i + 1) * (mid > 1000 ? 8 : 1), sz: asz, cum: ca });
  }
  const bidVol = bids.reduce((a, b) => a + b.sz, 0);
  const askVol = asks.reduce((a, b) => a + b.sz, 0);
  return { bids, asks, bidVol, askVol, mid, spread: asks[0].px - bids[0].px };
}
const book = buildBook(71248.5, 0.5);

// ---- Recent fills tape ----
const SIDES = ["buy", "sell"] as const;
function buildFills(n: number) {
  const out = [];
  let t = 9 * 3600 + 41 * 60 + 12;
  for (let i = 0; i < n; i++) {
    const symK = ["BTCUSDT", "ETHUSDT", "SOLUSDT"][Math.floor(rnd() * 3)];
    const base = SYMS[symK].px;
    const side = SIDES[Math.floor(rnd() * 2)];
    const px = base * (1 + gauss() * 0.0006);
    const qty = symK === "BTCUSDT" ? rnd() * 0.6 + 0.02 : symK === "ETHUSDT" ? rnd() * 8 + 0.5 : rnd() * 90 + 5;
    const fee = px * qty * 0.00018;
    out.push({ id: 1e5 - i, t, sym: symK, side, qty, px, fee, src: (rnd() > 0.28 ? "exchange" : "simulated") as "exchange" | "simulated" });
    t -= Math.floor(rnd() * 7 + 1);
  }
  return out;
}
const fills = buildFills(40);
function fmtClock(s: number) {
  const h = Math.floor(s / 3600) % 24,
    m = Math.floor(s / 60) % 60,
    sec = s % 60;
  return String(h).padStart(2, "0") + ":" + String(m).padStart(2, "0") + ":" + String(sec).padStart(2, "0");
}

// ---- Strategies ----
function spark(n: number, bias: number) {
  const a = [];
  let v = 0;
  for (let i = 0; i < n; i++) {
    v += gauss() * 1 + bias * 0.18;
    a.push(v);
  }
  return a;
}
const strategies = [
  { name: "MOM-XBT", pnl: 28940.55, win: 0.612, pf: 2.31, trades: 184, lots: 2, spark: spark(40, 1) },
  { name: "MR-ETH", pnl: 12184.2, win: 0.583, pf: 1.74, trades: 241, lots: 1, spark: spark(40, 1) },
  { name: "BASIS-SOL", pnl: -3420.18, win: 0.476, pf: 0.91, trades: 96, lots: 1, spark: spark(40, -1) },
  { name: "STAT-ARB", pnl: 9601.31, win: 0.668, pf: 2.02, trades: 312, lots: 0, spark: spark(40, 1) },
];

// ---- Risk limits ----
const risk = [
  { name: "Daily loss", used: 8240, limit: 25000, unit: "$" as const, inv: true },
  { name: "Max drawdown", used: 4.18, limit: 8.0, unit: "%" as const, inv: true },
  { name: "Gross exposure", used: 1.42e6, limit: 2.0e6, unit: "$" as const },
  { name: "Open orders", used: 11, limit: 20, unit: "" as const },
];

// ---- System health ----
const health = {
  latAvg: 412,
  latMin: 198,
  latMax: 1840, // microseconds tick-to-trade
  ringDrops: 0,
  provider: "OK",
  questdb: "PERSISTING",
  eventsPerSec: 14820,
  uptime: "06:42:18",
};

// ===================== BACKTEST =====================
const backtest = {
  name: "MOM-XBT + MR-ETH ensemble",
  range: "2025-01-01 → 2025-12-31",
  bars: "1m",
  universe: "BTCUSDT · ETHUSDT · SOLUSDT",
  headline: {
    totalReturn: 0.3842,
    sharpe: 2.14,
    sortino: 3.08,
    calmar: 2.41,
    maxDD: -0.1594,
    winRate: 0.598,
    profitFactor: 1.92,
    totalTrades: 1284,
    finalEquity: 1384200,
    startEquity: 1000000,
    cagr: 0.3842,
    avgTrade: 298.7,
  },
  bench: { alpha: 0.142, beta: 0.61, infoRatio: 0.88, vsBH: 0.121, bhReturn: 0.2632 },
};
// backtest equity vs benchmark
function buildBT(n: number) {
  const strat = [];
  const bench = [];
  let s = 1e6,
    b = 1e6,
    peak = 1e6;
  for (let i = 0; i < n; i++) {
    s *= 1 + (gauss() * 0.011 + 0.0014);
    b *= 1 + (gauss() * 0.013 + 0.0009);
    peak = Math.max(peak, s);
    strat.push({ i, v: s, dd: (s - peak) / peak });
    bench.push({ i, v: b });
  }
  const fs = backtest.headline.finalEquity / s;
  strat.forEach((p) => {
    p.v *= fs;
  });
  return { strat, bench };
}
const btCurve = buildBT(252);

// trade markers (subset on curve)
const btMarkers = [];
for (let i = 8; i < 250; i += 17) btMarkers.push({ i, kind: (rnd() > 0.5 ? "entry" : "exit") as "entry" | "exit", side: (rnd() > 0.5 ? "long" : "short") as "long" | "short", v: btCurve.strat[i].v });

// trade blotter
function buildBlotter(n: number): BlotterRow[] {
  const out: BlotterRow[] = [];
  let day = 0;
  for (let i = 0; i < n; i++) {
    const symK = ["BTCUSDT", "ETHUSDT", "SOLUSDT"][Math.floor(rnd() * 3)];
    const stratN = ["MOM-XBT", "MR-ETH", "BASIS-SOL", "STAT-ARB"][Math.floor(rnd() * 4)];
    const base = SYMS[symK].px * (0.8 + rnd() * 0.4);
    const side = (rnd() > 0.5 ? "long" : "short") as "long" | "short";
    const qty = symK === "BTCUSDT" ? rnd() * 1.5 + 0.05 : symK === "ETHUSDT" ? rnd() * 20 + 1 : rnd() * 200 + 10;
    const intended = base;
    const slipBps = gauss() * 3.2;
    const fill = intended * (1 + slipBps / 1e4);
    const comm = fill * qty * 0.0002;
    const pnl = gauss() * 900 + 110;
    day += Math.floor(rnd() * 3);
    out.push({ id: 5000 - i, day, sym: symK, strat: stratN, side, qty, fill, intended, slipBps, comm, pnl });
  }
  return out;
}
const blotter = buildBlotter(60);

// pnl distribution histogram
function buildHist(trades: BlotterRow[]): HistBin[] {
  const bins = new Array(13).fill(0);
  const lo = -2500,
    hi = 2500,
    w = (hi - lo) / bins.length;
  trades.forEach((t) => {
    let idx = Math.floor((t.pnl - lo) / w);
    idx = Math.max(0, Math.min(bins.length - 1, idx));
    bins[idx]++;
  });
  return bins.map((c, i) => ({ x0: lo + i * w, x1: lo + (i + 1) * w, c }));
}
const hist = buildHist(buildBlotter(400));

// per-symbol + per-strategy breakdown
const bySymbol = [
  { key: "BTCUSDT", pnl: 184200, win: 0.624, pf: 2.18, trades: 512 },
  { key: "ETHUSDT", pnl: 121800, win: 0.591, pf: 1.81, trades: 468 },
  { key: "SOLUSDT", pnl: -22400, win: 0.512, pf: 0.94, trades: 304 },
];
const byStrategy = [
  { key: "MOM-XBT", pnl: 198400, win: 0.641, pf: 2.42, trades: 384 },
  { key: "MR-ETH", pnl: 96200, win: 0.583, pf: 1.74, trades: 401 },
  { key: "STAT-ARB", pnl: 71200, win: 0.612, pf: 1.98, trades: 318 },
  { key: "BASIS-SOL", pnl: -18200, win: 0.498, pf: 0.96, trades: 181 },
];

export const TT: TTData = {
  SYMS,
  account,
  equityCurve,
  positions,
  lots,
  book,
  fills,
  strategies,
  risk,
  health,
  backtest,
  btCurve,
  btMarkers,
  blotter,
  hist,
  bySymbol,
  byStrategy,
  fmtClock,
  rnd,
  gauss,
  buildBook,
};

export default TT;
