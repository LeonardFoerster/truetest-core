#pragma once

#include "../core/event.h"
#include "../execution/portfolio.h"
#include "../analytics/analytics.h"

#include <chrono>
#include <cstdint>
#include <deque>

struct risk_limits
{
    double max_position_value = 1e9;     // max notional per symbol
    double max_drawdown = 0.30;          // 30% max drawdown → halt
    double max_loss_per_trade = 10000.0; // single trade loss limit
    int max_open_orders = 1000;          // prevent order flooding
    double max_portfolio_exposure = 5e9; // total across all symbols

    // Time-based limits (0 = no limit)
    double max_daily_loss = 0.0;         // max loss per day (resets at daily_reset_hour UTC)
    int daily_reset_hour = 0;            // hour (0-23) in UTC when daily loss counter resets
    int max_trades_per_hour = 0;         // max fills per rolling 60-minute window
    int max_orders_per_minute = 0;       // max orders per rolling 60-second window
};

enum class risk_action { pass, reject, halt, unwind };

class RiskManager
{
public:
    explicit RiskManager(risk_limits limits = {});

    // Called before an order is submitted to the orderbook.
    risk_action check_order(const order_event& order,
                            const portfolio& port,
                            const AnalyticsReport& snap);

    // Called after a fill to check portfolio-level limits.
    risk_action check_post_fill(const fill_event& fill,
                                const portfolio& port,
                                const AnalyticsReport& snap);

    // Record a fill for time-based tracking (called by engine after each fill)
    void on_fill(const fill_event& fill);

private:
    risk_limits limits_;

    // Time-based tracking state
    struct timestamped_entry {
        std::chrono::system_clock::time_point ts;
    };
    std::deque<timestamped_entry> order_timestamps_;   // for max_orders_per_minute
    std::deque<timestamped_entry> trade_timestamps_;   // for max_trades_per_hour

    // Daily loss tracking
    double daily_loss_ = 0.0;
    double daily_start_equity_ = 0.0;
    std::chrono::system_clock::time_point daily_reset_tp_{};  // next reset time

    void update_daily_reset(std::chrono::system_clock::time_point now);
    void prune_old_entries(std::deque<timestamped_entry>& entries,
                           std::chrono::system_clock::time_point cutoff);
};
