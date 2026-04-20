#pragma once

#include "../core/event.h"
#include "../threading/worker.h"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

struct welford_state
{
    int64_t n = 0;
    double mean = 0.0;
    double m2 = 0.0;

    void update(double x)
    {
        ++n;
        double delta = x - mean;
        mean += delta / static_cast<double>(n);
        m2 += delta * (x - mean);
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
    double quantity;
    double fill_price;
    double commission;
    double intended_price;
    std::chrono::system_clock::time_point timestamp;
    double pnl;
    std::string symbol;
    std::string strategy_name;
};

struct sub_analytics
{
    double total_pnl = 0.0;
    std::size_t trade_count = 0;
    std::size_t win_count = 0;
    double total_win = 0.0;
    double total_loss = 0.0;

    double win_rate() const
    {
        return trade_count > 0
            ? static_cast<double>(win_count) / static_cast<double>(trade_count) * 100.0
            : 0.0;
    }
    double profit_factor() const
    {
        return total_loss > 0.0 ? total_win / total_loss : (total_win > 0.0 ? 1e9 : 0.0);
    }
};

struct AnalyticsReport
{
    double initial_equity = 0.0;
    double final_equity = 0.0;
    double cumulative_return = 0.0;
    std::vector<equity_point> equity_curve;
    std::vector<double> trade_returns;

    double sharpe_ratio = 0.0;
    double sortino_ratio = 0.0;
    double max_drawdown = 0.0;
    double calmar_ratio = 0.0;

    double avg_slippage = 0.0;
    double avg_slippage_signed = 0.0;
    double avg_adverse_slippage = 0.0;
    double avg_favorable_slippage = 0.0;
    std::size_t adverse_slippage_count = 0;
    std::size_t favorable_slippage_count = 0;
    std::size_t total_orders = 0;
    std::size_t total_fills = 0;

    double avg_tick_to_trade_ns = 0.0;
    int64_t min_tick_to_trade_ns = 0;
    int64_t max_tick_to_trade_ns = 0;
    std::size_t tick_to_trade_samples = 0;

    double time_in_market_pct = 0.0;
    double avg_holding_period_ms = 0.0;

    std::size_t total_trades = 0;
    double win_rate = 0.0;
    double avg_win = 0.0;
    double avg_loss = 0.0;
    double profit_factor = 0.0;
    double largest_winner = 0.0;
    double largest_loser = 0.0;

    double rolling_sharpe = 0.0;
    double rolling_max_drawdown = 0.0;

    double buy_and_hold_return = 0.0;
    double strategy_vs_benchmark = 0.0;
    double alpha = 0.0;
    double beta = 0.0;
    double information_ratio = 0.0;
    double tracking_error = 0.0;
    std::vector<equity_point> benchmark_equity_curve;

    std::unordered_map<std::string, sub_analytics> per_symbol;
    std::unordered_map<std::string, sub_analytics> per_strategy;

    std::vector<trade_record> trades;
};

class Analytics : public Worker
{
public:
    explicit Analytics(double initial_cash = 100000.0,
                       std::size_t rolling_window = 252,
                       double risk_free_rate = 0.0);

    void on_event(const event_pointer& ev) override;

    AnalyticsReport generate_report() const;

    AnalyticsReport snapshot() const;

    void print_report() const;
    void export_csv(const std::string& equity_path, const std::string& trades_path) const;
    void export_json(const std::string& path) const;

    double rolling_sharpe() const;
    double rolling_max_drawdown() const;

private:
    void on_market(const market_event& m);
    void on_tick(const tick_event& t);
    void on_order(const order_event& o);
    void on_fill(const fill_event& f);

    double initial_cash_;
    double cash_;
    double position_qty_ = 0.0;
    double avg_entry_price_ = 0.0;
    double total_open_commission_ = 0.0;
    std::chrono::system_clock::time_point entry_time_;

    std::size_t rolling_window_;
    double risk_free_rate_;

    double last_close_ = 0.0;
    std::vector<equity_point> equity_curve_;

    std::deque<double> rolling_returns_;
    double prev_equity_ = 0.0;

    std::map<uint64_t, double> order_prices_;

    std::map<uint64_t, std::string> order_strategies_;

    std::vector<trade_record> trades_;
    std::vector<double> trade_returns_;
    double total_slippage_ = 0.0;
    double total_slippage_signed_ = 0.0;
    double total_adverse_slippage_ = 0.0;
    double total_favorable_slippage_ = 0.0;
    std::size_t slippage_count_ = 0;
    std::size_t adverse_count_ = 0;
    std::size_t favorable_count_ = 0;
    std::size_t total_orders_ = 0;
    std::size_t total_fills_ = 0;

    double total_holding_ms_ = 0.0;
    std::size_t holding_count_ = 0;
    std::size_t market_events_total_ = 0;
    std::size_t market_events_in_position_ = 0;

    double first_price_ = 0.0;
    bool first_price_set_ = false;
    std::vector<equity_point> benchmark_curve_;

    std::vector<double> strategy_returns_;
    std::vector<double> benchmark_returns_;

    std::unordered_map<std::string, sub_analytics> per_symbol_;
    std::unordered_map<std::string, sub_analytics> per_strategy_;


    welford_state return_stats_;

    welford_state downside_stats_;

    double peak_equity_ = 0.0;
    double max_drawdown_ = 0.0;

    std::size_t win_count_ = 0;
    double total_win_ = 0.0;
    double total_loss_ = 0.0;
    double largest_winner_ = 0.0;
    double largest_loser_ = 0.0;

    welford_state tick_to_trade_ns_;
    int64_t tick_to_trade_min_ns_ = 0;
    int64_t tick_to_trade_max_ns_ = 0;
};
