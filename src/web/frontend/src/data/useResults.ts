/* =========================================================================
   TrueTest — backtest results hook.

   Fetches /api/results once, runs it through adaptReport. Falls back to the
   bundled fixture if no engine is reachable so the Backtest Review renders
   offline.
   ========================================================================= */
import { useEffect, useState } from "react";
import { adaptReport, type ReportData } from "../adapters/report";
import type { ResultsReport } from "../wire";
import { fixtureReport } from "../fixtures";
import { authHeaders } from "./store";

export interface ResultsState {
  report: ReportData | null;
  loading: boolean;
  offline: boolean;
}

export function useResults(): ResultsState {
  const [state, setState] = useState<ResultsState>({ report: null, loading: true, offline: false });

  useEffect(() => {
    let cancelled = false;
    const url = "/api/results";

    fetch(url, { headers: authHeaders() })
      .then((r) => (r.ok ? r.json() : Promise.reject(r.status)))
      .then((j: ResultsReport) => {
        if (!cancelled) setState({ report: adaptReport(j), loading: false, offline: false });
      })
      .catch(() => {
        if (!cancelled) setState({ report: fixtureReport, loading: false, offline: true });
      });

    return () => {
      cancelled = true;
    };
  }, []);

  return state;
}
