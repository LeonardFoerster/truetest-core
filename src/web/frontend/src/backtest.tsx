/* =========================================================================
   TrueTest — Backtest Review report.
   Ported from the Claude Design prototype (backtest.jsx).
   ========================================================================= */
import { useState, useRef, useMemo } from "react";
import { fmt, cls } from "./format";
import { Panel, Sym, Side, useSort } from "./components";
import { BTChart, Histogram } from "./charts";
import { TT } from "./data";

function MetricBoard() {
  const h = TT.backtest.headline;
  return (
    <>
      <div className="metric-board">
        <div className="mcell mb-hero">
          <span className="mk">Total Return</span>
          <span className="mv up">{fmt.pct(h.totalReturn, 1)}</span>
          <span className="ms">
            {fmt.usd(h.startEquity, 0)} → {fmt.usd(h.finalEquity, 0)} · CAGR {fmt.pctP(h.cagr, 1)}
          </span>
        </div>
        <MCell k="Sharpe" v={h.sharpe.toFixed(2)} good={h.sharpe > 1} s="risk-adjusted" big />
        <MCell k="Sortino" v={h.sortino.toFixed(2)} good={h.sortino > 1} s="downside" big />
        <MCell k="Max Drawdown" v={fmt.pct(h.maxDD, 2)} cls="down" s="peak-to-trough" />
        <MCell k="Calmar" v={h.calmar.toFixed(2)} good={h.calmar > 1} s="ret / maxDD" />
      </div>
      <div className="metric-board" style={{ gridTemplateColumns: "repeat(4, 1fr)" }}>
        <MCell k="Win Rate" v={fmt.pctP(h.winRate, 1)} s={Math.round(h.totalTrades * h.winRate) + " / " + h.totalTrades} />
        <MCell k="Profit Factor" v={h.profitFactor.toFixed(2)} good={h.profitFactor > 1} s="gross win / loss" />
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

function BenchRow() {
  const b = TT.backtest.bench;
  return (
    <Panel title="Benchmark Comparison" sub="vs Buy & Hold" right={<span className="chip">BTC-spot 60/40 basket</span>}>
      <div className="bench-row" style={{ border: 0, borderRadius: 0 }}>
        <BCell k="Alpha" v={fmt.pct(b.alpha, 1)} good />
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

function EquityReport() {
  const [bw, setBw] = useState<[number, number]>([0, 1]);
  return (
    <Panel
      title="Equity Curve vs Benchmark"
      sub="2025 · 1m bars"
      right={
        <div className="legend">
          <span className="li">
            <span className="sw" style={{ background: "var(--up)" }} />
            Strategy
          </span>
          <span className="li">
            <span className="sw" style={{ background: "var(--tx-lo)" }} />
            Buy & Hold
          </span>
          <span className="li">
            <span style={{ width: 0, height: 0, borderLeft: "4px solid transparent", borderRight: "4px solid transparent", borderBottom: "7px solid var(--up)" }} />
            Entry
          </span>
          <span className="li">
            <span style={{ width: 6, height: 6, border: "1.4px solid var(--down)" }} />
            Exit
          </span>
        </div>
      }
      bodyClass="pad"
    >
      <BTChart strat={TT.btCurve.strat} bench={TT.btCurve.bench} markers={TT.btMarkers} brushStart={bw[0]} brushEnd={bw[1]} />
      <BrushBar value={bw} onChange={setBw} />
    </Panel>
  );
}

function BrushBar({ value, onChange }: any) {
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
  // mini overview path
  const path = useMemo(() => {
    const arr = TT.btCurve.strat,
      min = Math.min(...arr.map((d) => d.v)),
      max = Math.max(...arr.map((d) => d.v));
    return arr.map((d, i) => (i ? "L" : "M") + ((i / (arr.length - 1)) * 100).toFixed(2) + " " + (28 - ((d.v - min) / (max - min)) * 26).toFixed(2)).join(" ");
  }, []);
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
function Blotter() {
  const [fSide, setFSide] = useState("all");
  const [fStrat, setFStrat] = useState("all");
  const rows = TT.blotter.filter((r) => (fSide === "all" || r.side === fSide) && (fStrat === "all" || r.strat === fStrat));
  const { sorted, onSort, arrow } = useSort(rows, "id", "desc");
  const strats = ["all", "MOM-XBT", "MR-ETH", "BASIS-SOL", "STAT-ARB"];
  return (
    <Panel
      title="Trade Blotter"
      sub={rows.length + " trades"}
      right={
        <div className="filterbar">
          {["all", "long", "short"].map((s) => (
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
            <th className="l sortable" onClick={() => onSort("id")}>
              #{arrow("id")}
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
              <td className="l" style={{ color: "var(--tx-faint)" }}>
                {r.id}
              </td>
              <td className="l">{r.sym.replace("USDT", "")}</td>
              <td className="l">
                <Side side={r.side} />
              </td>
              <td>{fmt.num(r.qty, r.qty < 10 ? 3 : 1)}</td>
              <td className="hi">{fmt.num(r.fill, r.fill > 1000 ? 1 : 2)}</td>
              <td style={{ color: "var(--tx-faint)" }}>{fmt.num(r.intended, r.intended > 1000 ? 1 : 2)}</td>
              <td className={Math.abs(r.slipBps) > 2 ? (r.slipBps > 0 ? "down" : "up") : ""}>{fmt.sign(r.slipBps, 1)}</td>
              <td style={{ color: "var(--tx-lo)" }}>{fmt.usd(r.comm, 2)}</td>
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
function BreakdownGrid({ title, rows, isSym }: any) {
  const max = Math.max(...rows.map((r: any) => Math.abs(r.pnl)));
  return (
    <Panel title={title} sub={rows.length + (isSym ? " symbols" : " strategies")} bodyClass="pad">
      <div style={{ display: "flex", flexDirection: "column", gap: 9 }}>
        {rows.map((r: any) => (
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
              <KV k="PF" v={r.pf.toFixed(2)} c={r.pf >= 1 ? "up" : "down"} />
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

export function BacktestReview() {
  const bt = TT.backtest;
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
      <MetricBoard />
      <BenchRow />
      <EquityReport />
      <div className="two-col">
        <Blotter />
        <Panel title="P&L Distribution" sub="per trade" bodyClass="pad">
          <Histogram bins={TT.hist} />
          <div style={{ display: "flex", justifyContent: "space-between", marginTop: 8, fontSize: 10.5, color: "var(--tx-lo)" }}>
            <span>
              Losers <b className="down">{TT.hist.filter((b) => b.x1 <= 0).reduce((a, b) => a + b.c, 0)}</b>
            </span>
            <span>
              Winners <b className="up">{TT.hist.filter((b) => b.x0 >= 0).reduce((a, b) => a + b.c, 0)}</b>
            </span>
          </div>
        </Panel>
      </div>
      <div className="two-col" style={{ gridTemplateColumns: "1fr 1fr" }}>
        <BreakdownGrid title="Per-Symbol Breakdown" rows={TT.bySymbol} isSym />
        <BreakdownGrid title="Per-Strategy Breakdown" rows={TT.byStrategy} />
      </div>
    </div>
  );
}
