#pragma once

// ============================================================
// LIVE-SAFETY SURFACE — Phase 1 freeze (see prod.md)
// Any edit requires explicit two-person CCB review + 4 h
// mainnet shadow run on engine_shadow before merge.
// Files in this set: tt_target.h, engine.{h,cpp}, all
// *kill_switch*, *dead_mans_switch*, *reconciler* under
// providers/binance/, risk/*, ExecutionBridge, live_safety.h
// ============================================================

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

    double max_daily_loss = 0.0;  // daily_loss_ accumulates realized negative PnL
    int daily_reset_hour = 0;
    int max_trades_per_hour = 0;
    int max_orders_per_minute = 0;

    // Phase 2.3 — position sizing as % of equity + volatility
    double max_position_pct_of_equity = 0.0;  // 0 = disabled
    double max_portfolio_var_estimate = 0.0;  // 0 = disabled

    // Phase 2.4 — circuit breakers
    double max_spread_bps = 0.0;         // 0 = disabled
    double max_funding_8h_rate = 0.0;    // 0 = disabled (as fraction, e.g. 0.0005 = 0.05%)
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
    std::size_t  last_trade_seq = 0;

    // Note: daily_loss is maintained internally in RiskManager (realized negative trade PnL)

    // Phase 2.3 — filled by analytics worker before passing to RiskManager
    double equity = 0.0;
    double realized_vol_1h = 0.0;   // simple Welford or EWMA proxy

    // Phase 2.4 — current market conditions for circuit breakers
    double current_spread_bps = 0.0;
    double current_funding_8h_rate = 0.0;  // last known 8h funding rate (fraction)
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

    // Phase A (MC object reuse)
    void reset();

private:
    risk_limits limits_;

    struct timestamped_entry {
        std::chrono::system_clock::time_point ts;
    };
    std::deque<timestamped_entry> order_timestamps_;
    std::deque<timestamped_entry> trade_timestamps_;

    double daily_loss_ = 0.0;  // realized losses (see check_post_fill)
    double daily_start_equity_ = 0.0;
    std::chrono::system_clock::time_point daily_reset_tp_{};

    // Guard to avoid re-adding the same closed trade if check_post_fill is
    // invoked multiple times for the latest analytics snapshot.
    std::size_t last_daily_trade_seq_added_ = 0;

    void update_daily_reset(std::chrono::system_clock::time_point now);
    void prune_old_entries(std::deque<timestamped_entry>& entries,
                           std::chrono::system_clock::time_point cutoff);
};
