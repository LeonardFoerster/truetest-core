#pragma once

#include "analytics/results_json.h"

#include <string>

struct AnalyticsReport;

namespace truetest::web {

// Serialize a completed-run analytics report to the ResultsReport JSON
// contract for the Backtest Review screen.
//
// Supersedes the C-API's report_to_json (src/api/truetest_api.cpp): same flat
// metrics, but also emits the full trades[] blotter, the benchmark equity
// curve, trade_returns, and the slippage/latency block — everything the
// review UI renders. Faithful field names; the frontend adapter renames.
inline std::string report_to_json(const AnalyticsReport& report)
{
    return truetest::analytics_json::report_to_json(report);
}

// Accounting fields are additive to the v1 review contract.
inline constexpr int report_schema_version =
    truetest::analytics_json::report_schema_version;

} // namespace truetest::web
