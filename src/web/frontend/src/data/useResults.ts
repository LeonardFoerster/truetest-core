/* =========================================================================
   TrueTest — backtest results hook.

   Fetches /api/results once and runs it through adaptReport. Unavailable or
   unpublished results remain visibly unavailable; financial fixtures are
   never substituted for engine output.
   ========================================================================= */
import { useEffect, useState } from "react";
import { adaptReportJson, REPORT_LIMITS, type ReportData } from "../adapters/report";
import { authHeaders } from "./store";
import { readBoundedResponseText } from "./readBoundedResponse";

export interface ResultsState {
  report: ReportData | null;
  loading: boolean;
  offline: boolean;
}

export function useResults(): ResultsState {
  const [state, setState] = useState<ResultsState>({ report: null, loading: true, offline: false });

  useEffect(() => {
    let cancelled = false;
    const controller = new AbortController();
    const url = "/api/results";

    fetch(url, { headers: authHeaders(), signal: controller.signal })
      .then(async (r) => {
        if (!r.ok) throw r.status;
        const text = await readBoundedResponseText(r, REPORT_LIMITS.reportBytes);
        return adaptReportJson(text);
      })
      .then((report) => {
        if (!cancelled) setState({ report, loading: false, offline: false });
      })
      .catch(() => {
        // A 503 is the backend's explicit fail-closed "report not safely
        // published" state. Never replace it (or a network failure) with
        // plausible demo financial results.
        if (!cancelled) setState({ report: null, loading: false, offline: true });
      });

    return () => {
      cancelled = true;
      controller.abort("results_view_unmounted");
    };
  }, []);

  return state;
}
