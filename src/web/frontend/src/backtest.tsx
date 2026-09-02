/* =========================================================================
   TrueTest — Backtest Review report.
   Render unchanged from the Claude Design prototype; data now comes from the
   adapted ResultsReport (ReportData) instead of a mock global.
   ========================================================================= */
import { useMemo, useState, useRef } from "react";
import { fmt, cls } from "./format";
import { Panel, Sym, Side, useSort } from "./components";
import { BTChart, Histogram } from "./charts";
import { normalizedPosition, normalizedRange } from "./chartMath";
import { REPORT_LIMITS, type ReportData } from "./adapters/report";
import type { Backtest, BTPoint, BTMarker, BlotterRow, BreakdownRow } from "./types";

function MetricBoard({ headline: h }: { headline: Backtest["headline"] }) {
  return (
    <>
      <div className="metric-board">
        <div className="mcell mb-hero">
          <span className="mk">Total Return</span>
          <span className={"mv " + cls(h.totalReturn)}>{fmt.pct(h.totalReturn, 1)}</span>
          <span className="ms">
            {fmt.usd(h.startEquity, 0)} → {fmt.usd(h.finalEquity, 0)} · CAGR{" "}
            {h.cagr === null ? `unsupported (${h.cagrReason})` : fmt.pctP(h.cagr, 1)}
          </span>
        </div>
        <MCell k="Sharpe" v={h.sharpe === null ? "unsupported" : h.sharpe.toFixed(2)} good={h.sharpe === null ? undefined : h.sharpe > 1} s={h.sharpe === null ? h.sharpeReason : "risk-adjusted"} big />
        <MCell k="Sortino" v={h.sortino === null ? (h.sortinoReason === "no_downside_observed_unbounded" ? "unbounded" : "unsupported") : h.sortino.toFixed(2)} good={h.sortino === null ? undefined : h.sortino > 1} s={h.sortino === null ? h.sortinoReason : "downside"} big />
        <MCell
          k="Max Drawdown"
          v={h.maxDD === null ? "unsupported" : fmt.pct(h.maxDD, 2)}
          cls={h.maxDD === null ? undefined : "down"}
          s={h.maxDD === null ? h.maxDDReason : "peak-to-trough"}
        />
        <MCell
          k="Calmar"
          v={h.calmar === null ? "unsupported" : h.calmar.toFixed(2)}
          good={h.calmar === null ? undefined : h.calmar > 1}
          s={h.calmar === null ? h.calmarReason : "annualized ret / maxDD"}
        />
      </div>
      <div className="metric-board" style={{ gridTemplateColumns: "repeat(4, 1fr)" }}>
        <MCell k="Win Rate" v={fmt.pctP(h.winRate, 1)} s={Math.round(h.totalTrades * h.winRate) + " / " + h.totalTrades} />
        <MCell k="Profit Factor" v={h.profitFactor === null ? (h.profitFactorUnbounded ? "unbounded" : "unsupported") : h.profitFactor.toFixed(2)} good={h.profitFactor === null ? undefined : h.profitFactor > 1} s={h.profitFactor === null ? h.profitFactorReason : "gross win / loss"} />
        <MCell k="Total Trades" v={h.totalTrades.toLocaleString()} s="closed positions" />
        <MCell k="Avg Trade" v={fmt.signUsd(h.avgTrade)} cls={cls(h.avgTrade)} s="expectancy" />
      </div>
    </>
  );
}
function MCell({ k, v, s, cls: c, good, big }: any) {
  const color = c ? "var(--" + (c === "up" ? "up" : c === "down" ? "down" : "tx-hi") + ")" : good === true ? "var(--up)" : good === false ? "var(--down)" : "var(--tx-hi)";
  return (
    <div className="mcell">
      <span className="mk">{k}</span>
      <span className="mv" style={{ color, fontSize: big ? 26 : undefined }}>
        {v}
      </span>
      {s && <span className="ms">{s}</span>}
    </div>
  );
}

function BenchRow({ bench: b }: { bench: Backtest["bench"] }) {
  if (!b.valid) {
    return (
      <Panel title="Benchmark Comparison" sub="unsupported">
        <div className="empty">Benchmark unsupported: {b.reason}</div>
      </Panel>
    );
  }
  return (
    <Panel title="Benchmark Comparison" sub={`vs Buy & Hold${b.symbol ? ` (${b.symbol})` : ""}`} right={<span className="chip">alpha / beta / IR</span>}>
      <div className="bench-row" style={{ border: 0, borderRadius: 0 }}>
        <BCell k="Alpha" v={fmt.pct(b.alpha, 1)} good={b.alpha >= 0} />
        <BCell k="Beta" v={b.beta.toFixed(2)} neutral s="market sensitivity" />
        <BCell k="Information Ratio" v={b.infoRatio.toFixed(2)} good={b.infoRatio > 0} />
        <BCell k="vs Buy & Hold" v={fmt.pct(b.vsBH, 1)} good={b.vsBH > 0} s={"B&H " + fmt.pct(b.bhReturn, 1)} />
      </div>
    </Panel>
  );
}
function BCell({ k, v, good, neutral, s }: any) {
  const color = neutral ? "var(--tx-hi)" : good ? "var(--up)" : "var(--down)";
  return (
    <div className="mcell">
      <span className="mk">{k}</span>
      <span className="mv" style={{ color, fontSize: 18 }}>
        {v}
      </span>
      {s && <span className="ms">{s}</span>}
    </div>
  );
}

function EquityReport({ btCurve, markers, disclosure }: { btCurve: { strat: BTPoint[]; bench: BTPoint[] }; markers: BTMarker[]; disclosure: string }) {
  const [bw, setBw] = useState<[number, number]>([0, 1]);
  if (btCurve.strat.length === 0) {
    return (
      <Panel title="Equity Curve" sub="unavailable" bodyClass="pad">
        <div className="empty">Invalid or missing strategy equity curve.</div>
      </Panel>
    );
  }
  return (
    <Panel
      title="Equity Curve vs Benchmark"
      sub={disclosure}
      right={
        <div className="legend">
          <span className="li">
            <span className="sw" style={{ background: "var(--up)" }} />
            Strategy
          </span>
          {btCurve.bench.length > 0 && (
            <span className="li">
              <span className="sw" style={{ background: "var(--tx-lo)" }} />
              Buy & Hold
            </span>
          )}
          {markers.length > 0 && (
            <>
              <span className="li">
                <span style={{ width: 0, height: 0, borderLeft: "4px solid transparent", borderRight: "4px solid transparent", borderBottom: "7px solid var(--up)" }} />
                Entry
              </span>
              <span className="li">
                <span style={{ width: 6, height: 6, border: "1.4px solid var(--down)" }} />
                Exit
              </span>
            </>
          )}
        </div>
      }
      bodyClass="pad"
    >
      <BTChart strat={btCurve.strat} bench={btCurve.bench} markers={markers} brushStart={bw[0]} brushEnd={bw[1]} />
      <BrushBar value={bw} onChange={setBw} strat={btCurve.strat} />
    </Panel>
  );
}

function BrushBar({ value, onChange, strat }: { value: [number, number]; onChange: (v: [number, number]) => void; strat: BTPoint[] }) {
  const ref = useRef<HTMLDivElement>(null);
  const drag = (which: number) => (e: React.MouseEvent) => {
    e.preventDefault();
    const move = (ev: MouseEvent) => {
      const r = ref.current!.getBoundingClientRect();
      let p = Math.max(0, Math.min(1, (ev.clientX - r.left) / r.width));
      onChange(which === 0 ? [Math.min(p, value[1] - 0.05), value[1]] : [value[0], Math.max(p, value[0] + 0.05)]);
    };
    const up = () => {
      window.removeEventListener("mousemove", move);
      window.removeEventListener("mouseup", up);
    };
    window.addEventListener("mousemove", move);
    window.addEventListener("mouseup", up);
  };
  const path = useMemo(() => {
    if (strat.length < 2) return "";
    let min = Infinity,
      max = -Infinity;
    for (const point of strat) {
      min = Math.min(min, point.v);
      max = Math.max(max, point.v);
    }
    const yRange = normalizedRange(min, max);
    const firstTs = strat[0].tsMs;
    const timeRange = strat[strat.length - 1].tsMs - firstTs || 1;
    return strat.map((d, i) => (i ? "L" : "M") + (((d.tsMs - firstTs) / timeRange) * 100).toFixed(2) + " " + (28 - normalizedPosition(d.v, yRange) * 26).toFixed(2)).join(" ");
  }, [strat]);
  return (
    <div className="brush" ref={ref}>
      <svg width="100%" height="32" viewBox="0 0 100 32" preserveAspectRatio="none" style={{ position: "absolute", inset: 0 }}>
        <path d={path} fill="none" stroke="var(--tx-faint)" strokeWidth="0.8" />
      </svg>
      <div className="window" style={{ left: value[0] * 100 + "%", width: (value[1] - value[0]) * 100 + "%" }}>
        <div onMouseDown={drag(0)} style={{ position: "absolute", left: -5, top: 0, bottom: 0, width: 10, cursor: "ew-resize" }} />
        <div onMouseDown={drag(1)} style={{ position: "absolute", right: -5, top: 0, bottom: 0, width: 10, cursor: "ew-resize" }} />
      </div>
    </div>
  );
}

/* ----- Trade blotter ----- */
function Blotter({ blotter }: { blotter: BlotterRow[] }) {
  const [fSide, setFSide] = useState("all");
  const [fStrat, setFStrat] = useState("all");
  const strats = useMemo(() => ["all", ...Array.from(new Set(blotter.map((r) => r.strat)))], [blotter]);
  const rows = blotter.filter((r) => (fSide === "all" || r.side === fSide) && (fStrat === "all" || r.strat === fStrat));
  const { sorted, onSort, arrow } = useSort(rows, "tsMs", "desc");
  return (
    <Panel
      title="Execution Fill Legs"
      sub={rows.length + " reported physical fill legs"}
      right={
        <div className="filterbar">
          {["all", "buy", "sell"].map((s) => (
            <button key={s} className={"fbtn " + (fSide === s ? "on" : "")} onClick={() => setFSide(s)}>
              {s === "all" ? "All" : s[0].toUpperCase() + s.slice(1)}
            </button>
          ))}
          <select className="fbtn" value={fStrat} onChange={(e) => setFStrat(e.target.value)} style={{ marginLeft: 4 }}>
            {strats.map((s) => (
              <option key={s} value={s}>
                {s === "all" ? "All strats" : s}
              </option>
            ))}
          </select>
        </div>
      }
    >
      <table className="dt compact">
        <thead>
          <tr>
            <th className="l sortable" onClick={() => onSort("tsMs")}>
              Time (UTC){arrow("tsMs")}
            </th>
            <th className="l sortable" onClick={() => onSort("orderId")}>
              #{arrow("orderId")}
            </th>
            <th className="l">Symbol</th>
            <th className="l">Side</th>
            <th className="sortable" onClick={() => onSort("qty")}>
              Qty{arrow("qty")}
            </th>
            <th className="sortable" onClick={() => onSort("fill")}>
              Fill{arrow("fill")}
            </th>
            <th>Intended</th>
            <th className="sortable" onClick={() => onSort("slipBps")}>
              Slip(bps){arrow("slipBps")}
            </th>
            <th className="sortable" onClick={() => onSort("comm")}>
              Comm{arrow("comm")}
            </th>
            <th className="sortable" onClick={() => onSort("pnl")}>
              P&L{arrow("pnl")}
            </th>
            <th className="l">Strategy</th>
          </tr>
        </thead>
        <tbody>
          {sorted.slice(0, 40).map((r) => (
            <tr key={r.id}>
              <td className="l" style={{ color: "var(--tx-faint)", whiteSpace: "nowrap" }}>
                {new Date(r.tsMs).toISOString()}
              </td>
              <td className="l" style={{ color: "var(--tx-faint)" }}>
                {r.orderId}
              </td>
              <td className="l">{r.sym}</td>
              <td className="l">
                <Side side={r.side} />
              </td>
              <td>{fmt.num(r.qty, r.qty < 10 ? 3 : 1)}</td>
              <td className="hi">{fmt.num(r.fill, r.fill > 1000 ? 1 : 2)}</td>
              <td style={{ color: "var(--tx-faint)" }}>
                {r.intendedValid ? fmt.num(r.intended, r.intended > 1000 ? 1 : 2) : "unsupported"}
              </td>
              <td className={r.slippageValid && Math.abs(r.slipBps) > 2 ? (r.slipBps > 0 ? "down" : "up") : ""}>
                {r.slippageValid ? fmt.sign(r.slipBps, 1) : "unsupported"}
              </td>
              <td style={{ color: "var(--tx-lo)" }}>
                {fmt.num(r.comm, 2)}{r.commCurrency ? ` ${r.commCurrency}` : ""}
              </td>
              <td className={cls(r.pnl)}>{fmt.signUsd(r.pnl)}</td>
              <td className="l" style={{ color: "var(--tx-mid)", fontSize: 11 }}>
                {r.strat}
              </td>
            </tr>
          ))}
        </tbody>
      </table>
    </Panel>
  );
}

/* ----- breakdown small-multiples ----- */
function BreakdownGrid({ title, rows, isSym }: { title: string; rows: BreakdownRow[]; isSym?: boolean }) {
  const max = Math.max(...rows.map((r) => Math.abs(r.pnl)), 1);
  const visibleRows = rows.slice(0, REPORT_LIMITS.visibleBreakdownRows);
  return (
    <Panel title={title} sub={`${visibleRows.length} of ${rows.length}${isSym ? " symbols" : " strategies"}`} bodyClass="pad">
      <div style={{ display: "flex", flexDirection: "column", gap: 9 }}>
        {visibleRows.map((r) => (
          <div key={r.key} style={{ display: "flex", flexDirection: "column", gap: 6, padding: "8px 10px", background: "var(--bg-2)", border: "1px solid var(--line-soft)", borderRadius: "var(--r)" }}>
            <div style={{ display: "flex", alignItems: "center", justifyContent: "space-between" }}>
              {isSym ? <Sym s={r.key} /> : <span style={{ fontWeight: 600, fontSize: 12 }}>{r.key}</span>}
              <span className={"num " + cls(r.pnl)} style={{ fontSize: 14, fontWeight: 500 }}>
                {fmt.signUsd(r.pnl, 0)}
              </span>
            </div>
            <div className="track" style={{ height: 5 }}>
              <div className="fill" style={{ width: ((Math.abs(r.pnl) / max) * 100).toFixed(0) + "%", background: r.pnl >= 0 ? "var(--up)" : "var(--down)" }} />
            </div>
            <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr 1fr", gap: 8 }}>
              <KV k="Win" v={fmt.pctP(r.win, 1)} />
              <KV k="PF" v={r.pf === null ? (r.pfUnbounded ? "unbounded" : "unsupported") : r.pf.toFixed(2)} c={r.pf === null ? undefined : r.pf >= 1 ? "up" : "down"} />
              <KV k="Trades" v={r.trades} />
            </div>
          </div>
        ))}
      </div>
    </Panel>
  );
}
function KV({ k, v, c }: any) {
  return (
    <div style={{ display: "flex", flexDirection: "column", gap: 1 }}>
      <span className="lbl" style={{ fontSize: 9 }}>
        {k}
      </span>
      <span className={"num " + (c || "")} style={{ fontSize: 12, color: c ? undefined : "var(--tx-mid)" }}>
        {v}
      </span>
    </div>
  );
}

export function BacktestReview({ report }: { report: ReportData }) {
  const bt = report.backtest;
  const losers = report.blotter.reduce((count, row) => count + (row.pnl < 0 ? 1 : 0), 0);
  const winners = report.blotter.reduce((count, row) => count + (row.pnl > 0 ? 1 : 0), 0);
  return (
    <div className="report">
      <div className="report-head">
        <div>
          <div className="title">{bt.name}</div>
          <div className="meta" style={{ marginTop: 4 }}>
            <span>
              <b>Range</b> {bt.range}
            </span>
            <span>
              <b>Bars</b> {bt.bars}
            </span>
            <span>
              <b>Universe</b> {bt.universe}
            </span>
            <span>
              <b>Trades</b> {bt.headline.totalTrades.toLocaleString()}
            </span>
          </div>
        </div>
        <span className="badge badge-backtest">
          <span className="g" />
          BACKTEST · COMPLETE
        </span>
      </div>
      {!bt.headline.valuationComplete && (
        <div
          role="alert"
          style={{
            marginBottom: 12,
            padding: "10px 12px",
            border: "1px solid var(--down)",
            color: "var(--down)",
            background: "color-mix(in srgb, var(--down) 8%, transparent)",
          }}
        >
          VALUATION UNSUPPORTED: {bt.headline.valuationReason || "open position has no accepted market mark"}.
          Final equity and derived return metrics are provisional.
        </div>
      )}
      {!bt.headline.portfolioTimeSeriesValid && (
        <div
          role="alert"
          style={{
            marginBottom: 12,
            padding: "10px 12px",
            border: "1px solid var(--down)",
            color: "var(--down)",
            background: "color-mix(in srgb, var(--down) 8%, transparent)",
          }}
        >
          PORTFOLIO TIME SERIES UNSUPPORTED: {bt.headline.portfolioTimeSeriesReason || "causal cross-symbol mark order is unavailable"}.
          Path-dependent annualized return and drawdown metrics are invalid.
        </div>
      )}
      <MetricBoard headline={bt.headline} />
      <BenchRow bench={bt.bench} />
      <EquityReport btCurve={report.btCurve} markers={report.btMarkers} disclosure={report.curveDisclosure} />
      <div className="two-col">
        <Blotter blotter={report.blotter} />
        <Panel title="P&L Distribution" sub="reported physical fill legs" bodyClass="pad">
          <Histogram bins={report.hist} />
          <div style={{ display: "flex", justifyContent: "space-between", marginTop: 8, fontSize: 10.5, color: "var(--tx-lo)" }}>
            <span>
              Negative legs <b className="down">{losers}</b>
            </span>
            <span>
              Positive legs <b className="up">{winners}</b>
            </span>
          </div>
        </Panel>
      </div>
      <div className="two-col" style={{ gridTemplateColumns: "1fr 1fr" }}>
        <BreakdownGrid title="Per-Symbol Breakdown" rows={report.bySymbol} isSym />
        <BreakdownGrid title="Per-Strategy Breakdown" rows={report.byStrategy} />
      </div>
    </div>
  );
}
