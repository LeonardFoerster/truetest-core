/* =========================================================================
   TrueTest — React context for the adapted live + report data.
   Components read their slice from here instead of a mock global, so the same
   panels render whether the data comes from the live WS feed or a fixture.
   ========================================================================= */
import { createContext, useContext } from "react";
import type { LiveData } from "../adapters/snapshot";
import type { ReportData } from "../adapters/report";

export const LiveCtx = createContext<LiveData | null>(null);
export const ReportCtx = createContext<ReportData | null>(null);

export const useLive = (): LiveData | null => useContext(LiveCtx);
export const useReport = (): ReportData | null => useContext(ReportCtx);

let sessionToken: string | null | undefined;

// Optional ?token= bootstrap so a token-protected server can still be opened
// directly in the browser. REST uses Authorization; WS keeps the query-token
// fallback because browsers cannot set headers on a WebSocket handshake.
export function authToken(): string | null {
  if (sessionToken !== undefined) return sessionToken;
  try {
    const url = new URL(window.location.href);
    sessionToken = url.searchParams.get("token");
    if (sessionToken) {
      url.searchParams.delete("token");
      const next = url.pathname + url.search + url.hash;
      window.history.replaceState(window.history.state, "", next);
    }
    return sessionToken;
  } catch {
    sessionToken = null;
    return null;
  }
}

export function authHeaders(): HeadersInit | undefined {
  const tok = authToken();
  return tok ? { Authorization: `Bearer ${tok}` } : undefined;
}
