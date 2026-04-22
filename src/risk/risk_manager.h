#pragma once

#include "../core/event.h"
#include "../execution/portfolio.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>

// Forward-decl avoids a circular include: analytics.h needs risk_snapshot
// to expose a cheap accessor; risk_manager.h only uses AnalyticsReport by
// const-reference in the legacy overloads, so the full definition is only
// needed in the .cpp.
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

// Subset of AnalyticsReport consulted by RiskManager. Building a full
// AnalyticsReport per order is expensive (allocates trade / equity
// vectors, recomputes Sharpe/Sortino, etc.) and used to dominate the
// hot order path; the risk manager only ever reads a handful of fields
// so we carry them in this POD instead.
struct risk_snapshot
{
    double       max_drawdown   = 0.0;   // as percentage, matching AnalyticsReport.max_drawdown
    std::size_t  total_orders   = 0;
    std::size_t  total_fills    = 0;
    double       last_trade_pnl = 0.0;
    bool         has_last_trade = false;
};

class RiskManager
{
public:
    explicit RiskManager(risk_limits limits = {});

    // Fast-path overloads — construct a risk_snapshot from the analytics
    // layer in O(1). Preferred by the engine's per-order hot path.
    risk_action check_order(const order_event& order,
                            const portfolio& port,
                            const risk_snapshot& snap);

    risk_action check_post_fill(const fill_event& fill,
                                const portfolio& port,
                                const risk_snapshot& snap);

    // Legacy overloads — kept for workers and tests that already hold a
    // full AnalyticsReport. Forward into the fast path internally.
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
