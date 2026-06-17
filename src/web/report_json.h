#pragma once

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
std::string report_to_json(const AnalyticsReport& r);

inline constexpr int report_schema_version = 1;

} // namespace truetest::web
