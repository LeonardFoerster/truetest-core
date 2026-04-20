#pragma once

#include "analytics.h"

#include <cstddef>
#include <string>

namespace tt {

struct report_options
{
    std::size_t width = 72;
    std::size_t bar_width = 24;
    std::size_t sparkline_width = 60;
    std::size_t distribution_bins = 8;
    std::size_t worst_trades_count = 5;

    bool include_returns          = true;
    bool include_risk             = true;
    bool include_equity_sparkline = true;
    bool include_trades           = true;
    bool include_distribution     = true;
    bool include_execution        = true;
    bool include_exposure         = true;
    bool include_benchmark        = true;
    bool include_per_symbol       = true;
    bool include_per_strategy     = true;
    bool include_worst_trades     = true;

    std::string title = "Analytics Report";
};

std::string render_report(const AnalyticsReport& report,
                          const report_options& opts = {});

std::string render_returns_section          (const AnalyticsReport& r, const report_options& o);
std::string render_risk_section             (const AnalyticsReport& r, const report_options& o);
std::string render_trades_section           (const AnalyticsReport& r, const report_options& o);
std::string render_execution_section        (const AnalyticsReport& r, const report_options& o);
std::string render_exposure_section         (const AnalyticsReport& r, const report_options& o);
std::string render_distribution_section     (const AnalyticsReport& r, const report_options& o);
std::string render_equity_sparkline_section (const AnalyticsReport& r, const report_options& o);
std::string render_benchmark_section        (const AnalyticsReport& r, const report_options& o);
std::string render_per_symbol_section       (const AnalyticsReport& r, const report_options& o);
std::string render_per_strategy_section     (const AnalyticsReport& r, const report_options& o);
std::string render_worst_trades_section     (const AnalyticsReport& r, const report_options& o);

} // namespace tt
