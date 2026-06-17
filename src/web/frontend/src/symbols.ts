/* =========================================================================
   TrueTest — symbol presentation metadata (icon color + glyph).
   Pure display data, independent of the live feed. Unknown symbols fall back
   to a neutral chip with the first character.
   ========================================================================= */
import type { SymbolMeta } from "./types";

export const SYMS: Record<string, SymbolMeta> = {
  BTCUSDT: { px: 71248.5, tick: 0.5, color: "#f7931a", short: "₿" },
  ETHUSDT: { px: 3842.18, tick: 0.01, color: "#627eea", short: "Ξ" },
  SOLUSDT: { px: 177.94, tick: 0.01, color: "#14f195", short: "◎" },
};

export function symMeta(s: string): SymbolMeta {
  return SYMS[s] ?? { px: 0, tick: 0, color: "var(--bg-3)", short: (s || "?").slice(0, 1) };
}
