#pragma once

#include "../core/event.h"
#include "../risk/risk_manager.h"
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

// End-of-run open inventory (mark-to-market).
struct open_position_report
{
    std::string symbol;
    double quantity = 0.0;      // signed: >0 long, <0 short
    double avg_entry = 0.0;
    double mark = 0.0;
    double unrealized_pnl = 0.0;
    const char* side() const { return quantity >= 0.0 ? "long" : "short"; }
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
    double annualized_return = 0.0;
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
    std::size_t winning_trades = 0;
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

    // Realized = sum of closed-trade PnL; unrealized = open MTM vs avg entry.
    double realized_pnl = 0.0;
    double unrealized_pnl = 0.0;
    std::vector<open_position_report> open_positions;

    // Research honesty fields (soft risk + data quality).
    std::size_t soft_post_fill_breaches = 0;
    std::size_t data_rows_rejected = 0;
    // Fee model label for exports (FR-zero-fee-default): "zero"|"fixed"|"tiered"|...
    std::string fee_model = "zero";
};

class Analytics : public Worker
{
public:
    explicit Analytics(double initial_cash = 100000.0,
                       std::size_t rolling_window = 252,
                       double risk_free_rate = 0.0,
                       std::size_t periods_per_year = 252,
                       std::size_t max_equity_points = 100000);

    // Pre-size the hot-growth vectors when the engine already knows the
    // bar count. Bounded by max_equity_points_ so we don't reserve the
    // full 10M for a casually-huge CSV.
    void reserve_hint(std::size_t expected_bars);

    void on_event(const event_pointer& ev) override;
    void on_funding(const funding_event& fe);   // Phase 2.1

    // Lightweight synchronous mark-to-market for the engine's hot path when
    // full analytics runs on a worker thread: keeps risk_view()'s equity and
    // drawdown current (and identical to inline mode) without the per-event
    // heavy work (equity curve, return stats, benchmark) the worker does.
    void on_mark(const std::string& symbol, double price)
    {
        open_positions_[symbol].last_price = price;
        last_close_ = price;
        update_risk_equity(cash_ + position_value());
    }

    // Phase 2.4 — allow external update of the current 8h funding rate    // (called from provider when better funding rate data is available)
    void set_current_funding_rate_8h(double rate) { current_funding_8h_rate_ = rate; }

    // Phase 2.1 - cumulative funding P&L (cash deltas from funding events)
    double total_funding_pnl() const { return total_funding_pnl_; }

    AnalyticsReport generate_report() const;

    AnalyticsReport snapshot() const;

    // Cheap (O(1)) read of the handful of fields the RiskManager consumes.
    // Replaces per-order snapshot() on the engine's hot path.
    risk_snapshot risk_view() const
    {
        risk_snapshot r;
        r.max_drawdown   = max_drawdown_ * 100.0;
        r.total_orders   = total_orders_;
        r.total_fills    = total_fills_;
        if (!trades_.empty())
        {
            r.has_last_trade = true;
            r.last_trade_pnl = trades_.back().pnl;
            r.last_trade_seq = trades_.size();
        }
        // Phase 2.3 + 2.4 - populated from L2 events, funding events, and equity tracking
        r.equity = last_equity_;
        r.realized_vol_1h = realized_vol_1h_;
        r.current_spread_bps = current_spread_bps_;
        r.current_funding_8h_rate = current_funding_8h_rate_;
        return r;
    }

    void print_report() const;
    void export_csv(const std::string& equity_path, const std::string& trades_path) const;
    void export_json(const std::string& path) const;

    // Soft post-fill risk continued under research soft mode (EL-06).
    void note_soft_post_fill_breach() { ++soft_post_fill_breaches_; }
    void set_soft_post_fill_breaches(std::size_t n) { soft_post_fill_breaches_ = n; }
    std::size_t soft_post_fill_breaches() const { return soft_post_fill_breaches_; }
    // Data load reject count (invalid CSV/tick rows) for report honesty (DR-02).
    void set_data_rows_rejected(std::size_t n) { data_rows_rejected_ = n; }
    // Fee model echo so exports cannot be misread as net-of-fees (FR-zero-fee-default).
    void set_fee_model(std::string label) { fee_model_ = std::move(label); }
    const std::string& fee_model() const { return fee_model_; }
    std::size_t data_rows_rejected() const { return data_rows_rejected_; }
    // Fold engine-local research counters into export analytics (threaded presets).
    void fold_research_counters(std::size_t soft_breaches, std::size_t rows_rejected)
    {
        if (soft_breaches > soft_post_fill_breaches_)
            soft_post_fill_breaches_ = soft_breaches;
        if (rows_rejected > data_rows_rejected_)
            data_rows_rejected_ = rows_rejected;
    }

    double rolling_sharpe() const;
    double rolling_max_drawdown() const;

    // Phase A (MC object reuse)
    void reset(double initial_cash);

    // Cheap O(n) copy of the last n equity values for the live TUI's
    // sparkline strip - avoids snapshot()'s full report rebuild on every
    // render tick. Returns the most recent n samples (or fewer if the
    // curve is shorter); empty if nothing recorded yet.
    std::vector<double> equity_tail(std::size_t n) const;

    // Last n drawdown values as positive percentages (0 = at peak,
    // 5.0 = 5% below peak). Walks equity_curve_ once to recover the
    // running peak so the values match how the live drawdown atomic
    // is reported elsewhere.
    std::vector<double> drawdown_tail(std::size_t n) const;

    // Cheap O(K) read of the per-strategy analytics map for the live
    // TUI. Avoids snapshot()'s full report rebuild on every render
    // tick (~tens of ms in a 100k-trade run). Returns a copy because
    // the underlying map is mutated on the analytics worker thread.
    std::unordered_map<std::string, sub_analytics> per_strategy_view() const
    {
        return per_strategy_;
    }

    // Cheap latency snapshot for the Health panel - same idea as
    // per_strategy_view: avoid the full report rebuild.
    struct latency_view
    {
        double      avg_ns = 0.0;
        std::int64_t min_ns = 0;
        std::int64_t max_ns = 0;
        std::size_t  samples = 0;
    };
    latency_view latency_view_now() const
    {
        latency_view v;
        v.samples = static_cast<std::size_t>(tick_to_trade_ns_.n);
        v.avg_ns  = tick_to_trade_ns_.mean;
        v.min_ns  = tick_to_trade_min_ns_;
        v.max_ns  = tick_to_trade_max_ns_;
        return v;
    }

    double realized_pnl() const { return total_win_ - total_loss_; }
    double gross_profit() const { return total_win_; }
    double gross_loss() const { return total_loss_; }
    double max_drawdown_pct() const { return max_drawdown_ * 100.0; }
    double win_rate_pct() const
    {
        return trade_returns_.empty()
            ? 0.0
            : static_cast<double>(win_count_)
              / static_cast<double>(trade_returns_.size()) * 100.0;
    }

    std::size_t winning_trades() const { return win_count_; }

private:
    void on_market(const market_event& m);
    void on_tick(const tick_event& t);
    void on_order(const order_event& o);
    void on_fill(const fill_event& f);

    // Phase 2.4 - track spread from L2 for circuit breakers
    void on_l2_snapshot(const l2_snapshot_event& ev);
    void on_l2_update(const l2_update_event& ev);

    double initial_cash_;
    double cash_;

    // Per-symbol open-position state. A single global position would net
    // unrelated instruments against each other and mark them at the wrong
    // price in multi-symbol runs.
    struct open_position
    {
        double qty = 0.0;
        double avg_entry = 0.0;
        double open_commission = 0.0;
        double last_price = 0.0;   // last seen close/tick/fill price for this symbol
        std::chrono::system_clock::time_point entry_time{};
    };
    std::unordered_map<std::string, open_position> open_positions_;

    // Sum of qty * last_price across symbols (mark-to-market value).
    double position_value() const
    {
        double v = 0.0;
        for (const auto& [_, p] : open_positions_)
            if (std::abs(p.qty) > 1e-12) v += p.qty * p.last_price;
        return v;
    }
    bool any_position_open() const
    {
        for (const auto& [_, p] : open_positions_)
            if (std::abs(p.qty) > 1e-12) return true;
        return false;
    }

    std::size_t rolling_window_;
    double risk_free_rate_;
    std::size_t periods_per_year_;
    std::size_t max_equity_points_;

    std::size_t equity_stride_ = 1;
    std::size_t equity_counter_ = 0;
    std::size_t bench_stride_ = 1;
    std::size_t bench_counter_ = 0;

    void record_equity_point(std::vector<equity_point>& curve,
                             std::size_t& stride,
                             std::size_t& counter,
                             const equity_point& pt);
    void update_risk_equity(double equity) noexcept;

    double last_close_ = 0.0;
    std::vector<equity_point> equity_curve_;

    std::deque<double> rolling_returns_;
    double prev_equity_ = 0.0;

    std::map<uint64_t, double> order_prices_;

    std::map<uint64_t, std::string> order_strategies_;

    std::vector<trade_record> trades_;
    std::vector<double> trade_returns_;

    // Phase 2.3 - equity and vol for risk_snapshot
    double last_equity_ = 0.0;
    double realized_vol_1h_ = 0.0;
    double last_mid_price_ = 0.0;   // for vol calculation

    // Phase 2.4 - current spread and funding rate (updated from L2 / funding events)
    double current_spread_bps_ = 0.0;
    double current_funding_8h_rate_ = 0.0;

    // Phase 2.1 - accumulated funding cash P&L
    double total_funding_pnl_ = 0.0;
    double total_slippage_ = 0.0;
    double total_slippage_signed_ = 0.0;
    double total_adverse_slippage_ = 0.0;
    double total_favorable_slippage_ = 0.0;
    std::size_t slippage_count_ = 0;
    std::size_t adverse_count_ = 0;
    std::size_t favorable_count_ = 0;
    std::size_t total_orders_ = 0;
    std::size_t soft_post_fill_breaches_ = 0;
    std::size_t data_rows_rejected_ = 0;
    std::string fee_model_{"zero"};
    std::size_t total_fills_ = 0;

    double total_holding_ms_ = 0.0;
    std::size_t holding_count_ = 0;
    std::size_t market_events_total_ = 0;
    std::size_t market_events_in_position_ = 0;

    double first_price_ = 0.0;
    bool first_price_set_ = false;
    std::vector<equity_point> benchmark_curve_;
    double prev_bh_equity_ = 0.0;

    std::vector<double> strategy_returns_;
    std::vector<double> benchmark_returns_;

    std::unordered_map<std::string, sub_analytics> per_symbol_;
    std::unordered_map<std::string, sub_analytics> per_strategy_;


    welford_state return_stats_;

    // Sum over ALL return periods of min(r - MAR, 0)^2; downside deviation
    // for Sortino is sqrt(downside_sq_sum_ / return_stats_.n). (A Welford
    // stddev over only the negative returns ignores the loss level and uses
    // the wrong observation count.)
    double downside_sq_sum_ = 0.0;

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
