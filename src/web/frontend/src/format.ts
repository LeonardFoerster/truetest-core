/* =========================================================================
   TrueTest — number/currency formatters + semantic color helpers.
   Ported verbatim from the Claude Design prototype (components.jsx).
   ========================================================================= */

export const fmt = {
  usd(v: number, d = 2) {
    return (v < 0 ? "-$" : "$") + Math.abs(v).toLocaleString("en-US", { minimumFractionDigits: d, maximumFractionDigits: d });
  },
  usdK(v: number) {
    const a = Math.abs(v);
    if (a >= 1e6) return (v < 0 ? "-$" : "$") + (a / 1e6).toFixed(2) + "M";
    if (a >= 1e3) return (v < 0 ? "-$" : "$") + (a / 1e3).toFixed(1) + "K";
    return fmt.usd(v);
  },
  num(v: number, d = 2) {
    return v.toLocaleString("en-US", { minimumFractionDigits: d, maximumFractionDigits: d });
  },
  sign(v: number, d = 2) {
    return (v >= 0 ? "+" : "−") + Math.abs(v).toLocaleString("en-US", { minimumFractionDigits: d, maximumFractionDigits: d });
  },
  signUsd(v: number, d = 2) {
    return (v >= 0 ? "+$" : "−$") + Math.abs(v).toLocaleString("en-US", { minimumFractionDigits: d, maximumFractionDigits: d });
  },
  pct(v: number, d = 2) {
    return (v >= 0 ? "+" : "−") + Math.abs(v * 100).toFixed(d) + "%";
  },
  pctP(v: number, d = 2) {
    return (v * 100).toFixed(d) + "%";
  },
  bps(v: number) {
    return v.toFixed(1);
  },
};

export const cls = (v: number) => (v > 0 ? "up" : v < 0 ? "down" : "flat");
export const tri = (side: string) => (side === "long" || side === "buy" ? "▲" : "▼");
