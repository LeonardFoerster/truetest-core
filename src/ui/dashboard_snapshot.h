#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace truetest::ui {

// Engine-side snapshot consumed by the rich TUI panels. Filled under
// engine lock by engine::snapshot_dashboard() so the render thread can
// read a coherent view at the 100 ms tick.
struct dashboard_snapshot
{
    struct position_row
    {
        std::string symbol;
        double      qty           = 0.0;
        double      avg_entry     = 0.0;
        double      mark          = 0.0;
        double      unrealized    = 0.0;
    };

    struct lot_row
    {
        std::uint64_t opener_order_id = 0;
        std::string   symbol;
        std::string   strategy_name;
        char          side          = '?';   // 'L' or 'S'
        double        qty_open      = 0.0;
        double        entry_price   = 0.0;
        std::int64_t  age_seconds   = 0;
    };

    struct open_order_row
    {
        std::uint64_t order_id      = 0;
        std::string   symbol;
        std::string   strategy_name;
        char          side          = '?';   // 'B' or 'S'
        char          type          = '?';   // 'M' / 'L' / 'S' / 's'
        double        qty           = 0.0;
        double        price         = 0.0;
        std::int64_t  age_seconds   = 0;
        const char*   status        = "";
    };

    struct fill_row
    {
        std::chrono::system_clock::time_point ts{};
        std::string   symbol;
        char          side          = '?';
        double        qty           = 0.0;
        double        price         = 0.0;
        double        fee           = 0.0;
        const char*   source        = "";
    };

    struct risk_view
    {
        bool   halted               = false;
        double daily_loss           = 0.0;
        double daily_loss_limit     = 0.0;
        double max_drawdown_pct     = 0.0;
        double max_drawdown_limit   = 0.0;
        double exposure             = 0.0;
        double exposure_limit       = 0.0;
        std::size_t open_orders     = 0;
        std::size_t open_orders_limit = 0;
    };

    struct perf_view
    {
        std::size_t total_orders    = 0;
        std::size_t total_fills     = 0;
        std::size_t total_trades    = 0;
        double      win_rate        = 0.0;
        double      sharpe          = 0.0;
        double      sortino         = 0.0;
        double      profit_factor   = 0.0;
        double      avg_markout_bps = 0.0;
        std::size_t markout_samples = 0;
    };

    double cash             = 0.0;
    double equity           = 0.0;
    double initial_balance  = 0.0;
    double realized_pnl     = 0.0;
    double unrealized_pnl   = 0.0;

    std::vector<position_row>    positions;
    std::vector<lot_row>         lots;
    std::vector<open_order_row>  open_orders;
    std::vector<fill_row>        recent_fills;   // newest first

    risk_view risk;
    perf_view perf;
};

} // namespace truetest::ui
