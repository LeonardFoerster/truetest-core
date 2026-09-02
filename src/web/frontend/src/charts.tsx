/* =========================================================================
   TrueTest — chart primitives (SVG, data-driven).
   Ported from the Claude Design prototype (charts.jsx).
   ========================================================================= */
import { fmt } from "./format";
import { histogramCountScale, normalizedPosition, normalizedRange } from "./chartMath";

/* ---------- Live equity + drawdown chart ---------- */
export function EquityChart({ data, w = 720, h = 230 }: any) {
  const padL = 8,
    padR = 56,
    padT = 12,
    padB = 40;
  const iw = w - padL - padR,
    ih = h - padT - padB;
  const ddH = 36;
  const eqH = ih - ddH - 8;

  const eqs = data.map((d: any) => d.eq);
  const min = Math.min(...eqs),
    max = Math.max(...eqs);
  const yRange = normalizedRange(min, max);
  const sx = (i: number) => padL + (i / (data.length - 1)) * iw;
  const sy = (v: number) => padT + (1 - normalizedPosition(v, yRange)) * eqH;

  let line = "",
    area = "";
  data.forEach((d: any, i: number) => {
    const x = sx(i).toFixed(1),
      y = sy(d.eq).toFixed(1);
    line += (i ? "L" : "M") + x + " " + y + " ";
  });
  area = line + `L${sx(data.length - 1).toFixed(1)} ${padT + eqH} L${padL} ${padT + eqH} Z`;

  // drawdown sub
  const ddTop = padT + eqH + 8;
  const minDD = Math.min(...data.map((d: any) => d.dd), -0.001);
  const ddY = (v: number) => ddTop + (v / minDD) * ddH;
  let ddArea = `M${padL} ${ddTop} `;
  data.forEach((d: any, i: number) => {
    ddArea += "L" + sx(i).toFixed(1) + " " + ddY(d.dd).toFixed(1) + " ";
  });
  ddArea += `L${sx(data.length - 1).toFixed(1)} ${ddTop} Z`;

  const last = data[data.length - 1];
  const up = last.eq >= data[0].eq;
  const col = up ? "var(--up)" : "var(--down)";

  // y gridlines
  const grid = [0, 0.5, 1].map((t) => ({
    y: padT + t * eqH,
    v: max * (1 - t) + min * t,
  }));

  return (
    <svg width="100%" viewBox={`0 0 ${w} ${h}`} preserveAspectRatio="none" style={{ display: "block" }}>
      <defs>
        <linearGradient id="eqfill" x1="0" y1="0" x2="0" y2="1">
          <stop offset="0" stopColor={col} stopOpacity="0.18" />
          <stop offset="1" stopColor={col} stopOpacity="0" />
        </linearGradient>
      </defs>
      {grid.map((g, i) => (
        <g key={i}>
          <line x1={padL} x2={padL + iw} y1={g.y} y2={g.y} stroke="var(--line-soft)" strokeWidth="1" strokeDasharray="2 4" />
          <text x={w - padR + 6} y={g.y + 3} fontSize="9.5" fill="var(--tx-faint)" fontFamily="var(--mono)">
            {fmt.usdK(g.v)}
          </text>
        </g>
      ))}
      <path d={area} fill="url(#eqfill)" />
      <path d={line} fill="none" stroke={col} strokeWidth="1.75" strokeLinejoin="round" />
      {/* last marker */}
      <circle cx={sx(data.length - 1)} cy={sy(last.eq)} r="2.6" fill={col} />
      <line x1={sx(data.length - 1)} x2={w - padR} y1={sy(last.eq)} y2={sy(last.eq)} stroke={col} strokeWidth="1" strokeDasharray="2 3" opacity="0.5" />
      {/* drawdown */}
      <text x={padL} y={ddTop - 2} fontSize="8.5" fill="var(--tx-faint)" fontFamily="var(--sans)" letterSpacing="0.05em">
        DRAWDOWN
      </text>
      <path d={ddArea} fill="var(--down-dim)" stroke="var(--down)" strokeWidth="0.75" strokeOpacity="0.6" />
      <text x={w - padR + 6} y={ddTop + ddH + 3} fontSize="9.5" fill="var(--tx-faint)" fontFamily="var(--mono)">
        {(minDD * 100).toFixed(1)}%
      </text>
    </svg>
  );
}

/* ---------- Backtest: strategy vs benchmark + drawdown + markers ---------- */
export function BTChart({ strat, bench, markers, w = 1140, h = 320, brushStart = 0, brushEnd = 1 }: any) {
  const padL = 8,
    padR = 62,
    padT = 14,
    padB = 16;
  const iw = w - padL - padR;
  const ddH = 64,
    gap = 10;
  const eqH = h - padT - padB - ddH - gap;

  if (!Array.isArray(strat) || strat.length === 0) {
    return (
      <svg width="100%" viewBox={`0 0 ${w} ${h}`} role="img" aria-label="Equity curve unavailable">
        <text x={w / 2} y={h / 2} textAnchor="middle" fill="var(--tx-faint)">
          Equity curve unavailable
        </text>
      </svg>
    );
  }

  let min = Infinity,
    max = -Infinity,
    minTs = Infinity,
    maxTs = -Infinity;
  for (const point of strat) {
    min = Math.min(min, point.v);
    max = Math.max(max, point.v);
    minTs = Math.min(minTs, point.tsMs);
    maxTs = Math.max(maxTs, point.tsMs);
  }
  for (const point of bench) {
    min = Math.min(min, point.v);
    max = Math.max(max, point.v);
    minTs = Math.min(minTs, point.tsMs);
    maxTs = Math.max(maxTs, point.tsMs);
  }
  const yRange = normalizedRange(min, max);
  const n = strat.length;
  const timeRange = maxTs - minTs || 1;
  const sxTime = (tsMs: number) => padL + ((tsMs - minTs) / timeRange) * iw;
  const sy = (v: number) => padT + (1 - normalizedPosition(v, yRange)) * eqH;

  const path = (arr: any[]) => arr.map((d, i) => (i ? "L" : "M") + sxTime(d.tsMs).toFixed(1) + " " + sy(d.v).toFixed(1)).join(" ");
  const stratP = path(strat),
    benchP = path(bench);

  const ddTop = padT + eqH + gap + 18;
  let minDD = -0.001;
  for (const point of strat)
    minDD = Math.min(minDD, point.dd ?? 0);
  const ddY = (v: number) => ddTop + (v / minDD) * (ddH - 18);
  let ddArea = `M${sxTime(strat[0].tsMs).toFixed(1)} ${ddTop} ` + strat.map((d: any) => "L" + sxTime(d.tsMs).toFixed(1) + " " + ddY(d.dd).toFixed(1)).join(" ") + ` L${sxTime(strat[n - 1].tsMs).toFixed(1)} ${ddTop} Z`;

  const grid = [0, 0.25, 0.5, 0.75, 1].map((t) => ({
    y: padT + t * eqH,
    v: max * (1 - t) + min * t,
  }));
  const bw = brushStart * iw + padL,
    bwEnd = brushEnd * iw + padL;

  return (
    <svg width="100%" viewBox={`0 0 ${w} ${h}`} preserveAspectRatio="none" style={{ display: "block" }}>
      <defs>
        <linearGradient id="btfill" x1="0" y1="0" x2="0" y2="1">
          <stop offset="0" stopColor="var(--up)" stopOpacity="0.13" />
          <stop offset="1" stopColor="var(--up)" stopOpacity="0" />
        </linearGradient>
      </defs>
      {/* brush dim outside window */}
      <rect x={padL} y={padT} width={bw - padL} height={eqH} fill="var(--bg-inset)" opacity="0.55" />
      <rect x={bwEnd} y={padT} width={padL + iw - bwEnd} height={eqH} fill="var(--bg-inset)" opacity="0.55" />
      {grid.map((g, i) => (
        <g key={i}>
          <line x1={padL} x2={padL + iw} y1={g.y} y2={g.y} stroke="var(--line-soft)" strokeWidth="1" strokeDasharray="2 4" />
          <text x={w - padR + 6} y={g.y + 3} fontSize="10" fill="var(--tx-faint)" fontFamily="var(--mono)">
            {fmt.usdK(g.v)}
          </text>
        </g>
      ))}
      <path d={stratP + ` L${sxTime(strat[n - 1].tsMs).toFixed(1)} ${padT + eqH} L${sxTime(strat[0].tsMs).toFixed(1)} ${padT + eqH} Z`} fill="url(#btfill)" />
      <path d={benchP} fill="none" stroke="var(--tx-lo)" strokeWidth="1.4" strokeDasharray="4 3" opacity="0.85" />
      <path d={stratP} fill="none" stroke="var(--up)" strokeWidth="2" strokeLinejoin="round" />
      {markers.map((m: any, i: number) => {
        const x = sxTime(m.tsMs),
          y = sy(m.v);
        const c = m.side === "long" ? "var(--up)" : "var(--down)";
        return m.kind === "entry" ? (
          <path key={i} d={`M${x} ${y - 7} l4 7 l-8 0 Z`} fill={c} opacity="0.9" />
        ) : (
          <rect key={i} x={x - 3} y={y - 3} width="6" height="6" fill="none" stroke={c} strokeWidth="1.4" opacity="0.9" />
        );
      })}
      {/* drawdown subchart */}
      <text x={padL} y={ddTop - 6} fontSize="9" fill="var(--tx-faint)" fontFamily="var(--sans)" letterSpacing="0.06em">
        DRAWDOWN
      </text>
      <path d={ddArea} fill="var(--down-dim)" stroke="var(--down)" strokeWidth="1" strokeOpacity="0.7" />
      <text x={w - padR + 6} y={ddTop + (ddH - 18) + 3} fontSize="10" fill="var(--tx-faint)" fontFamily="var(--mono)">
        {(minDD * 100).toFixed(1)}%
      </text>
    </svg>
  );
}

/* ---------- P&L distribution histogram ---------- */
export function Histogram({ bins, w = 300, h = 200 }: any) {
  const padL = 6,
    padR = 6,
    padT = 10,
    padB = 22;
  const iw = w - padL - padR,
    ih = h - padT - padB;
  const countScale = histogramCountScale(bins);
  const bw = bins.length > 0 ? iw / bins.length : iw;
  return (
    <svg width="100%" viewBox={`0 0 ${w} ${h}`} style={{ display: "block" }}>
      <line x1={padL} x2={w - padR} y1={padT + ih} y2={padT + ih} stroke="var(--line)" strokeWidth="1" />
      {bins.map((b: any, i: number) => {
        const bh = (b.c / countScale) * ih;
        const x = padL + i * bw;
        const mid = (b.x0 + b.x1) / 2;
        const zero = b.x0 === 0 && b.x1 === 0;
        const col = zero ? "var(--tx-faint)" : mid > 0 ? "var(--up)" : "var(--down)";
        return <rect key={i} x={x + 1} y={padT + ih - bh} width={bw - 2} height={bh} fill={col} opacity={zero ? 0.5 : mid > 0 ? 0.7 : 0.6} rx="1" />;
      })}
      {/* zero line */}
      {(() => {
        const zi = bins.findIndex((b: any) => b.x0 <= 0 && b.x1 > 0);
        if (zi < 0 || bins[zi].x1 <= bins[zi].x0) return null;
        const zx = padL + (zi + (0 - bins[zi].x0) / (bins[zi].x1 - bins[zi].x0)) * bw;
        return <line x1={zx} x2={zx} y1={padT} y2={padT + ih} stroke="var(--tx-faint)" strokeWidth="1" strokeDasharray="2 2" />;
      })()}
      <text x={padL} y={h - 6} fontSize="9" fill="var(--tx-faint)" fontFamily="var(--mono)">
        −$2.5K
      </text>
      <text x={w - padR} y={h - 6} fontSize="9" fill="var(--tx-faint)" fontFamily="var(--mono)" textAnchor="end">
        +$2.5K
      </text>
    </svg>
  );
}
