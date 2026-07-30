/* =========================================================================
   TrueTest — offline fixtures.

   Loads the engine-shaped JSON emitted by the C++ dump tool
   (src/web/tools/dump_fixtures.cpp) and runs it through the adapters, yielding
   the exact component prop shapes. This both powers offline `npm run dev` and
   serves as the compile-time contract check: if the C++ serializer shape and
   the adapters drift apart, `tsc` fails here.
   ========================================================================= */
import snapshotJson from "./snapshot.json";
import reportJson from "./report.json";
import { adaptSnapshot, type LiveData } from "../adapters/snapshot";
import { adaptReport, type ReportData } from "../adapters/report";
import type { SnapshotFrame, ResultsReport } from "../wire";

export const fixtureSnapshot: LiveData = adaptSnapshot(snapshotJson as unknown as SnapshotFrame);
export const fixtureReport: ReportData = adaptReport(reportJson as unknown as ResultsReport);
