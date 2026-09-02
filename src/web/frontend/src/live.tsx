/* =========================================================================
   TrueTest — Live Dashboard cockpit panels.
   Render is unchanged from the Claude Design prototype; the data source is now
   the adapted live feed (LiveData slices passed as props) instead of a mock
   global. Positions/lots use the engine's authoritative mark/upnl directly.
   ========================================================================= */
import { useMemo } from "react";
import { fmt, cls, fmtClock } from "./format";
import { Panel, TickNum, Delta, Spark, Gauge, Side, Sym, useSort } from "./components";
import type { Account, Position, Lot, Book, Fill, Strategy, RiskLimit, Health, EquityPoint } from "./types";

/* ----- Account strip ----- */
export function AccountStrip({ account, equityCurve }: { account: Account; equityCurve: EquityPoint[] }) {
  const heroSpark = useMemo(
    () => (equityCurve.length > 1 ? equityCurve.map((p) => p.eq) : []),
    [equityCurve],
  );
  const a = account;
  return (
    <div className="acct-grid statrow">
      <div className="stat hero">
        <div className="top">
          <span className="lbl">Account Equity</span>
          <Delta value={a.equityDelta} pct={a.equityPct === null ? null : a.equityPct / 100} />
        </div>
        <div style={{ display: "flex", alignItems: "flex-end", justifyContent: "space-between", gap: 12 }}>
          <TickNum value={a.equity} fmt={(v: number) => fmt.usd(v)} className="val" />
          {heroSpark.length > 1 ? <Spark data={heroSpark} w={88} h={28} stroke="var(--up)" fill strokeW={1.5} /> : <span className="num">—</span>}
        </div>
      </div>
      <Stat label="Cash" value={a.cash} delta={a.cashDelta} money />
      <Stat label="Realized P&L" value={a.realized} delta={a.realizedDelta} money colored signed />
      <StatLive label="Unrealized P&L" value={a.unrealized} delta={a.unrealizedDelta} />
      <Stat label="Day P&L" value={a.equityDelta} delta={a.equityPct} pct money colored signed />
    </div>
  );
}
function Stat({ label, value, delta, money, pct, colored, signed }: any) {
  const usable = typeof value === "number" && Number.isFinite(value);
  return (
    <div className="stat">
      <div className="top">
        <span className="lbl">{label}</span>
        <span className="num">—</span>
      </div>
      <div style={{ display: "flex", alignItems: "baseline", gap: 7, flexWrap: "wrap" }}>
        <span className={"val md num " + (colored && usable ? cls(value) : "")}>
          {usable ? (signed ? fmt.signUsd(value) : money ? fmt.usd(value) : fmt.num(value)) : "—"}
        </span>
        <Delta value={delta} pct={pct ? delta / 100 : null} money={money && !pct} />
      </div>
    </div>
  );
}
function StatLive({ label, value, delta }: any) {
  return (
    <div className="stat">
      <div className="top">
        <span className="lbl">{label}</span>
        <span className="num">—</span>
      </div>
      <div style={{ display: "flex", alignItems: "baseline", gap: 7, flexWrap: "wrap" }}>
        <TickNum value={value} fmt={(v: number) => fmt.signUsd(v)} className="val md" colored />
        <Delta value={delta} money />
      </div>
    </div>
  );
}

/* ----- Positions ----- */
export function PositionsPanel({ positions }: { positions: Position[] }) {
  const rows = positions.map((p) => ({
    ...p,
    pct:
      p.mark !== null && Number.isFinite(p.entry) && p.entry !== 0
        ? ((p.mark - p.entry) / p.entry) * (p.side === "long" ? 1 : -1)
        : null,
  }));
  const { sorted, onSort, arrow } = useSort(rows, "upnl");
  const allPnlsAvailable = rows.every((r) => r.upnl !== null);
  const netPnl = allPnlsAvailable
    ? rows.reduce((sum, row) => sum + (row.upnl ?? 0), 0)
    : null;
  return (
    <Panel title="Positions" sub={rows.length + " open"} right={<span className="chip">Net {netPnl === null ? "N/A" : fmt.usdK(netPnl)}</span>}>
      <table className="dt">
        <thead>
          <tr>
            <th className="l sortable" onClick={() => onSort("sym")}>
              Symbol{arrow("sym")}
            </th>
            <th className="sortable" onClick={() => onSort("qty")}>
              Qty{arrow("qty")}
            </th>
            <th className="sortable" onClick={() => onSort("entry")}>
              Avg Entry{arrow("entry")}
            </th>
            <th className="sortable" onClick={() => onSort("mark")}>
              Mark{arrow("mark")}
            </th>
            <th className="sortable" onClick={() => onSort("upnl")}>
              Unreal. P&L{arrow("upnl")}
            </th>
          </tr>
        </thead>
        <tbody>
          {sorted.map((r) => (
            <tr key={r.sym}>
              <td className="l">
                <Sym s={r.sym} />
              </td>
              <td>
                <Side side={r.side}>{fmt.num(r.qty, r.qty < 100 ? 3 : 1)}</Side>
              </td>
              <td>{fmt.num(r.entry, 2)}</td>
              <td className="hi">
                <TickNum value={r.mark} fmt={(v: number) => fmt.num(v, v > 1000 ? 1 : 2)} />
              </td>
              <td className={r.upnl === null ? "" : cls(r.upnl)}>
                {r.upnl === null ? "—" : fmt.signUsd(r.upnl)}{" "}
                <span style={{ color: "var(--tx-faint)", fontSize: 10 }}>{r.pct === null ? "—" : fmt.pct(r.pct)}</span>
              </td>
            </tr>
          ))}
        </tbody>
      </table>
    </Panel>
  );
}

/* ----- Open lots / brackets ----- */
function Bracket({ lot }: { lot: Lot }) {
  const mark = lot.mark;
  const long = lot.side === "long";
  const lo = Math.min(lot.sl, lot.tp),
    hi = Math.max(lot.sl, lot.tp);
  const PAD = 9;
  const pos = (v: number) => PAD + Math.max(0, Math.min(1, (v - lo) / (hi - lo))) * (100 - 2 * PAD);
  const dir = long ? 1 : -1;
  const upnl = (mark - lot.entry) * dir;
  const rail = long
    ? "linear-gradient(90deg, var(--down-dim), var(--bg-3) 50%, var(--up-dim))"
    : "linear-gradient(90deg, var(--up-dim), var(--bg-3) 50%, var(--down-dim))";
  return (
    <div className="bracket">
      <div className="bh">
        <span className="strat">{lot.strat}</span>
        <Side side={lot.side} />
        <Sym s={lot.sym} />
        <span className="meta">{lot.mgr === "venue" ? <span className="tag exch">VENUE</span> : <span className="tag sim">ENGINE</span>}</span>
      </div>
      <div className="band">
        <div className="rail" style={{ background: rail }} />
        <div className="tick" style={{ left: pos(lot.sl) + "%" }}>
          <span className="pin" style={{ background: "var(--down)" }} />
          <span className="cap" style={{ color: "var(--down)" }}>
            SL {fmt.num(lot.sl, lot.sl > 1000 ? 0 : 2)}
          </span>
        </div>
        <div className="tick" style={{ left: pos(lot.entry) + "%" }}>
          <span className="pin" style={{ background: "var(--tx-mid)" }} />
          <span className="cap" style={{ top: -16 }}>
            {fmt.num(lot.entry, lot.entry > 1000 ? 0 : 2)}
          </span>
        </div>
        <div className="tick" style={{ left: pos(lot.tp) + "%" }}>
          <span className="pin" style={{ background: "var(--up)" }} />
          <span className="cap" style={{ color: "var(--up)" }}>
            TP {fmt.num(lot.tp, lot.tp > 1000 ? 0 : 2)}
          </span>
        </div>
        <div className="mark" style={{ left: pos(mark) + "%", background: upnl >= 0 ? "var(--up)" : "var(--down)" }} title={"Mark " + mark} />
      </div>
    </div>
  );
}
export function LotsPanel({ lots }: { lots: Lot[] }) {
  return (
    <Panel title="Open Lots" sub={lots.length + " · brackets"} right={<span className="chip">SL↔TP</span>}>
      <div>
        {lots.map((l) => (
          <Bracket key={l.id} lot={l} />
        ))}
      </div>
    </Panel>
  );
}

/* ----- Order book ladder ----- */
export function OrderBook({ book }: { book: Book }) {
  if (!book.bids.length || !book.asks.length) {
    return (
      <Panel title="Order Book" sub="L2" right={<span className="chip acc">Depth 10</span>}>
        <div className="placeholder" style={{ minHeight: 160 }}>
          <div className="pd">No depth for this session.</div>
        </div>
      </Panel>
    );
  }
  const maxCum = Math.max(book.bids[book.bids.length - 1].cum, book.asks[book.asks.length - 1].cum) || 1;
  const imb = book.bidVol / (book.bidVol + book.askVol || 1);
  const spreadBps = (book.spread / book.mid) * 1e4;
  return (
    <Panel title="Order Book" sub="L2" right={<span className="chip acc">Depth 10</span>}>
      <div className="imbal">
        <span className="lbl">Imbalance</span>
        <div className="bar">
          <div className="b" style={{ width: (imb * 100).toFixed(0) + "%" }} />
        </div>
        <span className="num" style={{ fontSize: 11, color: imb > 0.5 ? "var(--up)" : "var(--down)" }}>
          {(imb * 100).toFixed(0)}/{(100 - imb * 100).toFixed(0)}
        </span>
      </div>
      <div className="ladder">
        {[...book.asks].reverse().map((a, i) => (
          <div className="row ask" key={"a" + i}>
            <div className="depth" style={{ width: ((a.cum / maxCum) * 100).toFixed(0) + "%" }} />
            <span className="px num">{fmt.num(a.px, 1)}</span>
            <span className="sz num">{a.sz.toFixed(2)}</span>
            <span className="cum num">{a.cum.toFixed(1)}</span>
          </div>
        ))}
        <div className="mid">
          <span className="price num up">{fmt.num(book.mid, 1)}</span>
          <span className="spread">
            spread {fmt.num(book.spread, 1)} · {spreadBps.toFixed(2)} bps
          </span>
        </div>
        {book.bids.map((b, i) => (
          <div className="row bid" key={"b" + i}>
            <div className="depth" style={{ width: ((b.cum / maxCum) * 100).toFixed(0) + "%" }} />
            <span className="px num">{fmt.num(b.px, 1)}</span>
            <span className="sz num">{b.sz.toFixed(2)}</span>
            <span className="cum num">{b.cum.toFixed(1)}</span>
          </div>
        ))}
      </div>
    </Panel>
  );
}

/* ----- Fills tape ----- */
export function FillsTape({ fills }: { fills: Fill[] }) {
  return (
    <Panel title="Recent Fills" sub="live tape" right={<span className="chip">{fills.length}</span>}>
      <div className="tape">
        <div className="f" style={{ borderBottom: "1px solid var(--line)", position: "sticky", top: 0, background: "var(--bg-1)" }}>
          <span className="lbl">Time</span>
          <span className="lbl">Sym</span>
          <span className="lbl">Side</span>
          <span className="lbl" style={{ textAlign: "right" }}>
            Qty
          </span>
          <span className="lbl" style={{ textAlign: "right" }}>
            Price
          </span>
          <span className="lbl" style={{ textAlign: "right" }}>
            Src
          </span>
        </div>
        {fills.map((f) => (
          <div className="f" key={f.id}>
            <span className="t">{f.t != null ? fmtClock(f.t) : "now"}</span>
            <span style={{ fontFamily: "var(--sans)", fontWeight: 600, fontSize: 11 }}>{f.sym.replace("USDT", "")}</span>
            <span className={"s " + (f.side === "buy" ? "up" : "down")}>{f.side === "buy" ? "BUY" : "SELL"}</span>
            <span className="num" style={{ textAlign: "right" }}>
              {fmt.num(f.qty, f.qty < 10 ? 3 : 1)}
            </span>
            <span className="num" style={{ textAlign: "right", color: "var(--tx-hi)" }}>
              {fmt.num(f.px, f.px > 1000 ? 1 : 2)}
            </span>
            <span className="src">{f.src === "exchange" ? "EXC" : f.src === "simulated" ? "SIM" : "UNK"}</span>
          </div>
        ))}
      </div>
    </Panel>
  );
}

/* ----- Strategy cards ----- */
export function StrategyPanel({ strategies }: { strategies: Strategy[] }) {
  const { sorted, onSort, key } = useSort(strategies, "pnl");
  return (
    <Panel
      title="Strategies"
      sub={strategies.length + " active"}
      right={
        <select
          className="chip"
          value={key}
          onChange={(e) => onSort(e.target.value)}
          style={{ background: "var(--bg-3)", color: "var(--tx-mid)", border: "1px solid var(--line-soft)", borderRadius: 4, fontFamily: "var(--sans)" }}
        >
          <option value="pnl">P&L</option>
          <option value="win">Win %</option>
          <option value="pf">PF</option>
          <option value="trades">Trades</option>
        </select>
      }
      bodyClass="pad"
    >
      <div className="scards">
        {sorted.map((s) => (
          <div className="scard" key={s.name}>
            <div className="sh">
              <span className="sn">{s.name}</span>
              <span className={"pnl " + cls(s.pnl)}>{fmt.signUsd(s.pnl, 0)}</span>
            </div>
            {s.spark.length > 1 ? (
              <Spark data={s.spark} w={150} h={22} stroke={s.pnl >= 0 ? "var(--up)" : "var(--down)"} fill />
            ) : (
              <div className="num" style={{ height: 22 }}>history unavailable</div>
            )}
            <div className="grid">
              <div className="kv">
                <span className="k">Win</span>
                <span className="v">{(s.win * 100).toFixed(1)}%</span>
              </div>
              <div className="kv">
                <span className="k">PF</span>
                <span className={"v " + (s.pf >= 1 ? "up" : "down")}>{s.pf.toFixed(2)}</span>
              </div>
              <div className="kv">
                <span className="k">Trades</span>
                <span className="v">{s.trades}</span>
              </div>
              <div className="kv">
                <span className="k">Lots</span>
                <span className="v">{s.lots}</span>
              </div>
            </div>
          </div>
        ))}
      </div>
    </Panel>
  );
}

/* ----- Risk panel ----- */
export function RiskPanel({ risk, halted }: { risk: RiskLimit[]; halted: boolean }) {
  const complete = risk.every(
    (r) =>
      r.used !== null &&
      Number.isFinite(r.used) &&
      Number.isFinite(r.limit) &&
      r.limit > 0,
  );
  return (
    <Panel
      title="Risk Limits"
      sub="real-time"
      right={
        halted ? (
          <span className="chip" style={{ background: "var(--down-dim)", color: "var(--down)", borderColor: "var(--down)" }}>
            BREACH
          </span>
        ) : complete ? (
          <span className="chip" style={{ background: "var(--up-dim)", color: "var(--up)" }}>
            WITHIN
          </span>
        ) : (
          <span className="chip" style={{ background: "var(--bg-3)", color: "var(--tx-mid)" }}>
            UNKNOWN
          </span>
        )
      }
      bodyClass="pad"
    >
      <div style={{ display: "flex", flexDirection: "column", gap: 13 }}>
        {risk.map((r) => (
          <Gauge key={r.name} {...r} />
        ))}
      </div>
    </Panel>
  );
}

/* ----- System health ----- */
export function HealthStrip({ health }: { health: Health }) {
  const h = health;
  return (
    <Panel title="System Health" sub="ops" right={<span className="num" style={{ fontSize: 10.5, color: "var(--tx-lo)" }}>uptime {h.uptime}</span>}>
      <div className="health">
        <div className="hcell">
          <span className="hk">Tick→Trade</span>
          <span className="hv">{h.latAvg}µs</span>
          <span className="hsub">
            min {h.latMin} · max {h.latMax}
          </span>
        </div>
        <div className="hcell">
          <span className="hk">Ring Drops</span>
          <span className={"hv " + (h.ringDrops > 0 ? "warn" : "up")}>{h.ringDrops}</span>
          <span className="hsub">lock-free SPSC</span>
        </div>
        <div className="hcell">
          <span className="hk">Provider</span>
          <span className={"hv " + (h.provider === "OK" ? "up" : "down")} style={{ fontSize: 12 }}>
            ● {h.provider}
          </span>
          <span className="hsub">feed</span>
        </div>
        <div className="hcell">
          <span className="hk">QuestDB</span>
          <span className="hv" style={{ fontSize: 11, color: h.questdb === "PERSISTING" ? "var(--accent-hi)" : "var(--tx-lo)" }}>
            ● {h.questdb}
          </span>
          <span className="hsub">{h.eventsPerSec.toLocaleString()} ev/s</span>
        </div>
      </div>
    </Panel>
  );
}
