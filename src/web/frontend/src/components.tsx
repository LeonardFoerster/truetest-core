/* =========================================================================
   TrueTest — shared component atoms.
   Ported from the Claude Design prototype (components.jsx).
   ========================================================================= */
import { useState, useEffect, useRef, useMemo } from "react";
import { fmt, cls } from "./format";
import { symMeta } from "./symbols";

/* ---------- Panel ---------- */
export function Panel({ title, sub, right, children, className = "", bodyClass = "", style }: any) {
  return (
    <div className={"panel " + className} style={style}>
      <div className="panel-h">
        <span className="t">{title}</span>
        {sub && <span className="sub">{sub}</span>}
        {right && <span className="r">{right}</span>}
      </div>
      <div className={"panel-b " + bodyClass}>{children}</div>
    </div>
  );
}

/* ---------- Num with tick flash ---------- */
export function TickNum({ value, fmt: f = (v: number) => fmt.num(v), className = "", colored = false }: any) {
  const prev = useRef(value);
  const [flash, setFlash] = useState("");
  useEffect(() => {
    if (value > prev.current) setFlash("tick-up");
    else if (value < prev.current) setFlash("tick-down");
    prev.current = value;
    const t = setTimeout(() => setFlash(""), 560);
    return () => clearTimeout(t);
  }, [value]);
  const colorCls = colored ? cls(value) : "";
  return <span className={"num " + colorCls + " " + className + " " + flash} style={{ borderRadius: 3, padding: "0 2px" }}>{f(value)}</span>;
}

/* ---------- Delta chip ---------- */
export function Delta({ value, pct, money = false }: any) {
  const c = cls(value);
  const arrow = value >= 0 ? "▲" : "▼";
  return (
    <span className={"delta " + c}>
      <span style={{ fontSize: 8 }}>{arrow}</span>
      {money ? fmt.signUsd(value) : pct != null ? fmt.pct(pct) : fmt.sign(value)}
    </span>
  );
}

/* ---------- Sparkline ---------- */
export function Spark({ data, w = 64, h = 18, stroke, fill = false, strokeW = 1.25 }: any) {
  const { d, area, last } = useMemo(() => {
    const min = Math.min(...data),
      max = Math.max(...data);
    const rng = max - min || 1;
    const sx = (i: number) => (i / (data.length - 1)) * w;
    const sy = (v: number) => h - ((v - min) / rng) * (h - 2) - 1;
    let d = "",
      area = "";
    data.forEach((v: number, i: number) => {
      const x = sx(i).toFixed(1),
        y = sy(v).toFixed(1);
      d += (i ? "L" : "M") + x + " " + y + " ";
    });
    area = d + `L${w} ${h} L0 ${h} Z`;
    return { d, area, last: data[data.length - 1] >= data[0] };
  }, [data, w, h]);
  const col = stroke || (last ? "var(--up)" : "var(--down)");
  const gid = useMemo(() => "sg" + Math.random().toString(36).slice(2, 8), []);
  return (
    <svg width={w} height={h} style={{ display: "block", overflow: "visible" }}>
      {fill && (
        <defs>
          <linearGradient id={gid} x1="0" y1="0" x2="0" y2="1">
            <stop offset="0" stopColor={col} stopOpacity="0.22" />
            <stop offset="1" stopColor={col} stopOpacity="0" />
          </linearGradient>
        </defs>
      )}
      {fill && <path d={area} fill={`url(#${gid})`} />}
      <path d={d} fill="none" stroke={col} strokeWidth={strokeW} strokeLinejoin="round" strokeLinecap="round" />
    </svg>
  );
}

/* ---------- Gauge / limit bar ---------- */
export function Gauge({ name, used, limit, unit, inv }: any) {
  const pctUsed = Math.max(0, Math.min(1, used / limit));
  // color ramps amber->red as it approaches limit
  let color = "var(--up)";
  if (pctUsed >= 0.85) color = "var(--down)";
  else if (pctUsed >= 0.65) color = "var(--warn)";
  else if (inv) color = "var(--tx-mid)";
  else color = "var(--accent)";
  const fmtV = (v: number) => (unit === "$" ? fmt.usdK(v) : unit === "%" ? v.toFixed(2) + "%" : fmt.num(v, 0));
  return (
    <div className="gauge">
      <div className="gtop">
        <span className="gname">{name}</span>
        <span className="gval">
          {fmtV(used)} <span className="lim">/ {fmtV(limit)}</span>
        </span>
      </div>
      <div className="track">
        <div className="fill" style={{ width: (pctUsed * 100).toFixed(1) + "%", background: color }} />
      </div>
    </div>
  );
}

/* ---------- Side glyph ---------- */
export function Side({ side, children }: any) {
  const long = side === "long" || side === "buy";
  return (
    <span className={"side " + (long ? "long" : "short")}>
      <span className="tri">{long ? "▲" : "▼"}</span>
      {children || side.toUpperCase()}
    </span>
  );
}

/* ---------- Symbol cell ---------- */
export function Sym({ s }: any) {
  const meta = symMeta(s);
  return (
    <span className="sym">
      <span className="ic" style={{ background: meta.color }}>{meta.short}</span>
      {s}
    </span>
  );
}

/* ---------- Sortable header hook ---------- */
export function useSort(rows: any[], initialKey: string, initialDir = "desc") {
  const [key, setKey] = useState(initialKey);
  const [dir, setDir] = useState(initialDir);
  const sorted = useMemo(() => {
    const r = [...rows].sort((a, b) => {
      const av = a[key],
        bv = b[key];
      if (typeof av === "string") return dir === "asc" ? av.localeCompare(bv) : bv.localeCompare(av);
      return dir === "asc" ? av - bv : bv - av;
    });
    return r;
  }, [rows, key, dir]);
  const onSort = (k: string) => {
    if (k === key) setDir((d) => (d === "asc" ? "desc" : "asc"));
    else {
      setKey(k);
      setDir("desc");
    }
  };
  const arrow = (k: string) => (k === key ? <span className="ar">{dir === "asc" ? "↑" : "↓"}</span> : null);
  return { sorted, onSort, arrow, key, dir };
}
