/* =========================================================================
   TrueTest — Live Dashboard cockpit.
   Ported from the Claude Design prototype (live.jsx). useFeed currently drives
   a light in-page simulation; when the WS feed lands it is the single swap
   point (replace the setInterval generators with the /stream subscription).
   ========================================================================= */
import { useState, useEffect, useRef, useMemo } from "react";
import { fmt, cls } from "./format";
import { Panel, TickNum, Delta, Spark, Gauge, Side, Sym, useSort } from "./components";
import { TT } from "./data";

/* ----- light simulated feed ----- */
export function useFeed(active: boolean) {
  const [marks, setMarks] = useState<Record<string, number>>({ BTCUSDT: 71248.5, ETHUSDT: 3842.18, SOLUSDT: 177.94 });
  const [equity, setEquity] = useState(TT.account.equity);
  const [fills, setFills] = useState<any[]>(TT.fills);
  const [book, setBook] = useState(TT.book);
  const [eps, setEps] = useState(TT.health.eventsPerSec);
  const tickRef = useRef(0);
  useEffect(() => {
    if (!active) return;
    const id = setInterval(() => {
      tickRef.current++;
      setMarks((m) => ({
        BTCUSDT: +(m.BTCUSDT + TT.gauss() * 9).toFixed(1),
        ETHUSDT: +(m.ETHUSDT + TT.gauss() * 0.8).toFixed(2),
        SOLUSDT: +(m.SOLUSDT + TT.gauss() * 0.06).toFixed(2),
      }));
      setEquity((e) => +(e + TT.gauss() * 380 + 30).toFixed(2));
      setEps(() => Math.round(14820 + TT.gauss() * 900));
      setBook(() => TT.buildBook(71248.5 + TT.gauss() * 4, 0.5));
      if (tickRef.current % 2 === 0) {
        const symK = ["BTCUSDT", "ETHUSDT", "SOLUSDT"][Math.floor(TT.rnd() * 3)];
        const base = ({ BTCUSDT: 71248, ETHUSDT: 3842, SOLUSDT: 177.9 } as any)[symK];
        const side = TT.rnd() > 0.5 ? "buy" : "sell";
        const qty = symK === "BTCUSDT" ? TT.rnd() * 0.5 + 0.02 : symK === "ETHUSDT" ? TT.rnd() * 8 + 0.5 : TT.rnd() * 90 + 5;
        const px = base * (1 + TT.gauss() * 0.0005);
        const nf = { id: Date.now(), t: null, sym: symK, side, qty, px, fee: px * qty * 0.00018, src: TT.rnd() > 0.28 ? "exchange" : "simulated", isNew: true };
        setFills((f) => [nf, ...f].slice(0, 40));
      }
    }, 1700);
    return () => clearInterval(id);
  }, [active]);
  return { marks, equity, fills, book, eps };
}

/* ----- Account strip ----- */
function genSpark(n: number, bias: number) {
  const a = [];
  let v = 0;
  for (let i = 0; i < n; i++) {
    v += TT.gauss() + bias;
    a.push(v);
  }
  return a;
}
export function AccountStrip({ equity, marks }: any) {
  const sparks = useMemo(
    () => ({
      eq: genSpark(48, 0.22),
      cash: genSpark(40, -0.1),
      real: genSpark(40, 0.18),
      unreal: genSpark(40, -0.05),
      day: genSpark(40, 0.12),
    }),
    [],
  );
  const upnl = TT.positions.reduce((a, p) => {
    const dir = p.side === "long" ? 1 : -1;
    return a + (marks[p.sym] - p.entry) * p.qty * dir;
  }, 0);
  const a = TT.account;
  return (
    <div className="acct-grid statrow">
      <div className="stat hero">
        <div className="top">
          <span className="lbl">Account Equity</span>
          <Delta value={a.equityDelta} pct={a.equityPct / 100} />
        </div>
        <div style={{ display: "flex", alignItems: "flex-end", justifyContent: "space-between", gap: 12 }}>
          <TickNum value={equity} fmt={(v: number) => fmt.usd(v)} className="val" />
          <Spark data={sparks.eq} w={88} h={28} stroke="var(--up)" fill strokeW={1.5} />
        </div>
      </div>
      <Stat label="Cash" value={a.cash} delta={a.cashDelta} money spark={sparks.cash} />
      <Stat label="Realized P&L" value={a.realized} delta={a.realizedDelta} money colored spark={sparks.real} signed />
      <StatLive label="Unrealized P&L" value={upnl} delta={a.unrealizedDelta} spark={sparks.unreal} />
      <Stat label="Day P&L" value={a.equityDelta} delta={a.equityPct} pct money colored spark={sparks.day} signed />
    </div>
  );
}
function Stat({ label, value, delta, money, pct, colored, signed, spark }: any) {
  return (
    <div className="stat">
      <div className="top">
        <span className="lbl">{label}</span>
        <Spark data={spark} w={58} h={18} stroke={spark[spark.length - 1] >= spark[0] ? "var(--up)" : "var(--down)"} />
      </div>
      <div style={{ display: "flex", alignItems: "baseline", gap: 7, flexWrap: "wrap" }}>
        <span className={"val md num " + (colored ? cls(value) : "")}>{signed ? fmt.signUsd(value) : money ? fmt.usd(value) : fmt.num(value)}</span>
        <Delta value={delta} pct={pct ? delta / 100 : null} money={money && !pct} />
      </div>
    </div>
  );
}
function StatLive({ label, value, delta, spark }: any) {
  return (
    <div className="stat">
      <div className="top">
        <span className="lbl">{label}</span>
        <Spark data={spark} w={58} h={18} />
      </div>
      <div style={{ display: "flex", alignItems: "baseline", gap: 7, flexWrap: "wrap" }}>
        <TickNum value={value} fmt={(v: number) => fmt.signUsd(v)} className="val md" colored />
        <Delta value={delta} money />
      </div>
    </div>
  );
}

/* ----- Positions ----- */
export function PositionsPanel({ marks }: any) {
  const rows = TT.positions.map((p) => {
    const dir = p.side === "long" ? 1 : -1;
    const mark = marks[p.sym];
    const upnl = (mark - p.entry) * p.qty * dir;
    return { ...p, mark, upnl, pct: ((mark - p.entry) / p.entry) * dir };
  });
  const { sorted, onSort, arrow } = useSort(rows, "upnl");
  return (
    <Panel title="Positions" sub={rows.length + " open"} right={<span className="chip">Net {fmt.usdK(rows.reduce((a, r) => a + r.upnl, 0))}</span>}>
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
              <td className={cls(r.upnl)}>
                {fmt.signUsd(r.upnl)} <span style={{ color: "var(--tx-faint)", fontSize: 10 }}>{fmt.pct(r.pct)}</span>
              </td>
            </tr>
          ))}
        </tbody>
      </table>
    </Panel>
  );
}

/* ----- Open lots / brackets ----- */
function Bracket({ lot, marks }: any) {
  const mark = marks[lot.sym];
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
export function LotsPanel({ marks }: any) {
  return (
    <Panel title="Open Lots" sub={TT.lots.length + " · brackets"} right={<span className="chip">SL↔TP</span>}>
      <div>
        {TT.lots.map((l) => (
          <Bracket key={l.id} lot={l} marks={marks} />
        ))}
      </div>
    </Panel>
  );
}

/* ----- Order book ladder ----- */
export function OrderBook({ book }: any) {
  const maxCum = Math.max(book.bids[book.bids.length - 1].cum, book.asks[book.asks.length - 1].cum);
  const imb = book.bidVol / (book.bidVol + book.askVol);
  const spreadBps = (book.spread / book.mid) * 1e4;
  return (
    <Panel title="Order Book" sub="BTCUSDT · L2" right={<span className="chip acc">Depth 10</span>}>
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
        {[...book.asks].reverse().map((a: any, i: number) => (
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
        {book.bids.map((b: any, i: number) => (
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
export function FillsTape({ fills }: any) {
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
        {fills.map((f: any, i: number) => (
          <div className={"f" + (f.isNew && i === 0 ? " new" : "")} key={f.id}>
            <span className="t">{f.t != null ? TT.fmtClock(f.t) : "now"}</span>
            <span style={{ fontFamily: "var(--sans)", fontWeight: 600, fontSize: 11 }}>{f.sym.replace("USDT", "")}</span>
            <span className={"s " + (f.side === "buy" ? "up" : "down")}>{f.side === "buy" ? "BUY" : "SELL"}</span>
            <span className="num" style={{ textAlign: "right" }}>
              {fmt.num(f.qty, f.qty < 10 ? 3 : 1)}
            </span>
            <span className="num" style={{ textAlign: "right", color: "var(--tx-hi)" }}>
              {fmt.num(f.px, f.px > 1000 ? 1 : 2)}
            </span>
            <span className="src">{f.src === "exchange" ? "EXC" : "SIM"}</span>
          </div>
        ))}
      </div>
    </Panel>
  );
}

/* ----- Strategy cards ----- */
export function StrategyPanel() {
  const { sorted, onSort, key } = useSort(TT.strategies, "pnl");
  return (
    <Panel
      title="Strategies"
      sub={TT.strategies.length + " active"}
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
            <Spark data={s.spark} w={150} h={22} stroke={s.pnl >= 0 ? "var(--up)" : "var(--down)"} fill />
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
export function RiskPanel({ halted }: any) {
  return (
    <Panel
      title="Risk Limits"
      sub="real-time"
      right={
        halted ? (
          <span className="chip" style={{ background: "var(--down-dim)", color: "var(--down)", borderColor: "var(--down)" }}>
            BREACH
          </span>
        ) : (
          <span className="chip" style={{ background: "var(--up-dim)", color: "var(--up)" }}>
            WITHIN
          </span>
        )
      }
      bodyClass="pad"
    >
      <div style={{ display: "flex", flexDirection: "column", gap: 13 }}>
        {TT.risk.map((r) => (
          <Gauge key={r.name} {...r} used={halted && r.name === "Daily loss" ? 24100 : r.used} />
        ))}
      </div>
    </Panel>
  );
}

/* ----- System health ----- */
export function HealthStrip({ eps }: any) {
  const h = TT.health;
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
          <span className="hv up">{h.ringDrops}</span>
          <span className="hsub">buffer 0.4%</span>
        </div>
        <div className="hcell">
          <span className="hk">Provider</span>
          <span className="hv up" style={{ fontSize: 12 }}>
            ● {h.provider}
          </span>
          <span className="hsub">binance-ws</span>
        </div>
        <div className="hcell">
          <span className="hk">QuestDB</span>
          <span className="hv" style={{ fontSize: 11, color: "var(--accent-hi)" }}>
            ● {h.questdb}
          </span>
          <span className="hsub">{eps.toLocaleString()} ev/s</span>
        </div>
      </div>
    </Panel>
  );
}
