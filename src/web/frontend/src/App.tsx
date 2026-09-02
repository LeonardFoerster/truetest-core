/* =========================================================================
   TrueTest — app shell, mode switch, live/report wiring.
   Render unchanged from the Claude Design prototype; data now flows from the
   live WS feed (useLiveFeed) and REST results (useResults), with offline
   fixtures as fallback. The header "State" rail is a demo override on top of
   the real connection status.
   ========================================================================= */
import { useState } from "react";
import { fmt } from "./format";
import { Panel, TickNum } from "./components";
import { EquityChart } from "./charts";
import { AccountStrip, PositionsPanel, LotsPanel, OrderBook, FillsTape, StrategyPanel, RiskPanel, HealthStrip } from "./live";
import { BacktestReview } from "./backtest";
import { useLiveFeed } from "./data/useLiveFeed";
import { useResults } from "./data/useResults";
import type { LiveData } from "./adapters/snapshot";
import type { ConnStatus, Mode, EquityPoint } from "./types";

/* ---- Equity panel (live) ---- */
function EquityPanel({ equityCurve, equity }: { equityCurve: EquityPoint[]; equity: number | null }) {
  const effectiveEquity = equity ?? (equityCurve.length ? equityCurve[equityCurve.length - 1].eq : null);
  const first = equityCurve.length ? equityCurve[0].eq : null;
  const up = effectiveEquity !== null && first !== null ? effectiveEquity >= first : null;
  const change = effectiveEquity !== null && first !== null && first !== 0 ? (effectiveEquity - first) / first : null;
  return (
    <Panel
      title="Equity & Drawdown"
      sub="streaming"
      right={
        <>
          <TickNum value={equity} fmt={(v: number) => fmt.usd(v)} className="hi" />
          <span
            className="chip"
            style={{
              background: up === null ? "var(--bg-3)" : up ? "var(--up-dim)" : "var(--down-dim)",
              color: up === null ? "var(--tx-mid)" : up ? "var(--up)" : "var(--down)",
            }}
          >
            {change === null ? "N/A" : fmt.pct(change)}
          </span>
        </>
      }
      bodyClass="pad"
    >
      {equityCurve.length > 1 ? <EquityChart data={equityCurve} /> : <div className="placeholder"><div className="pd">Equity history unavailable.</div></div>}
    </Panel>
  );
}

/* ---- HALTED banner ---- */
function HaltBanner({ onResume }: { onResume: () => void }) {
  return (
    <div className="halt-banner">
      <div className="ico blink">!</div>
      <div>
        <div className="ht">RISK HALT · TRADING SUSPENDED</div>
        <div className="hd">Daily loss limit breached. All new orders blocked; open brackets held. Manual review required to resume.</div>
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
          <div className="pd">There is no running shadow or live session attached to this workspace. Start an engine session with --web to populate the cockpit.</div>
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
          <div className="pd">Lost connection to the engine event stream. Holding last known state — values may be stale.</div>
          <div className="reconnbar">
            <div className="b" />
          </div>
          <div className="lbl" style={{ color: "var(--warn)" }}>
            Reconnecting…
          </div>
        </div>
      </div>
    </div>
  );
}
function DegradedState() {
  return (
    <div style={{ padding: 9 }}>
      <div className="panel" style={{ minHeight: "calc(100vh - 140px)" }}>
        <div className="placeholder">
          <div className="pico" style={{ color: "var(--warn)", borderColor: "var(--warn-dim)" }}>⚠</div>
          <div className="pt" style={{ color: "var(--warn)" }}>Feed stale</div>
          <div className="pd">The stream is connected but no fresh, valid engine snapshot has arrived. Values are hidden until freshness is restored.</div>
        </div>
      </div>
    </div>
  );
}

/* ---- Live dashboard composition ---- */
function LiveDashboard({ status, live, onResume }: { status: ConnStatus; live: LiveData | null; onResume: () => void }) {
  if (status === "empty") return <EmptyState />;
  if (status === "disconnected") return <DisconnectedState />;
  if (status === "degraded") return <DegradedState />;
  if (status === "loading" || !live) return <SkeletonGrid />;
  const halted = status === "halted";
  return (
    <div className="live-grid">
      {halted && <HaltBanner onResume={onResume} />}
      <div className="acct">
        <AccountStrip account={live.account} equityCurve={live.equityCurve} />
      </div>
      <div className="col c1">
        <EquityPanel equityCurve={live.equityCurve} equity={live.account.equity} />
        <PositionsPanel positions={live.positions} />
        <LotsPanel lots={live.lots} />
      </div>
      <div className="col c2">
        <OrderBook book={live.book} />
        <FillsTape fills={live.fills} />
      </div>
      <div className="col c3">
        <RiskPanel risk={live.risk} halted={halted} />
        <StrategyPanel strategies={live.strategies} />
        <HealthStrip health={live.health} />
      </div>
    </div>
  );
}

/* ---- header bits ---- */
function ConnBadge({ status, offline }: { status: ConnStatus; offline: boolean }) {
  if (offline) return <span className="badge conn off"><span className="g" />Demo data</span>;
  const map: Record<string, { c: string; t: string; pulse?: boolean }> = {
    live: { c: "conn", t: "Connected", pulse: true },
    halted: { c: "conn halt", t: "Halted" },
    disconnected: { c: "conn reconn", t: "Reconnecting" },
    degraded: { c: "conn reconn", t: "Stale feed" },
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
  const [mode, setMode] = useState<Mode>("live");
  const live = useLiveFeed();
  const results = useResults();
  const [override, setOverride] = useState<ConnStatus | null>(null);

  // Real status from the feed; data.halted promotes to the alarm state.
  const base: ConnStatus =
    live.data?.halted ? "halted" : live.status === "live" ? "live" : live.status === "degraded" ? "degraded" : live.status === "disconnected" ? "disconnected" : "loading";
  const status: ConnStatus = override ?? base;

  const symbols = live.data ? Array.from(new Set(live.data.positions.map((p) => p.sym.replace("USDT", "")))) : [];
  const eps = live.data ? Math.round(live.data.health.eventsPerSec) : 0;

  const states: { k: ConnStatus; l: string; halt?: boolean }[] = [
    { k: "live", l: "Running" },
    { k: "loading", l: "Loading" },
    { k: "empty", l: "Empty" },
    { k: "disconnected", l: "Disconnected" },
    { k: "degraded", l: "Stale" },
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
                  {live.offline ? "OFFLINE-FIXTURE" : "TT-SESSION"}
                </span>
              </div>
              <span className="badge badge-shadow">
                <span className="g" />
                SHADOW
              </span>
            </div>
            <div className="hdr-seg">
              <ConnBadge status={status} offline={live.offline} />
            </div>
            <div className="hdr-seg" style={{ borderRight: "1px solid var(--line-soft)" }}>
              <div className="hdr-meta">
                <span className="k">Events / sec</span>
                <span className="v num" style={{ fontSize: 12 }}>
                  {status === "live" ? eps.toLocaleString() : status === "empty" ? "—" : "0"}
                </span>
              </div>
              <div className="hdr-meta">
                <span className="k">Symbols</span>
                <span className="v mono" style={{ fontSize: 12, color: "var(--tx-mid)" }}>
                  {symbols.length ? symbols.join(" · ") : "—"}
                </span>
              </div>
            </div>
          </>
        ) : (
          <div className="hdr-seg">
            <div className="hdr-meta">
              <span className="k">Report</span>
              <span className="v" style={{ fontSize: 12, color: "var(--tx-mid)" }}>
                {results.report?.backtest.name ?? "Backtest"}
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
              <button
                key={s.k}
                className={(status === s.k ? "on " : "") + (s.halt ? "halt" : "")}
                onClick={() => setOverride(s.k === "live" ? null : s.k)}
              >
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

      <div className="content">
        {mode === "live" ? (
          <LiveDashboard status={status} live={live.data} onResume={() => setOverride(null)} />
        ) : results.report ? (
          <BacktestReview report={results.report} />
        ) : (
          <SkeletonGrid />
        )}
      </div>
    </div>
  );
}
