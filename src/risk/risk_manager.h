#pragma once

#include "../core/event.h"
#include "../execution/portfolio.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>

// Forward-decl breaks a circular include with analytics.h.
struct AnalyticsReport;

struct risk_limits
{
    double max_position_value = 1e9;
    double max_drawdown = 0.30;
    double max_loss_per_trade = 10000.0;
    int max_open_orders = 1000;
    double max_portfolio_exposure = 5e9;

    double max_daily_loss = 0.0;
    int daily_reset_hour = 0;
    int max_trades_per_hour = 0;
    int max_orders_per_minute = 0;
};

enum class risk_action { pass, reject, halt, unwind };

// POD subset of AnalyticsReport — building a full report per order
// dominates the hot path (Sharpe/Sortino, vectors), so carry only what
// the risk checks actually read.
struct risk_snapshot
{
    double       max_drawdown   = 0.0;  // percent, matches AnalyticsReport
    std::size_t  total_orders   = 0;
    std::size_t  total_fills    = 0;
    double       last_trade_pnl = 0.0;
    bool         has_last_trade = false;
};

class RiskManager
{
public:
    explicit RiskManager(risk_limits limits = {});

    risk_action check_order(const order_event& order,
                            const portfolio& port,
                            const risk_snapshot& snap);

    risk_action check_post_fill(const fill_event& fill,
                                const portfolio& port,
                                const risk_snapshot& snap);

    // Legacy: workers/tests that already hold an AnalyticsReport.
    risk_action check_order(const order_event& order,
                            const portfolio& port,
                            const AnalyticsReport& snap);

    risk_action check_post_fill(const fill_event& fill,
                                const portfolio& port,
                                const AnalyticsReport& snap);

    void on_fill(const fill_event& fill);

private:
    risk_limits limits_;

    struct timestamped_entry {
        std::chrono::system_clock::time_point ts;
    };
    std::deque<timestamped_entry> order_timestamps_;
    std::deque<timestamped_entry> trade_timestamps_;

    double daily_loss_ = 0.0;
    double daily_start_equity_ = 0.0;
    std::chrono::system_clock::time_point daily_reset_tp_{};

    void update_daily_reset(std::chrono::system_clock::time_point now);
    void prune_old_entries(std::deque<timestamped_entry>& entries,
                           std::chrono::system_clock::time_point cutoff);
};
