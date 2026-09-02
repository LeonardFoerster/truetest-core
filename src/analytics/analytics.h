#pragma once

#include "../core/event.h"
#include "../risk/risk_manager.h"
#include "../threading/worker.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
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
    std::string commission_currency;
    double intended_price;
    std::chrono::system_clock::time_point timestamp;
    double pnl;
    std::string symbol;
    std::string strategy_name;
    double reference_price = 0.0;
    std::chrono::system_clock::time_point reference_timestamp{};
    double modeled_spread_bps = 0.0;
    double modeled_impact_bps = 0.0;
    double fill_probability = 1.0;
    std::chrono::nanoseconds modeled_latency{0};
    fill_execution_model execution_model = fill_execution_model::unclassified;
    fill_execution_reason execution_reason = fill_execution_reason::unknown;
    bool execution_is_exploratory = false;
    // Preserve the physical fill identity in the one report row emitted for
    // each accepted physical fill. Local fill ids are scoped by order id.
    uint64_t fill_id = 0;
    std::string venue_execution_id;
};

// End-of-run open inventory (mark-to-market).
struct open_position_report
{
    std::string symbol;
    std::string strategy_name;
    double quantity = 0.0;      // signed: >0 long, <0 short
    double avg_entry = 0.0;
    double mark = 0.0;
    bool mark_valid = false;
    std::string mark_source = "fill_price_provisional";
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
        return total_loss > 0.0 ? total_win / total_loss : 0.0;
    }
    bool profit_factor_valid() const { return total_loss > 0.0; }
    bool profit_factor_unbounded() const
    {
        return total_loss == 0.0 && total_win > 0.0;
    }
    const char* profit_factor_reason() const
    {
        if (profit_factor_valid()) return "computed_from_gross_win_and_loss";
        if (profit_factor_unbounded()) return "no_losses_unbounded";
        return "no_winning_or_losing_trades";
    }
};

struct AnalyticsReport
{
    double initial_equity = 0.0;
    double final_equity = 0.0;
    // Accounting identity (all values in account currency):
    // final = initial + realized_net + funding + unrealized_net + residual.
    double gross_realized_pnl = 0.0;
    double realized_pnl = 0.0;
    double funding_pnl = 0.0;
    double unrealized_pnl = 0.0;
    double total_commission = 0.0;
    double reconciliation_residual = 0.0;
    bool accounting_reconciled = true;
    std::string accounting_reconciliation_reason =
        "reconciled_within_floating_tolerance";
    // Default execution data is 1-minute, 24/7 crypto. Callers handling a
    // different cadence must supply the matching basis explicitly.
    std::size_t periods_per_year = 525600;
    // Risk-return samples are market marks only. Cash settlements are
    // separately reconciled and reset the market-return baseline, preventing
    // an irregular funding event from masquerading as another bar period.
    std::string return_observation_basis =
        "market_marks_excluding_cash_settlements";
    bool portfolio_time_series_valid = true;
    std::string portfolio_time_series_reason =
        "causal_complete_mark_cycles";
    std::size_t ambiguous_portfolio_mark_sequences_rejected = 0;
    // The chart may be adaptively decimated to its configured memory cap.
    // These values make the sampling honest while preserving an exact
    // full-stream drawdown witness for reconciliation.
    std::size_t equity_curve_sample_stride = 1;
    double max_drawdown_peak_equity = 0.0;
    double max_drawdown_trough_equity = 0.0;
    double cumulative_return = 0.0;
    double annualized_return = 0.0;
    bool annualized_return_valid = false;
    std::string annualized_return_reason = "insufficient_time_horizon";
    std::string annualized_return_basis = "causal_elapsed_time_365d";
    std::vector<equity_point> equity_curve;
    std::vector<double> trade_returns;

    double sharpe_ratio = 0.0;
    bool sharpe_ratio_valid = false;
    std::string sharpe_ratio_reason = "insufficient_return_observations";
    double sortino_ratio = 0.0;
    bool sortino_ratio_valid = false;
    std::string sortino_ratio_reason = "insufficient_return_observations";
    double max_drawdown = 0.0;
    double calmar_ratio = 0.0;
    bool calmar_ratio_valid = false;
    std::string calmar_ratio_reason = "annualized_return_unavailable";

    double avg_slippage = 0.0;
    double avg_slippage_signed = 0.0;
    double avg_adverse_slippage = 0.0;
    double avg_favorable_slippage = 0.0;
    std::size_t adverse_slippage_count = 0;
    std::size_t favorable_slippage_count = 0;
    std::size_t total_orders = 0;
    std::size_t total_fills = 0;
    // Exactly-once ingress evidence. Missing identities and contradictory
    // replays are rejected before any economic mutation; exact replays are
    // harmless no-ops. Exposing all three prevents rejected venue evidence
    // from disappearing into a private diagnostic counter.
    std::size_t duplicate_fill_replays_ignored = 0;
    std::size_t conflicting_fill_replays_rejected = 0;
    std::size_t missing_fill_identities_rejected = 0;
    std::size_t invalid_fill_payloads_rejected = 0;
    std::size_t unreconciled_funding_events_rejected = 0;
    std::size_t duplicate_funding_replays_ignored = 0;
    std::size_t conflicting_funding_replays_rejected = 0;
    std::size_t late_fill_events_rejected = 0;
    std::size_t late_funding_events_rejected = 0;
    std::size_t late_market_events_rejected = 0;
    std::size_t duplicate_market_marks_ignored = 0;
    std::size_t conflicting_market_marks_rejected = 0;

    double avg_tick_to_trade_ns = 0.0;
    int64_t min_tick_to_trade_ns = 0;
    int64_t max_tick_to_trade_ns = 0;
    std::size_t tick_to_trade_samples = 0;

    double time_in_market_pct = 0.0;
    bool time_in_market_valid = false;
    std::string time_in_market_reason = "insufficient_time_horizon";
    double avg_holding_period_ms = 0.0;

    // F-09b: one closed round trip (a position returning to flat), NOT one
    // closing fill leg. A single exit walking four book levels is one trade
    // that filled in four pieces; counting the legs inflated trade counts and
    // every per-trade average derived from them.
    std::size_t total_trades = 0;
    // Kept alongside so execution quality stays visible: how many closing
    // fills those round trips took.
    std::size_t closing_fill_legs = 0;
    std::size_t winning_trades = 0;

    double win_rate = 0.0;
    double avg_win = 0.0;
    double avg_loss = 0.0;
    double total_win = 0.0;
    double total_loss = 0.0;
    double profit_factor = 0.0;
    bool profit_factor_valid = false;
    bool profit_factor_unbounded = false;
    std::string profit_factor_reason = "no_winning_or_losing_trades";
    double largest_winner = 0.0;
    double largest_loser = 0.0;

    double rolling_sharpe = 0.0;
    double rolling_max_drawdown = 0.0;
    std::size_t rolling_return_count = 0;
    std::size_t rolling_window = 0;
    // `computed`, `insufficient_return_observations`,
    // `zero_return_variance`, or `no_drawdown_in_window`.
    std::string rolling_sharpe_reason = "insufficient_return_observations";
    std::string rolling_max_drawdown_reason = "insufficient_return_observations";

    double buy_and_hold_return = 0.0;
    double strategy_vs_benchmark = 0.0;
    bool benchmark_valid = false;
    std::string benchmark_reason = "no_market_symbol";
    std::string benchmark_symbol;
    double alpha = 0.0;
    double beta = 0.0;
    double information_ratio = 0.0;
    double tracking_error = 0.0;
    std::size_t benchmark_equity_curve_sample_stride = 1;
    std::string benchmark_curve_observation_basis =
        "selected_symbol_market_marks";
    std::vector<equity_point> benchmark_equity_curve;

    std::unordered_map<std::string, sub_analytics> per_symbol;
    std::unordered_map<std::string, sub_analytics> per_strategy;

    std::vector<trade_record> trades;
    // Any synthetic paper fill makes the run exploratory for execution-validity
    // claims, even when other fills are venue-reported.
    bool contains_exploratory_execution = false;

    // Open-position MTM is complete only when every open symbol has an
    // accepted market/tick/L2 mark. A fill price remains visible as a causal
    // provisional value but must not masquerade as an authoritative mark.
    bool valuation_complete = true;
    std::string valuation_reason = "all_open_positions_market_marked";

    // Realized = sum of closed-trade PnL; unrealized = open MTM vs avg entry.
    std::vector<open_position_report> open_positions;

    // Research honesty fields (soft risk + data quality).
    std::size_t soft_post_fill_breaches = 0;
    std::size_t data_rows_rejected = 0;
    // F-05a: the account's marked equity reached or crossed zero during the
    // run. Nothing in the engine liquidates or halts on that today, so every
    // number below describes a position that could not have existed. The
    // report renders an explicit invalidity banner instead of a Sharpe ratio.
    bool   bankrupt = false;
    double bankrupt_equity = 0.0;
    // F-06: ExitManager intent lifecycle, so a leaked or evicted pending
    // intent is visible in a report rather than only in the container.
    std::size_t exit_intents_registered = 0;
    std::size_t exit_intents_armed = 0;
    std::size_t exit_intents_cancelled = 0;
    std::size_t exit_intents_evicted = 0;
    // F-01(a): brackets refused because entry slippage reached the trade's
    // own designed stop distance, and the flattens that followed.
    std::size_t exit_slippage_disarms = 0;
    std::size_t exit_flatten_requests = 0;

    // Fee model label for exports (FR-zero-fee-default): "zero"|"fixed"|"tiered"|...
    std::string fee_model = "zero";

    // Schema-v1 serialization shared by the embedded web server and the C API.
    // Deterministic hashing canonicalizes the parsed representation; the raw
    // wire serializer intentionally preserves the established web contract.
    [[nodiscard]] std::string to_results_json() const;
};

class Analytics : public Worker
{
public:
    explicit Analytics(double initial_cash = 100000.0,
                       std::size_t rolling_window = 252,
                       double risk_free_rate = 0.0,
                       std::size_t periods_per_year = 525600,
                       std::size_t max_equity_points = 100000);

    // Pre-size the hot-growth vectors when the engine already knows the
    // bar count. Bounded by max_equity_points_ so we don't reserve the
    // full 10M for a casually-huge CSV.
    void reserve_hint(std::size_t expected_bars);

    void on_event(const event_pointer& ev) override;
    void on_funding(const funding_event& fe);   // Phase 2.1

    // Lightweight synchronous mark-to-market for the engine's hot path when
    // full analytics runs on a worker thread. For a multi-symbol portfolio a
    // drawdown observation is committed only after every economically open
    // symbol has received a fresh mark. This prevents a transient half-marked
    // hedge from becoming an order-dependent risk event.
    void on_mark(const std::string& symbol, double price)
    {
        if (!(price > 0.0) || !std::isfinite(price))
            return;
        if (!mark_keeps_equity_finite(symbol, price))
            return;
        set_symbol_price(symbol, price);
        bool have_open_position = false;
        bool complete_snapshot = true;
        bool repeated_before_snapshot_completion = false;
        for (const auto& [_, position] : open_positions_)
        {
            if (std::abs(position.qty) <= 1e-12)
                continue;
            if (position.symbol == symbol)
                repeated_before_snapshot_completion =
                    repeated_before_snapshot_completion
                    || position.fresh_for_risk_snapshot;
        }
        if (repeated_before_snapshot_completion)
        {
            invalidate_portfolio_time_series(
                portfolio_time_series_failure::ambiguous_mark_order);
            last_equity_ = cash_ + position_value();
            return;
        }
        for (auto& [_, position] : open_positions_)
        {
            if (std::abs(position.qty) <= 1e-12)
                continue;
            have_open_position = true;
            if (position.symbol == symbol)
                position.fresh_for_risk_snapshot = true;
            complete_snapshot = complete_snapshot
                && position.fresh_for_risk_snapshot;
        }

        const double equity = cash_ + position_value();
        if (!have_open_position || !complete_snapshot)
        {
            last_equity_ = equity;
            return;
        }
        clear_risk_mark_cycle();
        update_risk_equity(equity);
    }

    // Phase 2.4 — allow external update of the current 8h funding rate
    // (called from a provider when a dedicated funding-rate feed exists).
    // R3: also derived from funding settlements in on_funding(), which is the
    // producer the repository actually has — see the R3 design note §7.
    void set_current_funding_rate_8h(double rate)
    {
        if (!std::isfinite(rate))
            return;
        external_worst_funding_8h_rate_ = std::abs(rate);
        if (funding_reconciliation_failed_)
            return;
        current_funding_8h_rate_ = *external_worst_funding_8h_rate_;
        for (const auto& [_, candidate] : symbol_funding_8h_rates_)
            current_funding_8h_rate_ = std::max(
                current_funding_8h_rate_, candidate);
        funding_rate_known_ = true;
    }
    bool funding_rate_known() const { return funding_rate_known_; }

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
        // R3: total_orders_/total_fills_ are reporting counters and are
        // deliberately NOT carried into the risk snapshot. Open-order state
        // comes from the authoritative ledger (OrderTracker).
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
        // R3: an unknown funding rate must not read as "0.0, therefore inside
        // the limit". The breaker only engages once a rate actually exists.
        r.funding_rate_known = funding_rate_known_;
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

    // F-05a: latched by the engine's authoritative marked-equity pass the
    // first time account equity reaches or crosses zero.
    void mark_bankrupt(double equity)
    {
        if (bankrupt_) return;
        bankrupt_ = true;
        bankrupt_equity_ = equity;
    }
    bool is_bankrupt() const noexcept { return bankrupt_; }

    // F-06: ExitManager lifecycle counters, pushed in by the engine at
    // report time (Analytics does not depend on the exits layer).
    struct exit_lifecycle_counts
    {
        std::size_t registered = 0;
        std::size_t armed = 0;
        std::size_t cancelled = 0;
        std::size_t evicted = 0;
        std::size_t slippage_disarms = 0;
        std::size_t flatten_requests = 0;
    };
    void set_exit_lifecycle_counts(const exit_lifecycle_counts& c) { exit_lifecycle_ = c; }

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

    // F-05a / F-06: the bankruptcy latch and the exit-intent lifecycle live
    // on the engine event loop. A threaded get_analytics() returns a worker
    // Analytics that never saw either, so they are folded in after join for
    // the same reason the research counters are.
    void fold_lifecycle_counters(bool bankrupt, double bankrupt_equity,
                                 const exit_lifecycle_counts& exits)
    {
        if (bankrupt && !bankrupt_)
        {
            bankrupt_ = true;
            bankrupt_equity_ = bankrupt_equity;
        }
        exit_lifecycle_ = exits;
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

    double realized_pnl() const { return realized_leg_pnl_; }

    double gross_profit() const { return total_win_; }
    double gross_loss() const { return total_loss_; }
    double max_drawdown_pct() const { return max_drawdown_ * 100.0; }
    double win_rate_pct() const
    {
        return round_trip_count_ == 0
            ? 0.0
            : static_cast<double>(win_count_)
              / static_cast<double>(round_trip_count_) * 100.0;
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

    struct strategy_symbol_key
    {
        std::string strategy;
        std::string symbol;

        bool operator==(const strategy_symbol_key& other) const noexcept
        {
            return strategy == other.strategy && symbol == other.symbol;
        }
    };

    struct strategy_symbol_key_hash
    {
        std::size_t operator()(const strategy_symbol_key& k) const noexcept
        {
            std::size_t h1 = std::hash<std::string>{}(k.strategy);
            std::size_t h2 = std::hash<std::string>{}(k.symbol);
            return h1 ^ (h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
        }
    };

    // Per-(strategy, symbol) open-position state. Isolates multi-strategy
    // positions on the same instrument and avoids accidental netting or
    // misattributed PnL between different strategies.
    struct open_position
    {
        std::string symbol;
        std::string strategy_name;
        double qty = 0.0;
        double avg_entry = 0.0;
        double open_commission = 0.0;
        double last_price = 0.0;   // last seen close/tick/fill price for this symbol
        bool has_market_mark = false;
        bool fresh_for_risk_snapshot = false;
        bool fresh_for_portfolio_clock = false;
        std::chrono::system_clock::time_point entry_time{};
        // F-09b: PnL accumulated across every closing leg of the round trip
        // currently in progress, finalized once the position returns to flat.
        double      round_trip_pnl = 0.0;
        std::size_t round_trip_legs = 0;
    };

    std::unordered_map<strategy_symbol_key, open_position, strategy_symbol_key_hash> open_positions_;
    std::unordered_map<std::string, double> symbol_last_prices_;

    struct symbol_market_state
    {
        double first_price = 0.0;
        double last_price = 0.0;
        double last_mid = 0.0;
        double realized_vol_1h = 0.0;
        std::chrono::system_clock::time_point first_timestamp{};
        std::chrono::system_clock::time_point last_timestamp{};
        bool has_mark = false;
        // Benchmark observations are bar-completion marks only. Tick/L2
        // valuation must not become a hidden benchmark baseline.
        double benchmark_first_price = 0.0;
        double benchmark_last_price = 0.0;
        std::chrono::system_clock::time_point benchmark_first_timestamp{};
        std::chrono::system_clock::time_point benchmark_last_timestamp{};
        bool has_benchmark_mark = false;
        std::size_t benchmark_mark_count = 0;
        // Historical property: once a qualified benchmark stream has moved
        // away from its first mark it remains varying even after returning.
        bool benchmark_has_varied = false;
    };
    std::unordered_map<std::string, symbol_market_state> symbol_market_states_;
    std::size_t varying_market_symbols_ = 0;
    std::unordered_map<std::string, double> symbol_spread_bps_;
    bool benchmark_path_valid_ = true;
    bool benchmark_history_complete_ = true;
    std::string benchmark_symbol_;
    std::optional<std::chrono::system_clock::time_point>
        benchmark_selection_timestamp_;
    std::optional<std::chrono::system_clock::time_point>
        last_portfolio_clock_timestamp_;
    std::optional<std::chrono::system_clock::time_point>
        portfolio_mark_cycle_max_timestamp_;
    bool portfolio_time_series_valid_ = true;
    enum class portfolio_time_series_failure : std::uint8_t
    {
        none,
        ambiguous_mark_order,
        non_finite_return,
        late_market_event,
        conflicting_market_mark
    };
    portfolio_time_series_failure portfolio_time_series_failure_ =
        portfolio_time_series_failure::none;
    std::size_t ambiguous_portfolio_mark_sequences_rejected_ = 0;

    void set_symbol_price(const std::string& symbol, double price)
    {
        symbol_last_prices_[symbol] = price;
        for (auto& [k, pos] : open_positions_)
        {
            if (k.symbol == symbol)
            {
                pos.last_price = price;
                pos.has_market_mark = true;
            }
        }
    }

    // Sum of qty * last_price across open strategy positions (mark-to-market value).
    double position_value() const
    {
        long double v = 0.0L;
        for (const auto& [_, p] : open_positions_)
        {
            if (std::abs(p.qty) > 1e-12)
            {
                double px = p.last_price;
                if (!(px > 0.0))
                {
                    auto it = symbol_last_prices_.find(p.symbol);
                    if (it != symbol_last_prices_.end()) px = it->second;
                }
                v += static_cast<long double>(p.qty)
                    * static_cast<long double>(px);
            }
        }
        return static_cast<double>(v);
    }
    bool any_position_open() const
    {
        for (const auto& [_, p] : open_positions_)
            if (std::abs(p.qty) > 1e-12) return true;
        return false;
    }
    bool has_open_position_for_symbol(std::string_view symbol) const
    {
        for (const auto& [_, position] : open_positions_)
            if (position.symbol == symbol
                && std::abs(position.qty) > 1e-12)
                return true;
        return false;
    }
    bool has_economic_history_for_symbol(std::string_view symbol) const
    {
        for (const auto& [_, position] : open_positions_)
            if (position.symbol == symbol)
                return true;
        return false;
    }
    enum class market_state_update
    {
        rejected,
        accepted,
        benchmark_changed
    };
    market_state_update update_symbol_market_state(
        const std::string& symbol, double price,
        std::chrono::system_clock::time_point timestamp,
        bool allow_same_timestamp_change = false,
        bool eligible_for_benchmark = true);
    bool mark_keeps_equity_finite(std::string_view symbol,
                                  double price) const noexcept;
    bool mark_open_positions_fresh(
        std::string_view symbol,
        std::chrono::system_clock::time_point timestamp) noexcept;
    bool portfolio_mark_cycle_complete() const noexcept;
    static std::string_view portfolio_time_series_reason(
        portfolio_time_series_failure failure) noexcept;
    void invalidate_portfolio_time_series(
        portfolio_time_series_failure failure) noexcept;
    void clear_risk_mark_cycle() noexcept
    {
        for (auto& [_, position] : open_positions_)
            position.fresh_for_risk_snapshot = false;
    }
    void clear_portfolio_mark_cycle() noexcept
    {
        for (auto& [_, position] : open_positions_)
            position.fresh_for_portfolio_clock = false;
        portfolio_mark_cycle_max_timestamp_.reset();
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
    void record_equity_return(
        double equity,
        std::chrono::system_clock::time_point timestamp);
    void update_risk_equity(double equity) noexcept;
    bool advance_time_accounting(
        std::chrono::system_clock::time_point timestamp) noexcept;

    struct fill_identity
    {
        std::uint64_t order_id = 0;
        std::uint64_t fill_id = 0;
        std::string symbol;
        std::string strategy_name;
        std::string venue_execution_id;
        std::string commission_currency;
        std::uint64_t opener_order_id = 0;
        std::chrono::system_clock::time_point timestamp{};
        order_side side = order_side::buy;
        double quantity = 0.0;
        double price = 0.0;
        double commission = 0.0;
        double remaining = 0.0;
    };
    struct fill_identity_key
    {
        std::uint64_t order_id = 0;
        std::uint64_t fill_id = 0;

        bool operator==(const fill_identity_key&) const noexcept = default;
    };
    struct fill_identity_key_hash
    {
        std::size_t operator()(const fill_identity_key& key) const noexcept
        {
            const std::size_t h1 = std::hash<std::uint64_t>{}(key.order_id);
            const std::size_t h2 = std::hash<std::uint64_t>{}(key.fill_id);
            return h1 ^ (h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
        }
    };
    enum class fill_admission
    {
        accepted,
        duplicate,
        conflicting_replay,
        missing_identity
    };
    struct physical_fill_candidate
    {
        fill_identity identity;
        std::string venue_key;
    };
    static physical_fill_candidate make_physical_fill_candidate(
        const fill_event& fill);
    fill_admission classify_physical_fill(
        const physical_fill_candidate& candidate) const;
    void remember_physical_fill(physical_fill_candidate candidate);
    bool fill_keeps_accounting_finite(
        const fill_event& fill,
        std::string_view strategy_name) const;
    static bool same_physical_fill(const fill_identity& lhs,
                                   const fill_identity& rhs) noexcept;

    std::vector<equity_point> equity_curve_;

    std::deque<double> rolling_returns_;
    double prev_equity_ = 0.0;

    std::map<uint64_t, double> order_prices_;

    std::map<uint64_t, std::string> order_strategies_;

    std::vector<trade_record> trades_;
    std::vector<double> trade_returns_;
    // F-09b: closed round trips. win/loss aggregates below are keyed off
    // these, not off the individual closing legs in trade_returns_.
    std::size_t round_trip_count_ = 0;
    // Exact per-leg realized PnL, kept separate so a position that has
    // scaled out but not yet closed still reconciles against cash.
    double realized_leg_pnl_ = 0.0;
    double gross_realized_pnl_ = 0.0;
    double total_commission_ = 0.0;
    bool   bankrupt_ = false;          // F-05a
    double bankrupt_equity_ = 0.0;
    exit_lifecycle_counts exit_lifecycle_{};   // F-06


    // Phase 2.3 - equity and vol for risk_snapshot
    double last_equity_ = 0.0;
    double realized_vol_1h_ = 0.0;
    // Phase 2.4 - current spread and funding rate (updated from L2 / funding events)
    double current_spread_bps_ = 0.0;
    double current_funding_8h_rate_ = 0.0;
    bool   funding_rate_known_ = false;
    std::unordered_map<std::string, double> symbol_funding_8h_rates_;
    // The legacy provider seam is not symbol-qualified. Preserve it only as
    // a conservative account-wide worst rate so a later settlement cannot
    // accidentally overwrite a more adverse feed observation.
    std::optional<double> external_worst_funding_8h_rate_;
    struct funding_identity_key
    {
        std::chrono::system_clock::time_point timestamp{};
        std::string symbol;
        std::string reason;
        bool operator==(const funding_identity_key&) const noexcept = default;
    };
    struct funding_identity_key_hash
    {
        std::size_t operator()(const funding_identity_key& key) const noexcept
        {
            const auto ticks = key.timestamp.time_since_epoch().count();
            using tick_type = std::remove_cv_t<decltype(ticks)>;
            std::size_t value = std::hash<tick_type>{}(ticks);
            const auto mix = [&](std::size_t next) {
                value ^= next + 0x9e3779b9 + (value << 6) + (value >> 2);
            };
            mix(std::hash<std::string>{}(key.symbol));
            mix(std::hash<std::string>{}(key.reason));
            return value;
        }
    };
    struct funding_identity
    {
        double qty_change = 0.0;
        double cash_delta = 0.0;
    };
    std::unordered_map<funding_identity_key, funding_identity,
                       funding_identity_key_hash> admitted_funding_identities_;
    bool funding_reconciliation_failed_ = false;

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
    // Process-lifetime economic identity ledger. Stable (order_id, fill_id)
    // pairs are authoritative: exact replay is a no-op and any contradictory
    // payload is rejected before economic state mutation. Unlike the old
    // eight-slot ring, reconnect replay cannot age an identity out.
    std::unordered_map<fill_identity_key, fill_identity,
                       fill_identity_key_hash> admitted_fill_identities_;
    // Venue execution ids survive reconnect/replay even when an adapter
    // assigns a new local fill id. The key includes symbol because several
    // venues scope execution ids per instrument.
    std::unordered_map<std::string, fill_identity>
        admitted_venue_execution_identities_;
    bool fill_reconciliation_failed_ = false;
    std::size_t duplicate_fill_replays_ignored_ = 0;
    std::size_t conflicting_fill_replays_rejected_ = 0;
    std::size_t missing_fill_identities_rejected_ = 0;
    std::size_t invalid_fill_payloads_rejected_ = 0;
    std::size_t unreconciled_funding_events_rejected_ = 0;
    std::size_t duplicate_funding_replays_ignored_ = 0;
    std::size_t conflicting_funding_replays_rejected_ = 0;
    std::size_t late_fill_events_rejected_ = 0;
    std::size_t late_funding_events_rejected_ = 0;
    std::size_t late_market_events_rejected_ = 0;
    std::size_t duplicate_market_marks_ignored_ = 0;
    std::size_t conflicting_market_marks_rejected_ = 0;
    bool contains_exploratory_execution_ = false;

    double total_holding_ms_ = 0.0;
    std::size_t holding_count_ = 0;
    std::size_t market_events_total_ = 0;
    std::optional<std::chrono::system_clock::time_point>
        first_time_accounting_timestamp_;
    std::optional<std::chrono::system_clock::time_point>
        last_time_accounting_timestamp_;
    std::chrono::duration<long double> exposure_duration_{};
    bool time_accounting_valid_ = true;
    std::optional<std::chrono::system_clock::time_point>
        previous_return_timestamp_;
    bool return_cadence_valid_ = true;

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
    // Updated on every risk equity mark, independently of the display curve.
    // Together these two values reproduce max_drawdown exactly even when the
    // bounded chart curve is decimated.
    double max_drawdown_peak_equity_ = 0.0;
    double max_drawdown_trough_equity_ = 0.0;

    std::size_t win_count_ = 0;
    double total_win_ = 0.0;
    double total_loss_ = 0.0;
    double largest_winner_ = 0.0;
    double largest_loser_ = 0.0;

    welford_state tick_to_trade_ns_;
    int64_t tick_to_trade_min_ns_ = 0;
    int64_t tick_to_trade_max_ns_ = 0;
};
