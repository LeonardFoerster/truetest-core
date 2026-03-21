#pragma once

#include "../core/event.h"
#include "../threading/worker.h"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// Welford's online algorithm for running mean/variance.
// Numerically stable, single-pass, O(1) per update.
struct welford_state
{
    int64_t n = 0;
    double mean = 0.0;
    double m2 = 0.0;  // sum of squared deviations

    void update(double x)
    {
        ++n;
        double delta = x - mean;
        mean += delta / static_cast<double>(n);
        m2 += delta * (x - mean);  // note: uses UPDATED mean
    }

    double variance() const { return n > 1 ? m2 / static_cast<double>(n - 1) : 0.0; }
    double stddev() const { return std::sqrt(variance()); }
    void reset() { n = 0; mean = 0.0; m2 = 0.0; }
};

struct equity_point
{
    std::chrono::system_clock::time_point timestamp;
    double equity;
};

struct trade_record
{
    uint64_t order_id;
    order_side side;
    int quantity;
    double fill_price;
    double commission;
    double intended_price;
    std::chrono::system_clock::time_point timestamp;
    double pnl;  // set on closing trade
};

struct AnalyticsReport
{
    // Returns
    double initial_equity = 0.0;
    double final_equity = 0.0;
    double cumulative_return = 0.0;
    std::vector<equity_point> equity_curve;
    std::vector<double> trade_returns;

    // Risk
    double sharpe_ratio = 0.0;
    double sortino_ratio = 0.0;
    double max_drawdown = 0.0;
    double calmar_ratio = 0.0;

    // Execution quality
    double avg_slippage = 0.0;
    std::size_t total_orders = 0;
    std::size_t total_fills = 0;

    // Exposure
    double time_in_market_pct = 0.0;
    double avg_holding_period_ms = 0.0;

    // Trade breakdown
    std::size_t total_trades = 0;  // round-trip (buy+sell = 1 trade)
    double win_rate = 0.0;
    double avg_win = 0.0;
    double avg_loss = 0.0;
    double profit_factor = 0.0;
    double largest_winner = 0.0;
    double largest_loser = 0.0;

    // Benchmark
    double buy_and_hold_return = 0.0;
    double strategy_vs_benchmark = 0.0;

    // Trade log
    std::vector<trade_record> trades;
};

class Analytics : public Worker
{
public:
    explicit Analytics(double initial_cash = 100000.0);

    void on_event(const event_pointer& ev) override;

    // Full report (calls snapshot() internally, adds equity curve + trade log)
    AnalyticsReport generate_report() const;

    // Lightweight mid-run snapshot: returns current metrics from running
    // accumulators without copying equity curve or trade log vectors.
    AnalyticsReport snapshot() const;

    void print_report() const;
    void export_csv(const std::string& equity_path, const std::string& trades_path) const;

private:
    void on_market(const market_event& m);
    void on_order(const order_event& o);
    void on_fill(const fill_event& f);

    double initial_cash_;
    double cash_;
    double position_qty_ = 0;
    double entry_price_ = 0.0;
    std::chrono::system_clock::time_point entry_time_;
    bool in_position_ = false;

    // Equity tracking
    double last_close_ = 0.0;
    std::vector<equity_point> equity_curve_;

    // Order → intended price for slippage
    std::map<uint64_t, double> order_prices_;

    // Trade records
    std::vector<trade_record> trades_;
    std::vector<double> trade_returns_;
    double total_slippage_ = 0.0;
    std::size_t slippage_count_ = 0;
    std::size_t total_orders_ = 0;
    std::size_t total_fills_ = 0;

    // Exposure tracking
    double total_holding_ms_ = 0.0;
    std::size_t holding_count_ = 0;
    std::size_t market_events_total_ = 0;
    std::size_t market_events_in_position_ = 0;

    // Buy-and-hold benchmark
    double first_price_ = 0.0;
    bool first_price_set_ = false;

    // --- Streaming / incremental accumulators ---

    // Welford running stats for trade returns (Sharpe)
    welford_state return_stats_;

    // Welford for downside returns only (Sortino)
    welford_state downside_stats_;

    // Running max drawdown
    double peak_equity_ = 0.0;
    double max_drawdown_ = 0.0;  // as fraction (0.0 - 1.0)

    // Running trade breakdown (avoid iterating trade_returns_ in snapshot)
    std::size_t win_count_ = 0;
    double total_win_ = 0.0;
    double total_loss_ = 0.0;
    double largest_winner_ = 0.0;
    double largest_loser_ = 0.0;
};
