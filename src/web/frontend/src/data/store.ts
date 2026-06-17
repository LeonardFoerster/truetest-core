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

// Optional ?token= passthrough so a token-protected server can still be opened
// directly in the browser (http://host:port/?token=...).
export function authToken(): string | null {
  try {
    return new URLSearchParams(window.location.search).get("token");
  } catch {
    return null;
  }
}
