/* =========================================================================
   TrueTest — app shell, mode switch, state machine.
   Ported from the Claude Design prototype (app.jsx).
   ========================================================================= */
import { useState, useRef } from "react";
import { fmt } from "./format";
import { Panel, TickNum, Spark, Delta } from "./components";
import { EquityChart } from "./charts";
import { useFeed, AccountStrip, PositionsPanel, LotsPanel, OrderBook, FillsTape, StrategyPanel, RiskPanel, HealthStrip } from "./live";
import { BacktestReview } from "./backtest";
import { TT } from "./data";
import type { ConnStatus, Mode } from "./types";

/* ---- Equity panel (live) ---- */
function EquityPanel({ equity }: any) {
  const first = TT.equityCurve[0].eq;
  const up = equity >= first;
  return (
    <Panel
      title="Equity & Drawdown"
      sub="streaming"
      right={
        <>
          <TickNum value={equity} fmt={(v: number) => fmt.usd(v)} className="hi" />
          <span className="chip" style={{ background: up ? "var(--up-dim)" : "var(--down-dim)", color: up ? "var(--up)" : "var(--down)" }}>
            {fmt.pct((equity - first) / first)}
          </span>
        </>
      }
      bodyClass="pad"
    >
      <EquityChart data={TT.equityCurve} />
    </Panel>
  );
}

/* ---- HALTED banner ---- */
function HaltBanner({ onResume }: any) {
  return (
    <div className="halt-banner">
      <div className="ico blink">!</div>
      <div>
        <div className="ht">RISK HALT · TRADING SUSPENDED</div>
        <div className="hd">Daily loss limit breached (−$24,100 / −$25,000). All new orders blocked; open brackets held. Manual review required to resume.</div>
      </div>
      <button
        className="resume"
        onClick={onResume}
        style={{ fontFamily: "var(--sans)", fontSize: 11.5, fontWeight: 600, color: "var(--tx-hi)", background: "var(--bg-2)", border: "1px solid var(--line-strong)", borderRadius: 5, padding: "7px 14px", cursor: "pointer" }}
      >
        Acknowledge & Review
      </button>
    </div>
  );
}

/* ---- skeleton ---- */
function Sk({ w, h = 12, style }: any) {
  return <div className="sk" style={{ width: w, height: h, ...style }} />;
}
function SkPanel({ h = 200, lines = 5 }: any) {
  return (
    <div className="panel skeleton-panel" style={{ height: h }}>
      <div className="panel-h">
        <Sk w={90} h={9} />
      </div>
      <div className="panel-b pad" style={{ display: "flex", flexDirection: "column", gap: 10 }}>
        {Array.from({ length: lines }).map((_, i) => (
          <div key={i} style={{ display: "flex", justifyContent: "space-between", gap: 10 }}>
            <Sk w={"40%"} h={9} />
            <Sk w={"22%"} h={9} />
          </div>
        ))}
      </div>
    </div>
  );
}
function SkeletonGrid() {
  return (
    <div className="live-grid">
      <div className="acct statrow" style={{ gridTemplateColumns: "1.4fr 1fr 1fr 1fr 1fr", display: "grid" }}>
        {Array.from({ length: 5 }).map((_, i) => (
          <div className="stat" key={i}>
            <Sk w={70} h={9} />
            <Sk w={i === 0 ? 150 : 90} h={i === 0 ? 26 : 16} style={{ marginTop: 6 }} />
          </div>
        ))}
      </div>
      <div className="col c1">
        <SkPanel h={250} lines={3} />
        <SkPanel h={160} />
        <SkPanel h={180} />
      </div>
      <div className="col c2">
        <SkPanel h={300} lines={9} />
        <SkPanel h={180} />
      </div>
      <div className="col c3">
        <SkPanel h={180} />
        <SkPanel h={200} />
        <SkPanel h={110} />
      </div>
    </div>
  );
}

/* ---- empty / disconnected placeholders ---- */
function EmptyState() {
  return (
    <div style={{ padding: 9 }}>
      <div className="panel" style={{ minHeight: "calc(100vh - 140px)" }}>
        <div className="placeholder">
          <div className="pico" style={{ color: "var(--tx-lo)" }}>
            ◴
          </div>
          <div className="pt">No active session</div>
          <div className="pd">There is no running shadow or live session attached to this workspace. Start an engine session or load a feed to populate the cockpit.</div>
          <div style={{ display: "flex", gap: 8, marginTop: 4 }}>
            <span className="chip acc" style={{ padding: "6px 12px" }}>
              Attach session
            </span>
            <span className="chip" style={{ padding: "6px 12px" }}>
              Load JSON feed
            </span>
          </div>
        </div>
      </div>
    </div>
  );
}
function DisconnectedState() {
  return (
    <div style={{ padding: 9 }}>
      <div className="panel" style={{ minHeight: "calc(100vh - 140px)" }}>
        <div className="placeholder">
          <div className="pico" style={{ color: "var(--warn)", borderColor: "var(--warn-dim)" }}>
            ⚠
          </div>
          <div className="pt" style={{ color: "var(--warn)" }}>
            Feed disconnected
          </div>
          <div className="pd">Lost connection to the engine event stream. Last data 3s ago. Holding last known state — values may be stale.</div>
          <div className="reconnbar">
            <div className="b" />
          </div>
          <div className="lbl" style={{ color: "var(--warn)" }}>
            Reconnecting · attempt 2
          </div>
        </div>
      </div>
    </div>
  );
}

/* ---- Live dashboard composition ---- */
function LiveDashboard({ status, onResume }: { status: ConnStatus; onResume: () => void }) {
  const feed = useFeed(status === "live");
  if (status === "loading") return <SkeletonGrid />;
  if (status === "empty") return <EmptyState />;
  if (status === "disconnected") return <DisconnectedState />;
  const halted = status === "halted";
  return (
    <div className="live-grid">
      {halted && <HaltBanner onResume={onResume} />}
      <div className="acct">
        <AccountStrip equity={feed.equity} marks={feed.marks} />
      </div>
      <div className="col c1">
        <EquityPanel equity={feed.equity} />
        <PositionsPanel marks={feed.marks} />
        <LotsPanel marks={feed.marks} />
      </div>
      <div className="col c2">
        <OrderBook book={feed.book} />
        <FillsTape fills={feed.fills} />
      </div>
      <div className="col c3">
        <RiskPanel halted={halted} />
        <StrategyPanel />
        <HealthStrip eps={feed.eps} />
      </div>
    </div>
  );
}

/* ---- header bits ---- */
function ConnBadge({ status }: { status: ConnStatus }) {
  const map: Record<string, { c: string; t: string; pulse?: boolean }> = {
    live: { c: "conn", t: "Connected", pulse: true },
    halted: { c: "conn halt", t: "Halted" },
    disconnected: { c: "conn reconn", t: "Reconnecting" },
    loading: { c: "conn reconn", t: "Connecting" },
    empty: { c: "conn off", t: "No session" },
  };
  const m = map[status] || map.live;
  return (
    <span className={"badge " + m.c}>
      <span className={"g " + (m.pulse ? "pulse" : "")} />
      {m.t}
    </span>
  );
}

export default function App() {
  const [mode, setMode] = useState<Mode>("live"); // live | backtest
  const [status, setStatus] = useState<ConnStatus>("live"); // live | loading | empty | disconnected | halted
  const feedEps = useRef(14820);

  const states: { k: ConnStatus; l: string; halt?: boolean }[] = [
    { k: "live", l: "Running" },
    { k: "loading", l: "Loading" },
    { k: "empty", l: "Empty" },
    { k: "disconnected", l: "Disconnected" },
    { k: "halted", l: "HALTED", halt: true },
  ];

  return (
    <div className="app">
      <div className="header">
        <div className="brand">
          <div className="mark">T</div>
          <div className="name">
            True<b>Test</b>
          </div>
        </div>

        {mode === "live" ? (
          <>
            <div className="hdr-seg">
              <div className="hdr-meta">
                <span className="k">Session</span>
                <span className="v mono" style={{ fontSize: 12, color: "var(--tx-mid)" }}>
                  TT-SHADOW-0432
                </span>
              </div>
              <span className="badge badge-shadow">
                <span className="g" />
                SHADOW
              </span>
            </div>
            <div className="hdr-seg">
              <ConnBadge status={status} />
            </div>
            <div className="hdr-seg" style={{ borderRight: "1px solid var(--line-soft)" }}>
              <div className="hdr-meta">
                <span className="k">Events / sec</span>
                <span className="v num" style={{ fontSize: 12 }}>
                  {status === "live" ? "14,820" : status === "empty" ? "—" : "0"}
                </span>
              </div>
              <div className="hdr-meta">
                <span className="k">Uptime</span>
                <span className="v num" style={{ fontSize: 12 }}>
                  06:42:18
                </span>
              </div>
              <div className="hdr-meta">
                <span className="k">Symbols</span>
                <span className="v mono" style={{ fontSize: 12, color: "var(--tx-mid)" }}>
                  BTC · ETH · SOL
                </span>
              </div>
            </div>
          </>
        ) : (
          <div className="hdr-seg">
            <div className="hdr-meta">
              <span className="k">Report</span>
              <span className="v" style={{ fontSize: 12, color: "var(--tx-mid)" }}>
                MOM-XBT + MR-ETH ensemble
              </span>
            </div>
            <span className="badge badge-backtest">
              <span className="g" />
              BACKTEST
            </span>
          </div>
        )}

        <div className="spacer" />

        {mode === "live" && (
          <div className="staterail">
            <span className="sl">State</span>
            {states.map((s) => (
              <button key={s.k} className={(status === s.k ? "on " : "") + (s.halt ? "halt" : "")} onClick={() => setStatus(s.k)}>
                {s.l}
              </button>
            ))}
          </div>
        )}

        <div className="modeswitch">
          <button className={mode === "live" ? "on" : ""} onClick={() => setMode("live")}>
            <span className="dot" />
            Live Dashboard
          </button>
          <button className={mode === "backtest" ? "on" : ""} onClick={() => setMode("backtest")}>
            <span className="dot" />
            Backtest Review
          </button>
        </div>
      </div>

      <div className="content">{mode === "live" ? <LiveDashboard status={status} onResume={() => setStatus("live")} /> : <BacktestReview />}</div>
    </div>
  );
}
