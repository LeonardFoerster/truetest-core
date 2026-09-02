#include "analytics.h"
#include "report_generator.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
std::optional<double> finite_scaled_benchmark_equity(
    double initial_equity, double price, double first_price) noexcept
{
    if (!(initial_equity > 0.0) || !(price > 0.0)
        || !(first_price > 0.0) || !std::isfinite(initial_equity)
        || !std::isfinite(price) || !std::isfinite(first_price))
        return std::nullopt;
    const long double value = static_cast<long double>(initial_equity)
        * static_cast<long double>(price)
        / static_cast<long double>(first_price);
    constexpr long double limit =
        static_cast<long double>(std::numeric_limits<double>::max());
    if (!std::isfinite(value) || std::abs(value) > limit)
        return std::nullopt;
    return static_cast<double>(value);
}

std::optional<double> finite_benchmark_return(
    double price, double first_price) noexcept
{
    if (!(price > 0.0) || !(first_price > 0.0)
        || !std::isfinite(price) || !std::isfinite(first_price))
        return std::nullopt;
    const long double value = static_cast<long double>(price)
        / static_cast<long double>(first_price) - 1.0L;
    constexpr long double limit =
        static_cast<long double>(std::numeric_limits<double>::max());
    if (!std::isfinite(value) || std::abs(value) > limit)
        return std::nullopt;
    return static_cast<double>(value);
}

std::optional<double> finite_double(long double value) noexcept
{
    constexpr long double limit =
        static_cast<long double>(std::numeric_limits<double>::max());
    if (!std::isfinite(value) || std::abs(value) > limit)
        return std::nullopt;
    return static_cast<double>(value);
}

long double epoch_seconds(
    std::chrono::system_clock::time_point timestamp) noexcept
{
    return std::chrono::duration<long double>(
        timestamp.time_since_epoch()).count();
}

static const bool kAnalyticsPnlDebug = [] {
    const char* v = std::getenv("TT_ANALYTICS_PNL_DEBUG");
    return v && (*v == '1' || *v == 't' || *v == 'T');
}();
} // namespace

Analytics::Analytics(double initial_cash, std::size_t rolling_window, double risk_free_rate,
                     std::size_t periods_per_year, std::size_t max_equity_points)
    : initial_cash_(initial_cash), cash_(initial_cash),
      rolling_window_(rolling_window), risk_free_rate_(risk_free_rate),
      periods_per_year_(periods_per_year > 0 ? periods_per_year : 525600),
      max_equity_points_(max_equity_points > 4 ? max_equity_points : 4),
      prev_equity_(initial_cash), peak_equity_(initial_cash),
      max_drawdown_peak_equity_(initial_cash),
      max_drawdown_trough_equity_(initial_cash)
{
    if (!(initial_cash > 0.0) || !std::isfinite(initial_cash)
        || !std::isfinite(risk_free_rate))
        throw std::invalid_argument(
            "analytics initial cash must be finite and positive and risk-free rate finite");
}

void Analytics::reserve_hint(std::size_t expected_bars)
{
    // Cap equity curves at the decimation ceiling; returns vectors take
    // the raw hint - they're doubles, 8 bytes each, so even 10M entries
    // is 80 MB, acceptable for backtests at that scale.
    const std::size_t curve_cap = std::min(expected_bars, max_equity_points_);
    equity_curve_.reserve(curve_cap);
    benchmark_curve_.reserve(curve_cap);
    strategy_returns_.reserve(expected_bars);
    benchmark_returns_.reserve(expected_bars);
    admitted_fill_identities_.reserve(expected_bars);
    admitted_venue_execution_identities_.reserve(expected_bars);
    admitted_funding_identities_.reserve(expected_bars);
}

// Phase A (MC object reuse): reset to initial constructed state.
void Analytics::reset(double initial_cash)
{
    if (!(initial_cash > 0.0) || !std::isfinite(initial_cash))
        throw std::invalid_argument(
            "analytics initial cash must be finite and positive");
    initial_cash_ = initial_cash;
    cash_ = initial_cash;
    open_positions_.clear();
    symbol_last_prices_.clear();
    symbol_market_states_.clear();
    varying_market_symbols_ = 0;
    symbol_spread_bps_.clear();
    benchmark_path_valid_ = true;
    benchmark_history_complete_ = true;
    symbol_funding_8h_rates_.clear();
    benchmark_symbol_.clear();
    benchmark_selection_timestamp_.reset();
    last_portfolio_clock_timestamp_.reset();
    portfolio_mark_cycle_max_timestamp_.reset();
    portfolio_time_series_valid_ = true;
    portfolio_time_series_failure_ = portfolio_time_series_failure::none;
    ambiguous_portfolio_mark_sequences_rejected_ = 0;

    equity_stride_ = 1;
    equity_counter_ = 0;
    bench_stride_ = 1;
    bench_counter_ = 0;

    equity_curve_.clear();
    benchmark_curve_.clear();
    strategy_returns_.clear();
    benchmark_returns_.clear();

    rolling_returns_.clear();
    prev_equity_ = initial_cash;
    peak_equity_ = initial_cash;

    order_prices_.clear();
    order_strategies_.clear();

    trades_.clear();
    trade_returns_.clear();
    contains_exploratory_execution_ = false;
    round_trip_count_ = 0;
    realized_leg_pnl_ = 0.0;
    gross_realized_pnl_ = 0.0;
    total_commission_ = 0.0;
    fill_reconciliation_failed_ = false;
    bankrupt_ = false;
    bankrupt_equity_ = 0.0;
    exit_lifecycle_ = exit_lifecycle_counts{};


    last_equity_ = 0.0;
    realized_vol_1h_ = 0.0;
    current_spread_bps_ = 0.0;
    current_funding_8h_rate_ = 0.0;
    funding_rate_known_ = false;
    external_worst_funding_8h_rate_.reset();
    funding_reconciliation_failed_ = false;

    total_funding_pnl_ = 0.0;
    total_slippage_ = 0.0;
    total_slippage_signed_ = 0.0;
    total_adverse_slippage_ = 0.0;
    total_favorable_slippage_ = 0.0;
    slippage_count_ = 0;
    adverse_count_ = 0;
    favorable_count_ = 0;
    total_orders_ = 0;
    total_fills_ = 0;
    admitted_fill_identities_.clear();
    admitted_venue_execution_identities_.clear();
    duplicate_fill_replays_ignored_ = 0;
    conflicting_fill_replays_rejected_ = 0;
    missing_fill_identities_rejected_ = 0;
    invalid_fill_payloads_rejected_ = 0;
    unreconciled_funding_events_rejected_ = 0;
    admitted_funding_identities_.clear();
    duplicate_funding_replays_ignored_ = 0;
    conflicting_funding_replays_rejected_ = 0;
    late_fill_events_rejected_ = 0;
    late_funding_events_rejected_ = 0;
    late_market_events_rejected_ = 0;
    duplicate_market_marks_ignored_ = 0;
    conflicting_market_marks_rejected_ = 0;
    soft_post_fill_breaches_ = 0;
    data_rows_rejected_ = 0;

    total_holding_ms_ = 0.0;
    holding_count_ = 0;
    market_events_total_ = 0;
    first_time_accounting_timestamp_.reset();
    last_time_accounting_timestamp_.reset();
    exposure_duration_ = std::chrono::duration<long double>::zero();
    time_accounting_valid_ = true;
    previous_return_timestamp_.reset();
    return_cadence_valid_ = true;

    prev_bh_equity_ = initial_cash;

    per_symbol_.clear();
    per_strategy_.clear();

    return_stats_.reset();
    downside_sq_sum_ = 0.0;

    peak_equity_ = initial_cash;
    max_drawdown_ = 0.0;
    max_drawdown_peak_equity_ = initial_cash;
    max_drawdown_trough_equity_ = initial_cash;

    win_count_ = 0;
    total_win_ = 0.0;
    total_loss_ = 0.0;
    largest_winner_ = 0.0;
    largest_loser_ = 0.0;

    tick_to_trade_ns_.reset();
    tick_to_trade_min_ns_ = 0;
    tick_to_trade_max_ns_ = 0;

}

void Analytics::record_equity_point(std::vector<equity_point>& curve,
                                    std::size_t& stride,
                                    std::size_t& counter,
                                    const equity_point& pt)
{
    ++counter;
    if (counter % stride != 0) return;
    curve.push_back(pt);
    if (curve.size() > max_equity_points_)
    {
        std::vector<equity_point> reduced;
        reduced.reserve(curve.size() / 2 + 1);
        for (std::size_t i = 0; i < curve.size(); i += 2)
            reduced.push_back(curve[i]);
        curve = std::move(reduced);
        stride *= 2;
    }
}

void Analytics::on_event(const event_pointer& ev)
{
    switch (ev->get_type())
    {
        case event_type::market:
            on_market(*std::static_pointer_cast<market_event>(ev));
            break;
        case event_type::tick:
            on_tick(*std::static_pointer_cast<tick_event>(ev));
            break;
        case event_type::order:
            on_order(*std::static_pointer_cast<order_event>(ev));
            break;
        case event_type::fill:
            on_fill(*std::static_pointer_cast<fill_event>(ev));
            break;
        case event_type::signal:
            break;
        case event_type::l2_snapshot:
            on_l2_snapshot(*std::static_pointer_cast<l2_snapshot_event>(ev));
            break;
        case event_type::l2_update:
            on_l2_update(*std::static_pointer_cast<l2_update_event>(ev));
            break;
        case event_type::cancel:
        case event_type::amend:
        case event_type::rejection:
            break;
        case event_type::funding:
            on_funding(*std::static_pointer_cast<funding_event>(ev));
            break;
    }
}

void Analytics::on_funding(const funding_event& fe)
{
    constexpr long double double_max =
        static_cast<long double>(std::numeric_limits<double>::max());
    const double cash_delta = fe.get_cash_delta();
    const double qty_change = fe.get_qty_change();
    const std::string_view funding_symbol = fe.get_symbol();
    const std::string_view reason = fe.get_reason();
    const auto latch_reconciliation_failure = [&]() {
        ++unreconciled_funding_events_rejected_;
        funding_reconciliation_failed_ = true;
        current_funding_8h_rate_ = std::numeric_limits<double>::max();
        funding_rate_known_ = true;
    };

    if (funding_symbol.empty() || reason != "FUNDING_FEE"
        || fe.get_timestamp().time_since_epoch().count() <= 0
        || !std::isfinite(cash_delta) || cash_delta == 0.0
        || !std::isfinite(qty_change) || qty_change != 0.0)
    {
        latch_reconciliation_failure();
        return;
    }
    funding_identity_key identity_key{
        fe.get_timestamp(), std::string(funding_symbol), std::string(reason)};
    const auto prior = admitted_funding_identities_.find(identity_key);
    if (prior != admitted_funding_identities_.end())
    {
        if (prior->second.qty_change == qty_change
            && prior->second.cash_delta == cash_delta)
            ++duplicate_funding_replays_ignored_;
        else
        {
            ++conflicting_funding_replays_rejected_;
            funding_reconciliation_failed_ = true;
            current_funding_8h_rate_ = std::numeric_limits<double>::max();
            funding_rate_known_ = true;
        }
        return;
    }
    // Stable replay identity is evaluated before the global event clock.
    // An exact reconnect replay is not a new economic event and therefore
    // cannot make an otherwise monotonic run non-monotonic. A novel late
    // settlement is still rejected below and is not admitted to the ledger.
    if (last_time_accounting_timestamp_
        && fe.get_timestamp() < *last_time_accounting_timestamp_)
    {
        ++late_funding_events_rejected_;
        funding_reconciliation_failed_ = true;
        current_funding_8h_rate_ = std::numeric_limits<double>::max();
        funding_rate_known_ = true;
        time_accounting_valid_ = false;
        return;
    }

    long double total_notional = 0.0L;
    for (const auto& [_, pos] : open_positions_)
    {
        if (pos.symbol == funding_symbol)
            total_notional += static_cast<long double>(pos.qty)
                * static_cast<long double>(pos.last_price);
    }
    const long double projected_cash = static_cast<long double>(cash_)
        + static_cast<long double>(cash_delta);
    const long double projected_funding =
        static_cast<long double>(total_funding_pnl_)
        + static_cast<long double>(cash_delta);
    long double projected_equity = projected_cash;
    for (const auto& [_, pos] : open_positions_)
        projected_equity += static_cast<long double>(pos.qty)
            * static_cast<long double>(pos.last_price);
    if (!std::isfinite(projected_cash)
        || !std::isfinite(projected_funding)
        || !std::isfinite(projected_equity)
        || std::abs(projected_cash) > double_max
        || std::abs(projected_funding) > double_max
        || std::abs(projected_equity) > double_max)
    {
        latch_reconciliation_failure();
        return;
    }
    if (!std::isfinite(total_notional)
        || std::abs(total_notional) <= 1e-9L)
    {
        latch_reconciliation_failure();
        return;
    }

    if (!advance_time_accounting(fe.get_timestamp()))
        return;
    admitted_funding_identities_.try_emplace(
        std::move(identity_key),
        funding_identity{qty_change, cash_delta});
    // Phase 2.1 - funding cash deltas adjust our internal cash and equity curve.
    // This makes funding visible in reports, TUI sparkline, and risk_view().
    cash_ = static_cast<double>(projected_cash);
    total_funding_pnl_ = static_cast<double>(projected_funding);

    // R3: derive the realized 8h funding rate from the settlement itself.
    // The venue identity is funding_fee = -position_notional * rate, so
    // rate = -cash_delta / signed_position_notional. This is the only
    // funding-rate producer the repository has (a dedicated rate feed can
    // still override it through set_current_funding_rate_8h). Spot never
    // emits funding_event, so spot instruments never acquire perpetual
    // semantics through this path.
    // Linear scan over the (few) tracked symbols rather than constructing a
    // std::string key from the event's fixed-capacity symbol buffer.
    if (std::abs(total_notional) > 1e-9L)
    {
        // Risk consumes one scalar and currently has no position side. Feed
        // it the conservative absolute realized settlement rate so a short
        // paying funding cannot hide behind a negative market-rate sign.
        const long double realized_rate = std::abs(
            static_cast<long double>(cash_delta) / total_notional);
        const double rate = realized_rate <= double_max
            ? static_cast<double>(realized_rate)
            : std::numeric_limits<double>::max();
        if (std::isfinite(rate))
        {
            symbol_funding_8h_rates_[std::string(funding_symbol)] = rate;
            if (!funding_reconciliation_failed_)
            {
                current_funding_8h_rate_ = external_worst_funding_8h_rate_
                    .value_or(symbol_funding_8h_rates_.begin()->second);
                for (const auto& [_, candidate] : symbol_funding_8h_rates_)
                    current_funding_8h_rate_ = std::max(
                        current_funding_8h_rate_, candidate);
            }
            funding_rate_known_ = true;
        }
    }

    // Mirror the equity calculation from on_market
    double equity = cash_ + position_value();

    update_risk_equity(equity);

    // Record a point so the equity curve (and any downstream reports) shows the funding step
    record_equity_point(equity_curve_, equity_stride_, equity_counter_,
                        {fe.get_timestamp(), equity});
    // Funding is a cash settlement, not a market period. Retain it in the
    // reconciled account PnL and reset the market-return baseline so the next
    // price mark cannot mislabel an irregular settlement as a bar return.
    // This also keeps strategy and benchmark return vectors aligned.
    prev_equity_ = equity;
}

void Analytics::on_market(const market_event& m)
{
    const auto state_update = update_symbol_market_state(
        m.get_symbol(), m.get_close(), m.get_timestamp());
    if (state_update == market_state_update::rejected)
        return;
    set_symbol_price(m.get_symbol(), m.get_close());

    const bool have_open_position = any_position_open();
    if (have_open_position && !portfolio_time_series_valid_)
    {
        last_equity_ = cash_ + position_value();
        return;
    }
    const bool symbols_synchronized = have_open_position
        ? mark_open_positions_fresh(m.get_symbol(), m.get_timestamp())
            && portfolio_mark_cycle_complete()
        : m.get_symbol() == benchmark_symbol_;
    const auto portfolio_clock_timestamp = have_open_position
        ? portfolio_mark_cycle_max_timestamp_
        : std::optional{m.get_timestamp()};
    const bool is_portfolio_clock = symbols_synchronized
        && portfolio_clock_timestamp
        && (!last_portfolio_clock_timestamp_
            || *portfolio_clock_timestamp
                > *last_portfolio_clock_timestamp_);
    const double equity = cash_ + position_value();
    last_equity_ = equity;
    if (!have_open_position
        && state_update == market_state_update::benchmark_changed
        && last_portfolio_clock_timestamp_
        && *last_portfolio_clock_timestamp_ == m.get_timestamp())
    {
        const auto benchmark = symbol_market_states_.find(benchmark_symbol_);
        if (benchmark != symbol_market_states_.end()
            && benchmark->second.benchmark_first_price > 0.0)
        {
            const auto bh_equity_now = finite_scaled_benchmark_equity(
                initial_cash_, benchmark->second.benchmark_last_price,
                benchmark->second.benchmark_first_price);
            if (bh_equity_now)
            {
                if (market_events_total_ > 1 && prev_bh_equity_ > 0.0)
                {
                    const auto bh_ret = finite_benchmark_return(
                        *bh_equity_now, prev_bh_equity_);
                    if (bh_ret)
                        benchmark_returns_.push_back(*bh_ret);
                    else
                        benchmark_path_valid_ = false;
                }
                record_equity_point(benchmark_curve_, bench_stride_,
                                    bench_counter_,
                                    {m.get_timestamp(), *bh_equity_now});
                prev_bh_equity_ = *bh_equity_now;
            }
            else
                benchmark_path_valid_ = false;
        }
    }
    if (!is_portfolio_clock)
    {
        if (have_open_position && symbols_synchronized)
            clear_portfolio_mark_cycle();
        return;
    }
    const auto clock_timestamp = *portfolio_clock_timestamp;
    if (!advance_time_accounting(clock_timestamp))
    {
        clear_portfolio_mark_cycle();
        return;
    }
    last_portfolio_clock_timestamp_ = clock_timestamp;

    market_events_total_++;

    record_equity_point(equity_curve_, equity_stride_, equity_counter_,
                        {clock_timestamp, equity});

    double bh_equity_now = 0.0;
    bool have_bh_now = false;
    const auto benchmark = symbol_market_states_.find(benchmark_symbol_);
    if (benchmark != symbol_market_states_.end()
        && benchmark->second.benchmark_first_price > 0.0
        && benchmark->second.benchmark_last_price > 0.0)
    {
        const auto candidate = finite_scaled_benchmark_equity(
            initial_cash_, benchmark->second.benchmark_last_price,
            benchmark->second.benchmark_first_price);
        if (candidate)
        {
            bh_equity_now = *candidate;
            have_bh_now = true;
        }
        else
            benchmark_path_valid_ = false;
    }

    if (market_events_total_ > 1)
    {
        record_equity_return(equity, clock_timestamp);
        if (have_bh_now && prev_bh_equity_ > 0.0)
        {
            const auto bh_ret = finite_benchmark_return(
                bh_equity_now, prev_bh_equity_);
            if (bh_ret)
                benchmark_returns_.push_back(*bh_ret);
            else
                benchmark_path_valid_ = false;
        }
    }
    else
    {
        prev_equity_ = equity;
        previous_return_timestamp_ = clock_timestamp;
    }
    if (have_bh_now)
    {
        record_equity_point(benchmark_curve_, bench_stride_, bench_counter_,
                            {clock_timestamp, bh_equity_now});
        prev_bh_equity_ = bh_equity_now;
    }

    update_risk_equity(equity);
    clear_portfolio_mark_cycle();
}

void Analytics::on_tick(const tick_event& t)
{
    const auto state_update = update_symbol_market_state(
        t.get_symbol(), t.get_price(), t.get_timestamp(),
        /*allow_same_timestamp_change=*/true,
        /*eligible_for_benchmark=*/false);
    if (state_update == market_state_update::rejected)
        return;
    set_symbol_price(t.get_symbol(), t.get_price());
    const bool marked_open_symbol =
        mark_open_positions_fresh(t.get_symbol(), t.get_timestamp());
    const double equity = cash_ + position_value();
    if (!portfolio_time_series_valid_)
    {
        last_equity_ = equity;
        return;
    }
    if (marked_open_symbol && portfolio_mark_cycle_complete())
    {
        update_risk_equity(equity);
        clear_portfolio_mark_cycle();
    }
    else
        last_equity_ = equity;
}

Analytics::market_state_update Analytics::update_symbol_market_state(
    const std::string& symbol, double price,
    std::chrono::system_clock::time_point timestamp,
    bool allow_same_timestamp_change,
    bool eligible_for_benchmark)
{
    if (symbol.empty() || !(price > 0.0) || !std::isfinite(price))
        return market_state_update::rejected;
    if (!mark_keeps_equity_finite(symbol, price))
        return market_state_update::rejected;

    auto state_it = symbol_market_states_.find(symbol);
    const auto reject_late_or_conflicting = [&, this](
        portfolio_time_series_failure failure) {
        const bool economic_history =
            has_economic_history_for_symbol(symbol);
        if (economic_history || has_open_position_for_symbol(symbol)
            || symbol == benchmark_symbol_)
            invalidate_portfolio_time_series(failure);
        if (symbol == benchmark_symbol_)
            benchmark_path_valid_ = false;
    };
    if (state_it != symbol_market_states_.end()
        && timestamp == state_it->second.last_timestamp)
    {
        if (price == state_it->second.last_price)
        {
            ++duplicate_market_marks_ignored_;
            return market_state_update::rejected;
        }
        if (!allow_same_timestamp_change)
        {
            ++conflicting_market_marks_rejected_;
            reject_late_or_conflicting(
                portfolio_time_series_failure::conflicting_market_mark);
            return market_state_update::rejected;
        }
    }
    if (last_time_accounting_timestamp_
        && timestamp < *last_time_accounting_timestamp_)
    {
        ++late_market_events_rejected_;
        reject_late_or_conflicting(
            portfolio_time_series_failure::late_market_event);
        return market_state_update::rejected;
    }

    if (state_it != symbol_market_states_.end())
    {
        if (timestamp < state_it->second.last_timestamp)
        {
            ++late_market_events_rejected_;
            reject_late_or_conflicting(
                portfolio_time_series_failure::late_market_event);
            return market_state_update::rejected;
        }
    }

    auto& state = state_it == symbol_market_states_.end()
        ? symbol_market_states_.try_emplace(symbol).first->second
        : state_it->second;
    bool benchmark_changed = false;
    if (!state.has_mark)
    {
        state.first_price = price;
        state.first_timestamp = timestamp;
        state.has_mark = true;
    }
    if (state.last_mid > 0.0 && price > 0.0)
    {
        constexpr double alpha = 0.02;
        const double ret = std::log(price) - std::log(state.last_mid);
        state.realized_vol_1h = alpha * std::abs(ret)
            + (1.0 - alpha) * state.realized_vol_1h;
    }
    bool first_variation = false;
    if (eligible_for_benchmark)
    {
        if (!state.has_benchmark_mark)
        {
            state.has_benchmark_mark = true;
            state.benchmark_first_price = price;
            state.benchmark_last_price = price;
            state.benchmark_first_timestamp = timestamp;
            state.benchmark_last_timestamp = timestamp;
            state.benchmark_mark_count = 1;
            if (trades_.empty() && varying_market_symbols_ == 0
                && (!benchmark_selection_timestamp_
                    || (timestamp == *benchmark_selection_timestamp_
                        && (benchmark_symbol_.empty()
                            || symbol < benchmark_symbol_))))
            {
                benchmark_symbol_ = symbol;
                benchmark_selection_timestamp_ = timestamp;
                benchmark_curve_.clear();
                benchmark_returns_.clear();
                bench_stride_ = 1;
                bench_counter_ = 0;
                prev_bh_equity_ = 0.0;
                benchmark_changed = true;
            }
        }
        else
        {
            ++state.benchmark_mark_count;
            first_variation = !state.benchmark_has_varied
                && price != state.benchmark_first_price;
            if (first_variation)
            {
                state.benchmark_has_varied = true;
                ++varying_market_symbols_;
            }
            state.benchmark_last_price = price;
            state.benchmark_last_timestamp = timestamp;
        }
    }
    state.last_mid = price;
    state.last_price = price;
    state.last_timestamp = timestamp;

    // A constant symbol that happened to arrive first must not remain the
    // benchmark once another symbol becomes the sole historically varying
    // path. Select at the first causal observation of that variation and
    // seed the retained curve from the symbol's already-known first mark.
    // If a second symbol later varies, snapshot() rejects the ambiguity.
    if (eligible_for_benchmark && trades_.empty() && first_variation
        && varying_market_symbols_ == 1 && symbol != benchmark_symbol_)
    {
        benchmark_symbol_ = symbol;
        benchmark_selection_timestamp_ = timestamp;
        benchmark_curve_.clear();
        benchmark_returns_.clear();
        bench_stride_ = 1;
        bench_counter_ = 0;
        prev_bh_equity_ = initial_cash_;
        benchmark_history_complete_ = state.benchmark_mark_count == 2;
        if (benchmark_history_complete_)
            record_equity_point(
                benchmark_curve_, bench_stride_, bench_counter_,
                {state.benchmark_first_timestamp, initial_cash_});
        else
            benchmark_path_valid_ = false;
        benchmark_changed = true;
    }

    realized_vol_1h_ = 0.0;
    for (const auto& [_, candidate] : symbol_market_states_)
        realized_vol_1h_ = std::max(
            realized_vol_1h_, candidate.realized_vol_1h);

    return benchmark_changed
        ? market_state_update::benchmark_changed
        : market_state_update::accepted;
}

bool Analytics::mark_keeps_equity_finite(
    std::string_view symbol, double price) const noexcept
{
    long double projected = static_cast<long double>(cash_);
    for (const auto& [_, position] : open_positions_)
    {
        if (std::abs(position.qty) <= 1e-12)
            continue;
        const double mark = position.symbol == symbol
            ? price : position.last_price;
        projected += static_cast<long double>(position.qty)
            * static_cast<long double>(mark);
    }
    constexpr long double double_max =
        static_cast<long double>(std::numeric_limits<double>::max());
    return std::isfinite(projected) && std::abs(projected) <= double_max;
}

bool Analytics::mark_open_positions_fresh(
    std::string_view symbol,
    std::chrono::system_clock::time_point timestamp) noexcept
{
    bool found = false;
    bool repeated_before_cycle_completion = false;
    for (const auto& [_, position] : open_positions_)
    {
        if (std::abs(position.qty) > 1e-12
            && position.symbol == symbol)
        {
            found = true;
            repeated_before_cycle_completion =
                repeated_before_cycle_completion
                || position.fresh_for_portfolio_clock;
        }
    }
    if (repeated_before_cycle_completion)
    {
        invalidate_portfolio_time_series(
            portfolio_time_series_failure::ambiguous_mark_order);
        return false;
    }
    for (auto& [_, position] : open_positions_)
    {
        if (std::abs(position.qty) > 1e-12 && position.symbol == symbol)
        {
            position.fresh_for_portfolio_clock = true;
            found = true;
        }
    }
    if (found && (!portfolio_mark_cycle_max_timestamp_
                  || timestamp > *portfolio_mark_cycle_max_timestamp_))
        portfolio_mark_cycle_max_timestamp_ = timestamp;
    return found;
}

std::string_view Analytics::portfolio_time_series_reason(
    portfolio_time_series_failure failure) noexcept
{
    switch (failure)
    {
        case portfolio_time_series_failure::none:
            return "causal_complete_mark_cycles";
        case portfolio_time_series_failure::ambiguous_mark_order:
            return "ambiguous_cross_symbol_arrival_without_watermark";
        case portfolio_time_series_failure::non_finite_return:
            return "non_finite_portfolio_return";
        case portfolio_time_series_failure::late_market_event:
            return "late_market_event_rejected";
        case portfolio_time_series_failure::conflicting_market_mark:
            return "conflicting_same_timestamp_market_mark";
    }
    return "unknown_portfolio_time_series_failure";
}

void Analytics::invalidate_portfolio_time_series(
    portfolio_time_series_failure failure) noexcept
{
    if (!portfolio_time_series_valid_)
        return;
    portfolio_time_series_valid_ = false;
    portfolio_time_series_failure_ = failure;
    if (failure == portfolio_time_series_failure::ambiguous_mark_order)
        ++ambiguous_portfolio_mark_sequences_rejected_;
    time_accounting_valid_ = false;
    return_cadence_valid_ = false;
    max_drawdown_ = 1.0;
    max_drawdown_peak_equity_ = peak_equity_;
    max_drawdown_trough_equity_ = 0.0;
    clear_risk_mark_cycle();
    clear_portfolio_mark_cycle();
}

bool Analytics::portfolio_mark_cycle_complete() const noexcept
{
    bool found_open = false;
    for (const auto& [_, position] : open_positions_)
    {
        if (std::abs(position.qty) <= 1e-12)
            continue;
        found_open = true;
        if (!position.fresh_for_portfolio_clock)
            return false;
    }
    return found_open;
}

void Analytics::on_order(const order_event& o)
{
    order_prices_[o.get_order_id()] = o.get_price();
    if (!o.get_strategy_name().empty())
        order_strategies_[o.get_order_id()] = o.get_strategy_name();
    total_orders_++;
}

void Analytics::on_fill(const fill_event& f)
{
    const double filled_qty = f.get_filled_quantity();
    const double fill_price = f.get_fill_price();
    const double commission = f.get_commission();
    const double remaining_qty = f.get_remaining_qty();
    const auto& provenance = f.get_provenance();
    const bool provenance_valid =
        std::isfinite(provenance.intended_price)
        && provenance.intended_price >= 0.0
        && std::isfinite(provenance.reference_price)
        && provenance.reference_price >= 0.0
        && std::isfinite(provenance.modeled_spread_bps)
        && std::isfinite(provenance.modeled_impact_bps)
        && std::isfinite(provenance.fill_probability)
        && provenance.fill_probability >= 0.0
        && provenance.fill_probability <= 1.0
        && provenance.modeled_latency.count() >= 0
        && (provenance.reference_timestamp.time_since_epoch().count() == 0
            || provenance.reference_timestamp <= f.get_timestamp());
    if (!(filled_qty > 0.0) || !std::isfinite(filled_qty)
        || !(fill_price > 0.0) || !std::isfinite(fill_price)
        || !std::isfinite(commission)
        || !(remaining_qty >= 0.0) || !std::isfinite(remaining_qty)
        || f.get_symbol().empty()
        || (commission != 0.0 && f.get_commission_currency().empty())
        || !std::isfinite(filled_qty * fill_price)
        || !provenance_valid)
    {
        ++invalid_fill_payloads_rejected_;
        fill_reconciliation_failed_ = true;
        return;
    }

    auto identity = make_physical_fill_candidate(f);
    switch (classify_physical_fill(identity))
    {
        case fill_admission::missing_identity:
            ++missing_fill_identities_rejected_;
            fill_reconciliation_failed_ = true;
            return;
        case fill_admission::conflicting_replay:
            ++conflicting_fill_replays_rejected_;
            fill_reconciliation_failed_ = true;
            return;
        case fill_admission::duplicate:
            ++duplicate_fill_replays_ignored_;
            // Backfill an unseen local or venue alias without applying the
            // already admitted economic fill again.
            remember_physical_fill(std::move(identity));
            return;
        case fill_admission::accepted:
            break;
    }

    const std::string* strat_name = &f.get_strategy_name();
    const auto strat_it = order_strategies_.find(f.get_order_id());
    if (strat_it != order_strategies_.end())
        strat_name = &strat_it->second;
    if (!fill_keeps_accounting_finite(f, *strat_name))
    {
        ++invalid_fill_payloads_rejected_;
        fill_reconciliation_failed_ = true;
        return;
    }
    // Temporal validation is part of admission. Do not insert a stable
    // identity and only then discover that the fill cannot be applied.
    if (last_time_accounting_timestamp_
        && f.get_timestamp() < *last_time_accounting_timestamp_)
    {
        ++late_fill_events_rejected_;
        fill_reconciliation_failed_ = true;
        time_accounting_valid_ = false;
        return;
    }

    // Only a novel fill that passed payload, projected-accounting, and time
    // validation may enter the process-lifetime identity ledger.
    remember_physical_fill(std::move(identity));
    if (!advance_time_accounting(f.get_timestamp()))
    {
        fill_reconciliation_failed_ = true;
        return;
    }

    // Lock the benchmark history to the first economic symbol, not to an
    // unrelated lexicographically selected market-data symbol. The curve is
    // seeded from that symbol's causal mark history at ingress. A later
    // cross-symbol economic history is reported unsupported rather than
    // silently relabeling the already accumulated benchmark curve.
    if (trades_.empty())
    {
        const bool retain_causal_path = benchmark_symbol_ == f.get_symbol()
            && !benchmark_curve_.empty() && benchmark_path_valid_;
        benchmark_symbol_ = f.get_symbol();
        benchmark_selection_timestamp_ = f.get_timestamp();
        if (!retain_causal_path)
        {
            benchmark_curve_.clear();
            benchmark_returns_.clear();
            bench_stride_ = 1;
            bench_counter_ = 0;
            prev_bh_equity_ = 0.0;
            benchmark_path_valid_ = true;
            benchmark_history_complete_ = true;
        }
        const auto state = symbol_market_states_.find(f.get_symbol());
        if (!retain_causal_path && state != symbol_market_states_.end()
            && state->second.has_benchmark_mark
            && state->second.benchmark_first_price > 0.0
            && state->second.benchmark_last_price > 0.0)
        {
            if (state->second.benchmark_has_varied)
            {
                benchmark_path_valid_ = false;
                benchmark_history_complete_ = false;
            }
            const auto benchmark_equity = finite_scaled_benchmark_equity(
                initial_cash_, state->second.benchmark_last_price,
                state->second.benchmark_first_price);
            if (benchmark_equity && benchmark_history_complete_)
            {
                record_equity_point(benchmark_curve_, bench_stride_,
                                    bench_counter_,
                                    {state->second.benchmark_first_timestamp,
                                     initial_cash_});
                if (state->second.benchmark_last_timestamp
                    != state->second.benchmark_first_timestamp)
                    record_equity_point(
                        benchmark_curve_, bench_stride_, bench_counter_,
                        {state->second.benchmark_last_timestamp,
                         *benchmark_equity});
                prev_bh_equity_ = *benchmark_equity;
            }
            else if (!benchmark_equity)
                benchmark_path_valid_ = false;
        }
    }
    total_fills_++;

    if (f.get_latency_ns() > 0)
    {
        int64_t lat = f.get_latency_ns();
        tick_to_trade_ns_.update(static_cast<double>(lat));
        if (tick_to_trade_ns_.n == 1 || lat < tick_to_trade_min_ns_)
            tick_to_trade_min_ns_ = lat;
        if (lat > tick_to_trade_max_ns_)
            tick_to_trade_max_ns_ = lat;
    }

    double intended = provenance.intended_price;
    auto it = order_prices_.find(f.get_order_id());
    if (!(intended > 0.0) && it != order_prices_.end())
        intended = it->second;
    if (intended > 0.0)
    {
        double raw = f.get_fill_price() - intended;
        double side_sign = (f.get_side() == order_side::buy) ? +1.0 : -1.0;
        double signed_slip = raw * side_sign;
        total_slippage_ += std::abs(raw);
        total_slippage_signed_ += signed_slip;
        if (signed_slip > 0.0) { total_adverse_slippage_ += signed_slip; adverse_count_++; }
        else if (signed_slip < 0.0) { total_favorable_slippage_ += -signed_slip; favorable_count_++; }
        slippage_count_++;
    }

    trade_record rec;
    rec.order_id = f.get_order_id();
    rec.side = f.get_side();
    rec.quantity = f.get_filled_quantity();
    rec.fill_price = f.get_fill_price();
    rec.commission = f.get_commission();
    rec.commission_currency = f.get_commission_currency();
    rec.intended_price = intended;
    rec.reference_price = provenance.reference_price;
    rec.reference_timestamp = provenance.reference_timestamp;
    rec.modeled_spread_bps = provenance.modeled_spread_bps;
    rec.modeled_impact_bps = provenance.modeled_impact_bps;
    rec.fill_probability = provenance.fill_probability;
    rec.modeled_latency = provenance.modeled_latency;
    rec.execution_model = provenance.model;
    rec.execution_reason = provenance.reason;
    rec.execution_is_exploratory = provenance.exploratory;
    rec.fill_id = f.get_fill_id();
    rec.venue_execution_id = f.get_venue_execution_id();
    rec.timestamp = f.get_timestamp();
    rec.pnl = 0.0;
    rec.symbol = f.get_symbol();
    rec.strategy_name = *strat_name;
    contains_exploratory_execution_ =
        contains_exploratory_execution_ || provenance.exploratory;

    total_commission_ += commission;
    const double side_sign = (f.get_side() == order_side::buy) ? +1.0 : -1.0;
    double qty_left = filled_qty;

    strategy_symbol_key pos_key{*strat_name, f.get_symbol()};
    auto& pos = open_positions_[pos_key];
    pos.symbol = f.get_symbol();
    pos.strategy_name = *strat_name;
    double valuation_price = fill_price;
    const auto known_mark = symbol_market_states_.find(f.get_symbol());
    const bool has_known_market_mark =
        known_mark != symbol_market_states_.end()
        && known_mark->second.has_mark
        && known_mark->second.last_timestamp <= f.get_timestamp()
        && known_mark->second.last_price > 0.0;
    if (has_known_market_mark)
        valuation_price = known_mark->second.last_price;
    pos.last_price = valuation_price;
    pos.has_market_mark = pos.has_market_mark || has_known_market_mark;
    symbol_last_prices_[f.get_symbol()] = valuation_price;

    if (std::abs(pos.qty) > 1e-12 && pos.qty * side_sign < 0.0)
    {
        double pos_sign = (pos.qty > 0.0) ? 1.0 : -1.0;
        double prev_abs = std::abs(pos.qty);
        double close_qty = std::min(prev_abs, qty_left);

        double gross = (fill_price - pos.avg_entry) * close_qty * pos_sign;

        double close_comm = commission * (close_qty / filled_qty);
        double open_comm_share = pos.open_commission * (close_qty / prev_abs);
        double pnl = gross - close_comm - open_comm_share;

        if (kAnalyticsPnlDebug) {
            std::fprintf(stderr,
                "[ANALYTICS_PNL_CLOSE] sym=%s side=%s fill_px=%.8f filled_qty=%.6f comm=%.6f "
                "pos_qty=%.6f avg_entry=%.8f open_comm_acc=%.6f "
                "close_qty=%.6f gross=%.8f close_c=%.6f open_share=%.6f pnl=%.8f\n",
                f.get_symbol().c_str(),
                (f.get_side() == order_side::buy ? "BUY" : "SELL"),
                fill_price, filled_qty, commission,
                pos.qty, pos.avg_entry, pos.open_commission,
                close_qty, gross, close_comm, open_comm_share, pnl);
        }

        pos.open_commission -= open_comm_share;

        cash_ += -side_sign * close_qty * fill_price - close_comm;

        pos.qty += side_sign * close_qty;
        qty_left -= close_qty;
        if (std::abs(pos.qty) < 1e-12)
        {
            pos.qty = 0.0;
            pos.avg_entry = 0.0;
            pos.open_commission = 0.0;
        }
        // NOTE: the round-trip settlement below reads pos.qty, so it must
        // stay after this flat-normalisation.


        rec.pnl = pnl;
        trade_returns_.push_back(pnl);
        gross_realized_pnl_ += gross;
        realized_leg_pnl_ += pnl;

        // F-09b: PnL lands per closing leg (cash and realized PnL must stay
        // exact against every partial), but a *trade* is the whole round
        // trip. Accumulate here and settle the win/loss statistics only when
        // the position actually returns to flat — otherwise a single exit
        // walking four book levels counts as four trades and drags every
        // per-trade average with it.
        pos.round_trip_pnl += pnl;
        ++pos.round_trip_legs;

        // per-symbol / per-strategy PnL is exact per leg; their trade counts
        // settle with the round trip below.
        per_symbol_[f.get_symbol()].total_pnl += pnl;
        if (!strat_name->empty())
            per_strategy_[*strat_name].total_pnl += pnl;

        if (std::abs(pos.qty) <= 1e-12)
        {
            const double trip_pnl = pos.round_trip_pnl;
            ++round_trip_count_;

            if (trip_pnl > 0.0)
            {
                win_count_++;
                total_win_ += trip_pnl;
                if (trip_pnl > largest_winner_) largest_winner_ = trip_pnl;
            }
            else
            {
                total_loss_ += std::abs(trip_pnl);
                if (trip_pnl < largest_loser_) largest_loser_ = trip_pnl;
            }

            {
                auto& sa = per_symbol_[f.get_symbol()];
                sa.trade_count++;
                if (trip_pnl > 0.0) { sa.win_count++; sa.total_win += trip_pnl; }
                else { sa.total_loss += std::abs(trip_pnl); }
            }
            if (!strat_name->empty())
            {
                auto& sa = per_strategy_[*strat_name];
                sa.trade_count++;
                if (trip_pnl > 0.0) { sa.win_count++; sa.total_win += trip_pnl; }
                else { sa.total_loss += std::abs(trip_pnl); }
            }

            // One holding-period sample per round trip, entry to final exit.
            const long double hold_ms =
                (epoch_seconds(f.get_timestamp())
                 - epoch_seconds(pos.entry_time)) * 1000.0L;
            if (hold_ms >= 0.0L
                && hold_ms <= static_cast<long double>(
                    std::numeric_limits<double>::max()))
                total_holding_ms_ += static_cast<double>(hold_ms);
            else
                time_accounting_valid_ = false;
            holding_count_++;

            pos.round_trip_pnl = 0.0;
            pos.round_trip_legs = 0;
        }
    }


    if (qty_left > 1e-12)
    {
        double open_comm = commission * (qty_left / filled_qty);
        double prev_abs = std::abs(pos.qty);

        pos.avg_entry = (pos.avg_entry * prev_abs + fill_price * qty_left)
                      / (prev_abs + qty_left);
        pos.qty += side_sign * qty_left;
        pos.open_commission += open_comm;

        if (kAnalyticsPnlDebug && prev_abs < 1e-12) {
            std::fprintf(stderr,
                "[ANALYTICS_PNL_OPEN] sym=%s side=%s fill_px=%.8f qty=%.6f comm=%.6f "
                "avg_entry_set=%.8f open_comm_acc=%.6f\n",
                f.get_symbol().c_str(),
                (f.get_side() == order_side::buy ? "BUY" : "SELL"),
                fill_price, qty_left, open_comm, pos.avg_entry, pos.open_commission);
        }

        if (prev_abs < 1e-12)
            pos.entry_time = f.get_timestamp();

        cash_ -= side_sign * qty_left * fill_price + open_comm;
    }

    trades_.push_back(rec);

    // Fill cash/position mutation is immediately visible to post-fill risk.
    // Do not append an equity-curve point here (curve cadence is market-time),
    // but never leave risk_view() at the pre-fill mark.
    const double equity = cash_ + position_value();
    update_risk_equity(equity);
    clear_risk_mark_cycle();
    clear_portfolio_mark_cycle();
}

void Analytics::record_equity_return(
    double equity,
    std::chrono::system_clock::time_point timestamp)
{
    if (previous_return_timestamp_)
    {
        constexpr long double year_seconds =
            std::chrono::duration<long double>(std::chrono::days{365}).count();
        const long double expected = year_seconds
            / static_cast<long double>(periods_per_year_);
        const long double observed = epoch_seconds(timestamp)
            - epoch_seconds(*previous_return_timestamp_);
        const long double tolerance = std::max(1.0e-9L,
                                               expected * 1.0e-9L);
        if (!(observed > 0.0L)
            || std::abs(observed - expected) > tolerance)
            return_cadence_valid_ = false;
    }
    previous_return_timestamp_ = timestamp;

    if (!(prev_equity_ > 0.0))
    {
        prev_equity_ = equity;
        return;
    }

    constexpr long double limit =
        static_cast<long double>(std::numeric_limits<double>::max());
    const long double projected_return =
        (static_cast<long double>(equity)
         - static_cast<long double>(prev_equity_))
        / static_cast<long double>(prev_equity_);
    const long double mar = static_cast<long double>(risk_free_rate_)
        / static_cast<long double>(periods_per_year_);
    const long double shortfall = projected_return - mar;
    const long double projected_downside = shortfall < 0.0L
        ? static_cast<long double>(downside_sq_sum_)
            + shortfall * shortfall
        : static_cast<long double>(downside_sq_sum_);
    const long double projected_n =
        static_cast<long double>(return_stats_.n) + 1.0L;
    const long double delta = projected_return
        - static_cast<long double>(return_stats_.mean);
    const long double projected_mean =
        static_cast<long double>(return_stats_.mean) + delta / projected_n;
    const long double projected_m2 =
        static_cast<long double>(return_stats_.m2)
        + delta * (projected_return - projected_mean);
    if (!std::isfinite(projected_return)
        || !std::isfinite(projected_downside)
        || !std::isfinite(projected_m2)
        || std::abs(projected_return) > limit
        || projected_downside > limit
        || projected_m2 > limit)
    {
        invalidate_portfolio_time_series(
            portfolio_time_series_failure::non_finite_return);
        prev_equity_ = equity;
        return;
    }
    const double strat_ret = static_cast<double>(projected_return);
    strategy_returns_.push_back(strat_ret);
    return_stats_.update(strat_ret);
    if (shortfall < 0.0L)
        downside_sq_sum_ = static_cast<double>(projected_downside);
    rolling_returns_.push_back(strat_ret);
    if (rolling_returns_.size() > rolling_window_)
        rolling_returns_.pop_front();
    prev_equity_ = equity;
}

Analytics::physical_fill_candidate Analytics::make_physical_fill_candidate(
    const fill_event& f)
{
    const std::uint64_t fill_id = f.get_fill_id();
    const std::string_view venue_execution_id = f.get_venue_execution_id();
    physical_fill_candidate candidate{fill_identity{
        f.get_order_id(), fill_id, f.get_symbol(), f.get_strategy_name(),
        std::string(venue_execution_id),
        std::string(f.get_commission_currency()),
        f.get_opener_order_id(),
        f.get_timestamp(), f.get_side(),
        f.get_filled_quantity(), f.get_fill_price(), f.get_commission(),
        f.get_remaining_qty()}, {}};
    if (!venue_execution_id.empty())
    {
        candidate.venue_key.reserve(candidate.identity.symbol.size() + 1
                                    + venue_execution_id.size());
        candidate.venue_key.append(candidate.identity.symbol);
        candidate.venue_key.push_back('\0');
        candidate.venue_key.append(venue_execution_id);
    }
    return candidate;
}

Analytics::fill_admission Analytics::classify_physical_fill(
    const physical_fill_candidate& candidate) const
{
    const auto& identity = candidate.identity;
    if (identity.fill_id == 0 && identity.venue_execution_id.empty())
        return fill_admission::missing_identity;

    auto local = admitted_fill_identities_.end();
    if (identity.fill_id != 0)
        local = admitted_fill_identities_.find(
            fill_identity_key{identity.order_id, identity.fill_id});

    auto venue = admitted_venue_execution_identities_.end();
    if (!identity.venue_execution_id.empty())
        venue = admitted_venue_execution_identities_.find(candidate.venue_key);

    const bool local_seen = local != admitted_fill_identities_.end();
    const bool venue_seen =
        venue != admitted_venue_execution_identities_.end();
    if ((local_seen && !same_physical_fill(local->second, identity))
        || (venue_seen && !same_physical_fill(venue->second, identity)))
        return fill_admission::conflicting_replay;

    if (local_seen || venue_seen)
        return fill_admission::duplicate;

    return fill_admission::accepted;
}

void Analytics::remember_physical_fill(physical_fill_candidate candidate)
{
    const auto key = fill_identity_key{
        candidate.identity.order_id, candidate.identity.fill_id};
    bool local_inserted = false;
    try
    {
        if (candidate.identity.fill_id != 0)
            local_inserted = admitted_fill_identities_.try_emplace(
                key, candidate.identity).second;
        if (!candidate.identity.venue_execution_id.empty())
            admitted_venue_execution_identities_.try_emplace(
                std::move(candidate.venue_key),
                std::move(candidate.identity));
    }
    catch (...)
    {
        // A half-committed alias ledger would turn a retry into a duplicate
        // and silently lose its economic mutation. unordered_map insertion
        // itself has the strong guarantee; erase the first alias if the
        // second allocation/hash step fails.
        if (local_inserted)
            admitted_fill_identities_.erase(key);
        throw;
    }
}

bool Analytics::fill_keeps_accounting_finite(
    const fill_event& f, std::string_view strategy_name) const
{
    constexpr long double limit =
        static_cast<long double>(std::numeric_limits<double>::max());
    const auto representable = [](long double value) noexcept {
        constexpr long double max =
            static_cast<long double>(std::numeric_limits<double>::max());
        return std::isfinite(value) && std::abs(value) <= max;
    };

    const long double quantity = f.get_filled_quantity();
    const long double price = f.get_fill_price();
    const long double commission = f.get_commission();
    const long double side = f.get_side() == order_side::buy ? 1.0L : -1.0L;
    const long double projected_cash = static_cast<long double>(cash_)
        - side * quantity * price - commission;
    const long double projected_total_commission =
        static_cast<long double>(total_commission_) + commission;
    if (!representable(projected_cash)
        || !representable(projected_total_commission))
        return false;

    const strategy_symbol_key key{std::string(strategy_name), f.get_symbol()};
    const auto existing = open_positions_.find(key);
    const open_position* position = existing == open_positions_.end()
        ? nullptr : &existing->second;
    const long double old_qty = position ? position->qty : 0.0L;
    const long double projected_qty = old_qty + side * quantity;
    if (!representable(projected_qty))
        return false;

    long double projected_avg = position ? position->avg_entry : 0.0L;
    long double projected_open_commission =
        position ? position->open_commission : 0.0L;
    long double realized_delta = 0.0L;
    long double gross_delta = 0.0L;
    long double qty_left = quantity;
    long double qty_after_close = old_qty;
    bool closes_round_trip = false;
    if (old_qty != 0.0L && old_qty * side < 0.0L)
    {
        const long double previous_abs = std::abs(old_qty);
        const long double close_qty = std::min(previous_abs, qty_left);
        const long double position_side = old_qty > 0.0L ? 1.0L : -1.0L;
        gross_delta = (price - projected_avg) * close_qty * position_side;
        const long double close_commission = commission * close_qty / quantity;
        const long double open_share = projected_open_commission
            * close_qty / previous_abs;
        realized_delta = gross_delta - close_commission - open_share;
        projected_open_commission -= open_share;
        qty_after_close += side * close_qty;
        qty_left -= close_qty;
        if (std::abs(qty_after_close) <= 1e-12L)
        {
            closes_round_trip = true;
            qty_after_close = 0.0L;
            projected_avg = 0.0L;
            projected_open_commission = 0.0L;
        }
    }
    if (qty_left > 1e-12L)
    {
        const long double previous_abs = std::abs(qty_after_close);
        const long double denominator = previous_abs + qty_left;
        projected_avg = (projected_avg * previous_abs + price * qty_left)
            / denominator;
        projected_open_commission += commission * qty_left / quantity;
    }

    if (!representable(projected_avg)
        || !representable(projected_open_commission)
        || !representable(static_cast<long double>(realized_leg_pnl_)
                          + realized_delta)
        || !representable(static_cast<long double>(gross_realized_pnl_)
                          + gross_delta)
        || (position
            && !representable(static_cast<long double>(position->round_trip_pnl)
                              + realized_delta)))
        return false;

    if (realized_delta != 0.0L)
    {
        const auto symbol_stats = per_symbol_.find(f.get_symbol());
        if (symbol_stats != per_symbol_.end()
            && !representable(
                static_cast<long double>(symbol_stats->second.total_pnl)
                + realized_delta))
            return false;
        if (!strategy_name.empty())
        {
            const auto strategy_stats = per_strategy_.find(
                std::string(strategy_name));
            if (strategy_stats != per_strategy_.end()
                && !representable(
                    static_cast<long double>(strategy_stats->second.total_pnl)
                    + realized_delta))
                return false;
        }
    }
    if (closes_round_trip && position)
    {
        const long double trip =
            static_cast<long double>(position->round_trip_pnl)
            + realized_delta;
        const long double projected_wins = static_cast<long double>(total_win_)
            + (trip > 0.0L ? trip : 0.0L);
        const long double projected_losses =
            static_cast<long double>(total_loss_)
            + (trip > 0.0L ? 0.0L : std::abs(trip));
        if (!representable(projected_wins)
            || !representable(projected_losses))
            return false;

        const auto check_breakdown = [&](const sub_analytics* stats) {
            if (!stats) return true;
            return representable(static_cast<long double>(stats->total_win)
                                 + (trip > 0.0L ? trip : 0.0L))
                && representable(static_cast<long double>(stats->total_loss)
                                 + (trip > 0.0L ? 0.0L : std::abs(trip)));
        };
        const auto symbol_stats = per_symbol_.find(f.get_symbol());
        if (!check_breakdown(symbol_stats == per_symbol_.end()
                                 ? nullptr : &symbol_stats->second))
            return false;
        if (!strategy_name.empty())
        {
            const auto strategy_stats = per_strategy_.find(
                std::string(strategy_name));
            if (!check_breakdown(strategy_stats == per_strategy_.end()
                                     ? nullptr : &strategy_stats->second))
                return false;
        }
    }

    long double projected_equity = projected_cash;
    for (const auto& [candidate_key, candidate] : open_positions_)
    {
        if (candidate_key == key)
            continue;
        projected_equity += static_cast<long double>(candidate.qty)
            * static_cast<long double>(candidate.last_price);
    }
    const double projected_mark = position && position->last_price > 0.0
        ? position->last_price : f.get_fill_price();
    projected_equity += projected_qty
        * static_cast<long double>(projected_mark);
    return std::isfinite(projected_equity)
        && std::abs(projected_equity) <= limit;
}

bool Analytics::same_physical_fill(const fill_identity& lhs,
                                   const fill_identity& rhs) noexcept
{
    return lhs.order_id == rhs.order_id
        && lhs.symbol == rhs.symbol
        && lhs.strategy_name == rhs.strategy_name
        && lhs.venue_execution_id == rhs.venue_execution_id
        && lhs.commission_currency == rhs.commission_currency
        && lhs.opener_order_id == rhs.opener_order_id
        && lhs.timestamp == rhs.timestamp
        && lhs.side == rhs.side
        && lhs.quantity == rhs.quantity
        && lhs.price == rhs.price
        && lhs.commission == rhs.commission
        && lhs.remaining == rhs.remaining;
}

void Analytics::update_risk_equity(double equity) noexcept
{
    last_equity_ = equity;
    if (equity > peak_equity_)
        peak_equity_ = equity;
    if (peak_equity_ > 0.0)
    {
        const long double dd_wide =
            (static_cast<long double>(peak_equity_)
             - static_cast<long double>(equity))
            / static_cast<long double>(peak_equity_);
        const double dd = std::isfinite(dd_wide)
            && dd_wide <= static_cast<long double>(
                std::numeric_limits<double>::max())
            ? static_cast<double>(dd_wide)
            : std::numeric_limits<double>::max();
        if (dd > max_drawdown_)
        {
            max_drawdown_ = dd;
            max_drawdown_peak_equity_ = peak_equity_;
            max_drawdown_trough_equity_ = equity;
        }
    }
}

bool Analytics::advance_time_accounting(
    std::chrono::system_clock::time_point timestamp) noexcept
{
    if (!last_time_accounting_timestamp_)
    {
        first_time_accounting_timestamp_ = timestamp;
        last_time_accounting_timestamp_ = timestamp;
        return true;
    }
    if (timestamp < *last_time_accounting_timestamp_)
    {
        time_accounting_valid_ = false;
        return false;
    }
    if (any_position_open())
        exposure_duration_ += std::chrono::duration<long double>(
            epoch_seconds(timestamp)
            - epoch_seconds(*last_time_accounting_timestamp_));
    last_time_accounting_timestamp_ = timestamp;
    return true;
}

void Analytics::on_l2_snapshot(const l2_snapshot_event& ev)
{
    if (ev.bid_count() > 0 && ev.ask_count() > 0) {
        double best_bid = ev.bid(0).price;
        double best_ask = ev.ask(0).price;
        if (best_ask > best_bid && best_bid > 0) {
            double mid = (best_ask + best_bid) / 2.0;
            symbol_spread_bps_[ev.get_symbol()] =
                ((best_ask - best_bid) / mid) * 10000.0;
            current_spread_bps_ = 0.0;
            for (const auto& [_, spread] : symbol_spread_bps_)
                current_spread_bps_ = std::max(current_spread_bps_, spread);
            const auto state_update = update_symbol_market_state(
                ev.get_symbol(), mid, ev.get_timestamp(),
                /*allow_same_timestamp_change=*/true,
                /*eligible_for_benchmark=*/false);
            if (state_update != market_state_update::rejected)
            {
                set_symbol_price(ev.get_symbol(), mid);
                const bool marked_open_symbol =
                    mark_open_positions_fresh(ev.get_symbol(),
                                              ev.get_timestamp());
                const double equity = cash_ + position_value();
                if (!portfolio_time_series_valid_)
                {
                    last_equity_ = equity;
                    return;
                }
                if (marked_open_symbol && portfolio_mark_cycle_complete())
                {
                    update_risk_equity(equity);
                    clear_portfolio_mark_cycle();
                }
                else
                    last_equity_ = equity;
            }
        }
    }
}

void Analytics::on_l2_update(const l2_update_event& /*ev*/)
{
    // Incremental updates would require maintaining a local book.
    // Snapshots from the depth stream are sufficient for Phase 2 circuit breakers.
}

std::vector<double> Analytics::equity_tail(std::size_t n) const
{
    std::vector<double> out;
    if (n == 0 || equity_curve_.empty()) return out;
    const std::size_t take = std::min(n, equity_curve_.size());
    out.reserve(take);
    const std::size_t start = equity_curve_.size() - take;
    for (std::size_t i = start; i < equity_curve_.size(); ++i)
        out.push_back(equity_curve_[i].equity);
    return out;
}

std::vector<double> Analytics::drawdown_tail(std::size_t n) const
{
    std::vector<double> out;
    if (n == 0 || equity_curve_.empty()) return out;

    // Walk from the start so the running peak we report reflects the
    // full history, matching how max_drawdown_pct() is computed
    // elsewhere. Cheap - we only emit n values into out.
    const std::size_t take = std::min(n, equity_curve_.size());
    out.reserve(take);
    const std::size_t emit_start = equity_curve_.size() - take;

    double peak = 0.0;
    for (std::size_t i = 0; i < equity_curve_.size(); ++i)
    {
        const double eq = equity_curve_[i].equity;
        if (eq > peak) peak = eq;
        if (i >= emit_start)
        {
            const double dd_pct = (peak > 0.0)
                ? std::max(0.0, (peak - eq) / peak * 100.0)
                : 0.0;
            out.push_back(dd_pct);
        }
    }
    return out;
}

double Analytics::rolling_sharpe() const
{
    if (rolling_returns_.size() < 2) return 0.0;

    double sum = 0.0;
    for (double r : rolling_returns_) sum += r;
    double mean = sum / static_cast<double>(rolling_returns_.size());

    const double ppy = static_cast<double>(periods_per_year_);
    double rf_per_period = (ppy > 0.0) ? risk_free_rate_ / ppy : 0.0;
    double excess_mean = mean - rf_per_period;

    double sq_sum = 0.0;
    for (double r : rolling_returns_)
    {
        double d = r - mean;
        sq_sum += d * d;
    }
    double stddev = std::sqrt(sq_sum / static_cast<double>(rolling_returns_.size() - 1));
    const double ann_factor = std::sqrt(ppy);
    return (stddev > 0.0) ? (excess_mean / stddev) * ann_factor : 0.0;
}

double Analytics::rolling_max_drawdown() const
{
    if (rolling_returns_.empty()) return 0.0;

    double peak = 1.0;
    double equity = 1.0;
    double max_dd = 0.0;

    for (double r : rolling_returns_)
    {
        equity *= (1.0 + r);
        if (equity > peak) peak = equity;
        if (peak > 0.0)
        {
            double dd = (peak - equity) / peak;
            if (dd > max_dd) max_dd = dd;
        }
    }
    return max_dd;
}

AnalyticsReport Analytics::snapshot() const
{
    AnalyticsReport r;
    r.initial_equity = initial_cash_;
    r.final_equity = cash_ + position_value();

    // A report is a terminal observation boundary. If only a subset of the
    // economically open symbols has advanced since the last complete cycle,
    // final_equity contains a mixed-time valuation while the return clock
    // still ends at the older complete cycle. Never annualize that hybrid.
    const bool incomplete_mark_cycle = any_position_open()
        && portfolio_mark_cycle_max_timestamp_.has_value();
    r.portfolio_time_series_valid = portfolio_time_series_valid_
        && !incomplete_mark_cycle;
    r.portfolio_time_series_reason = incomplete_mark_cycle
        ? "incomplete_portfolio_mark_cycle_at_snapshot"
        : std::string(portfolio_time_series_reason(
            portfolio_time_series_failure_));
    if (incomplete_mark_cycle)
    {
        r.valuation_complete = false;
        r.valuation_reason =
            "incomplete_portfolio_mark_cycle_at_snapshot";
    }
    const long double cumulative_return =
        (static_cast<long double>(r.final_equity)
         - static_cast<long double>(initial_cash_))
        / static_cast<long double>(initial_cash_);
    constexpr long double cumulative_limit =
        static_cast<long double>(std::numeric_limits<double>::max());
    if (std::isfinite(cumulative_return)
        && std::abs(cumulative_return) <= cumulative_limit)
        r.cumulative_return = static_cast<double>(cumulative_return);
    else
    {
        r.portfolio_time_series_valid = false;
        r.portfolio_time_series_reason = "non_finite_portfolio_return";
    }
    r.ambiguous_portfolio_mark_sequences_rejected =
        ambiguous_portfolio_mark_sequences_rejected_;
    r.total_orders = total_orders_;
    r.total_fills = total_fills_;
    r.duplicate_fill_replays_ignored = duplicate_fill_replays_ignored_;
    r.conflicting_fill_replays_rejected =
        conflicting_fill_replays_rejected_;
    r.missing_fill_identities_rejected =
        missing_fill_identities_rejected_;
    r.invalid_fill_payloads_rejected = invalid_fill_payloads_rejected_;
    r.unreconciled_funding_events_rejected =
        unreconciled_funding_events_rejected_;
    r.duplicate_funding_replays_ignored =
        duplicate_funding_replays_ignored_;
    r.conflicting_funding_replays_rejected =
        conflicting_funding_replays_rejected_;
    r.late_fill_events_rejected = late_fill_events_rejected_;
    r.late_funding_events_rejected = late_funding_events_rejected_;
    r.late_market_events_rejected = late_market_events_rejected_;
    r.duplicate_market_marks_ignored = duplicate_market_marks_ignored_;
    r.conflicting_market_marks_rejected =
        conflicting_market_marks_rejected_;
    r.total_trades = round_trip_count_;
    r.closing_fill_legs = trade_returns_.size();
    r.soft_post_fill_breaches = soft_post_fill_breaches_;
    r.data_rows_rejected = data_rows_rejected_;
    r.fee_model = fee_model_;
    r.bankrupt = bankrupt_;
    r.bankrupt_equity = bankrupt_equity_;
    r.exit_intents_registered = exit_lifecycle_.registered;
    r.exit_intents_armed      = exit_lifecycle_.armed;
    r.exit_intents_cancelled  = exit_lifecycle_.cancelled;
    r.exit_intents_evicted    = exit_lifecycle_.evicted;
    r.exit_slippage_disarms   = exit_lifecycle_.slippage_disarms;
    r.exit_flatten_requests   = exit_lifecycle_.flatten_requests;

    // Realized PnL is the exact per-leg sum: a position that has scaled out
    // but not yet closed has realized cash even though its round trip is
    // still open (F-09b).
    r.realized_pnl = realized_leg_pnl_;
    r.gross_realized_pnl = gross_realized_pnl_;
    r.funding_pnl = total_funding_pnl_;
    r.total_commission = total_commission_;
    r.periods_per_year = periods_per_year_;
    r.equity_curve_sample_stride = equity_stride_;
    r.benchmark_equity_curve_sample_stride = bench_stride_;
    r.max_drawdown_peak_equity = max_drawdown_peak_equity_;
    r.max_drawdown_trough_equity = max_drawdown_trough_equity_;

    r.unrealized_pnl = 0.0;
    r.open_positions.clear();
    for (const auto& [k, pos] : open_positions_)
    {
        if (std::abs(pos.qty) <= 1e-12) continue;
        open_position_report op;
        op.symbol = pos.symbol;
        op.strategy_name = pos.strategy_name;
        op.quantity = pos.qty;
        op.avg_entry = pos.avg_entry;
        op.mark = pos.last_price > 0.0 ? pos.last_price : pos.avg_entry;
        op.mark_valid = pos.has_market_mark;
        op.mark_source = pos.has_market_mark
            ? "market_mark" : "fill_price_provisional";
        if (!op.mark_valid)
        {
            r.valuation_complete = false;
            r.valuation_reason = "open_position_without_market_mark";
        }
        // Long: (mark - entry) * qty; short qty negative → same formula.
        // Entry commission is already deducted from cash, so the remaining
        // open share belongs in unrealized PnL until the position closes.
        op.unrealized_pnl =
            (op.mark - op.avg_entry) * op.quantity - pos.open_commission;
        r.unrealized_pnl += op.unrealized_pnl;
        r.open_positions.push_back(std::move(op));
    }
    r.reconciliation_residual = r.final_equity -
        (r.initial_equity + r.realized_pnl + r.funding_pnl + r.unrealized_pnl);
    // Scale against the value being reconciled, not the largest gross term.
    // Gross-term scaling becomes materially fail-open under cancellation
    // (for example, 1e16 + -1e16 with only one unit of equity remaining).
    // If double precision cannot reconcile such a cancellation at the final
    // equity scale, the supported accounting path must fail closed.
    const double reconciliation_scale =
        std::max(1.0, std::abs(r.final_equity));
    const double reconciliation_tolerance =
        64.0 * std::numeric_limits<double>::epsilon()
        * reconciliation_scale;
    if (!std::isfinite(r.reconciliation_residual) ||
        std::abs(r.reconciliation_residual) > reconciliation_tolerance)
    {
        r.accounting_reconciled = false;
        r.accounting_reconciliation_reason =
            "accounting_identity_outside_floating_tolerance";
        r.valuation_complete = false;
        r.valuation_reason = r.accounting_reconciliation_reason;
    }
    if (funding_reconciliation_failed_)
    {
        r.accounting_reconciled = false;
        r.accounting_reconciliation_reason =
            "unreconciled_funding_settlement";
        r.valuation_complete = false;
        r.valuation_reason = r.accounting_reconciliation_reason;
    }
    if (fill_reconciliation_failed_)
    {
        r.accounting_reconciled = false;
        r.accounting_reconciliation_reason = "unreconciled_fill_event";
        r.valuation_complete = false;
        r.valuation_reason = r.accounting_reconciliation_reason;
    }

    r.avg_slippage = (slippage_count_ > 0) ? total_slippage_ / static_cast<double>(slippage_count_) : 0.0;
    r.avg_slippage_signed = (slippage_count_ > 0)
        ? total_slippage_signed_ / static_cast<double>(slippage_count_) : 0.0;
    r.avg_adverse_slippage = (adverse_count_ > 0)
        ? total_adverse_slippage_ / static_cast<double>(adverse_count_) : 0.0;
    r.avg_favorable_slippage = (favorable_count_ > 0)
        ? total_favorable_slippage_ / static_cast<double>(favorable_count_) : 0.0;
    r.adverse_slippage_count = adverse_count_;
    r.favorable_slippage_count = favorable_count_;

    r.tick_to_trade_samples = static_cast<std::size_t>(tick_to_trade_ns_.n);
    r.avg_tick_to_trade_ns = tick_to_trade_ns_.mean;
    r.min_tick_to_trade_ns = tick_to_trade_min_ns_;
    r.max_tick_to_trade_ns = tick_to_trade_max_ns_;

    if (!r.portfolio_time_series_valid)
    {
        r.time_in_market_reason = r.portfolio_time_series_reason;
    }
    else if (!time_accounting_valid_)
    {
        r.time_in_market_reason = "non_monotonic_economic_time";
    }
    else if (first_time_accounting_timestamp_
             && last_time_accounting_timestamp_
             && *last_time_accounting_timestamp_
                    > *first_time_accounting_timestamp_)
    {
        const auto horizon = std::chrono::duration<long double>(
            epoch_seconds(*last_time_accounting_timestamp_)
            - epoch_seconds(*first_time_accounting_timestamp_));
        const long double fraction = exposure_duration_.count()
            / horizon.count();
        r.time_in_market_pct = static_cast<double>(
            std::clamp(fraction, 0.0L, 1.0L) * 100.0L);
        r.time_in_market_valid = true;
        r.time_in_market_reason = "computed_from_economic_time";
    }
    r.avg_holding_period_ms = (holding_count_ > 0) ? total_holding_ms_ / static_cast<double>(holding_count_) : 0.0;
    r.total_win = total_win_;
    r.total_loss = total_loss_;

    if (round_trip_count_ > 0)
    {
        r.winning_trades = win_count_;
        r.win_rate = static_cast<double>(win_count_) / static_cast<double>(round_trip_count_) * 100.0;
        r.avg_win = (win_count_ > 0) ? total_win_ / static_cast<double>(win_count_) : 0.0;
        std::size_t losses = round_trip_count_ - win_count_;

        r.avg_loss = (losses > 0) ? total_loss_ / static_cast<double>(losses) : 0.0;
        if (total_loss_ > 0.0)
        {
            r.profit_factor = total_win_ / total_loss_;
            r.profit_factor_valid = true;
            r.profit_factor_reason = "computed_from_gross_win_and_loss";
        }
        else if (total_win_ > 0.0)
        {
            r.profit_factor_unbounded = true;
            r.profit_factor_reason = "no_losses_unbounded";
        }
        r.largest_winner = largest_winner_;
        r.largest_loser = largest_loser_;
    }

    const double ppy = static_cast<double>(periods_per_year_);
    const double rf_per_period = (ppy > 0.0) ? risk_free_rate_ / ppy : 0.0;
    const double ann_factor = std::sqrt(ppy);

    if (!r.portfolio_time_series_valid)
    {
        r.sharpe_ratio_reason = r.portfolio_time_series_reason;
        r.sortino_ratio_reason = r.portfolio_time_series_reason;
    }
    else if (!return_cadence_valid_)
    {
        r.sharpe_ratio_reason = "observation_interval_mismatch";
        r.sortino_ratio_reason = "observation_interval_mismatch";
    }
    else if (return_stats_.n > 1)
    {
        double excess_mean = return_stats_.mean - rf_per_period;
        r.sharpe_ratio = (return_stats_.stddev() > 0.0)
            ? (excess_mean / return_stats_.stddev()) * ann_factor : 0.0;
        r.sharpe_ratio_valid = return_stats_.stddev() > 0.0;
        r.sharpe_ratio_reason = r.sharpe_ratio_valid
            ? "computed_at_configured_cadence" : "zero_return_variance";

        // Downside deviation over ALL periods: sqrt(mean of min(r - MAR, 0)^2).
        double downside_dev = std::sqrt(
            downside_sq_sum_ / static_cast<double>(return_stats_.n));
        if (downside_dev > 0.0)
        {
            r.sortino_ratio = (excess_mean / downside_dev) * ann_factor;
            r.sortino_ratio_valid = true;
            r.sortino_ratio_reason = "computed_at_configured_cadence";
        }
        else if (excess_mean > 0.0)
        {
            r.sortino_ratio = 0.0;
            r.sortino_ratio_valid = false;
            r.sortino_ratio_reason = "no_downside_observed_unbounded";
        }
        else
        {
            r.sortino_ratio_reason = "zero_downside_and_nonpositive_excess_return";
        }
    }

    r.max_drawdown = max_drawdown_ * 100.0;

    if (!r.portfolio_time_series_valid)
    {
        r.annualized_return_reason = r.portfolio_time_series_reason;
    }
    else if (!time_accounting_valid_)
    {
        r.annualized_return_reason = "non_monotonic_economic_time";
    }
    else if (first_time_accounting_timestamp_
             && last_time_accounting_timestamp_
             && *last_time_accounting_timestamp_
                    > *first_time_accounting_timestamp_
             && r.cumulative_return >= -1.0)
    {
        constexpr auto year = std::chrono::days{365};
        const long double elapsed =
            epoch_seconds(*last_time_accounting_timestamp_)
            - epoch_seconds(*first_time_accounting_timestamp_);
        const long double exponent =
            std::chrono::duration<long double>(year).count() / elapsed;
        const double annualized = std::pow(
            1.0 + r.cumulative_return,
            static_cast<double>(exponent)) - 1.0;
        if (std::isfinite(annualized))
        {
            r.annualized_return = annualized;
            r.annualized_return_valid = true;
            r.annualized_return_reason = "computed_from_causal_elapsed_time";
        }
        else
        {
            r.annualized_return_reason = "non_finite_annualization";
        }
    }
    else if (r.cumulative_return < -1.0)
    {
        r.annualized_return_reason = "equity_below_zero";
    }

    if (!r.annualized_return_valid)
    {
        r.calmar_ratio_reason = "annualized_return_unavailable";
    }
    else if (!(r.max_drawdown > 0.0) || !std::isfinite(r.max_drawdown))
    {
        r.calmar_ratio_reason = "positive_finite_drawdown_required";
    }
    else
    {
        const double calmar = (r.annualized_return * 100.0) / r.max_drawdown;
        if (std::isfinite(calmar))
        {
            r.calmar_ratio = calmar;
            r.calmar_ratio_valid = true;
            r.calmar_ratio_reason = "computed_from_annualized_return_and_drawdown";
        }
        else
        {
            r.calmar_ratio_reason = "non_finite_calmar_ratio";
        }
    }

    r.rolling_sharpe = return_cadence_valid_ ? rolling_sharpe() : 0.0;
    r.rolling_max_drawdown = rolling_max_drawdown() * 100.0;
    r.rolling_return_count = rolling_returns_.size();
    r.rolling_window = rolling_window_;
    if (!r.portfolio_time_series_valid)
    {
        r.rolling_sharpe_reason = r.portfolio_time_series_reason;
    }
    else if (!return_cadence_valid_)
    {
        r.rolling_sharpe_reason = "observation_interval_mismatch";
    }
    else if (rolling_returns_.size() < 2)
    {
        r.rolling_sharpe_reason = "insufficient_return_observations";
    }
    else
    {
        double sum = 0.0;
        for (const double value : rolling_returns_) sum += value;
        const double mean = sum / static_cast<double>(rolling_returns_.size());
        double sq_sum = 0.0;
        for (const double value : rolling_returns_)
        {
            const double delta = value - mean;
            sq_sum += delta * delta;
        }
        r.rolling_sharpe_reason = (sq_sum > 0.0)
            ? "computed" : "zero_return_variance";
    }
    r.rolling_max_drawdown_reason = !r.portfolio_time_series_valid
        ? r.portfolio_time_series_reason
        : rolling_returns_.empty()
            ? "insufficient_return_observations"
            : (r.rolling_max_drawdown > 0.0)
                ? "computed" : "no_drawdown_in_window";

    std::string economic_symbol;
    bool economic_symbols_ambiguous = false;
    for (const auto& [_, position] : open_positions_)
    {
        if (economic_symbol.empty())
            economic_symbol = position.symbol;
        else if (position.symbol != economic_symbol)
            economic_symbols_ambiguous = true;
    }

    if (economic_symbols_ambiguous)
    {
        r.benchmark_reason =
            "explicit_benchmark_required_for_cross_symbol_economic_history";
    }
    else if (!economic_symbol.empty()
             && !benchmark_symbol_.empty()
             && economic_symbol != benchmark_symbol_)
    {
        r.benchmark_reason =
            "explicit_benchmark_required_for_cross_symbol_economic_history";
    }
    else if (!economic_symbol.empty())
    {
        r.benchmark_symbol = economic_symbol;
    }
    else
    {
        std::string sole_varying_symbol;
        std::size_t varying_symbols = 0;
        std::size_t marked_symbols = 0;
        for (const auto& [symbol, state] : symbol_market_states_)
        {
            if (!state.has_benchmark_mark)
                continue;
            ++marked_symbols;
            if (state.benchmark_has_varied)
            {
                ++varying_symbols;
                sole_varying_symbol = symbol;
            }
        }
        if (marked_symbols == 1)
        {
            for (const auto& [symbol, state] : symbol_market_states_)
                if (state.has_benchmark_mark) r.benchmark_symbol = symbol;
        }
        else if (varying_symbols == 1)
        {
            r.benchmark_symbol = std::move(sole_varying_symbol);
            r.benchmark_reason =
                "single_varying_market_symbol_constant_others";
        }
        else if (varying_symbols == 0 && !benchmark_symbol_.empty())
        {
            r.benchmark_symbol = benchmark_symbol_;
            r.benchmark_reason = "all_market_symbols_constant";
        }
        else
        {
            r.benchmark_reason =
                "explicit_benchmark_required_for_multi_symbol_market";
        }
    }

    // A report may only expose the benchmark path that was selected and
    // retained causally at ingress. Never relabel another symbol post hoc:
    // its intermediate path was not necessarily recorded.
    if (!r.benchmark_symbol.empty()
        && r.benchmark_symbol != benchmark_symbol_)
    {
        r.benchmark_symbol.clear();
        r.benchmark_reason = "causal_benchmark_path_unavailable";
    }

    if (!r.benchmark_symbol.empty())
    {
        const auto benchmark = symbol_market_states_.find(r.benchmark_symbol);
        if (benchmark != symbol_market_states_.end()
            && benchmark->second.benchmark_last_price > 0.0
            && benchmark->second.benchmark_first_price > 0.0)
        {
            const auto buy_and_hold = finite_benchmark_return(
                benchmark->second.benchmark_last_price,
                benchmark->second.benchmark_first_price);
            const long double relative = buy_and_hold
                ? static_cast<long double>(r.cumulative_return)
                    - static_cast<long double>(*buy_and_hold)
                : std::numeric_limits<long double>::infinity();
            constexpr long double limit = static_cast<long double>(
                std::numeric_limits<double>::max());
            if (benchmark_path_valid_ && buy_and_hold
                && std::isfinite(relative) && std::abs(relative) <= limit)
            {
                r.buy_and_hold_return = *buy_and_hold;
                r.strategy_vs_benchmark = static_cast<double>(relative);
                r.benchmark_valid = true;
            }
            else
            {
                r.benchmark_symbol.clear();
                r.benchmark_reason = benchmark_history_complete_
                    ? "non_finite_benchmark_path"
                    : "causal_benchmark_path_unavailable";
            }
            if (r.benchmark_valid)
            {
                if (economic_symbol.empty()
                    && r.benchmark_reason
                        != "single_varying_market_symbol_constant_others"
                    && r.benchmark_reason != "all_market_symbols_constant")
                    r.benchmark_reason = "single_market_symbol";
                else if (!economic_symbol.empty())
                    r.benchmark_reason = "single_economic_symbol";
            }
        }
        else
        {
            r.benchmark_symbol.clear();
            r.benchmark_reason = "benchmark_marks_unavailable";
        }
    }

    if (r.benchmark_valid
        && r.benchmark_symbol == benchmark_symbol_
        && strategy_returns_.size() > 1
        && benchmark_returns_.size() == strategy_returns_.size())
    {
        const std::size_t n = strategy_returns_.size();
        long double strategy_mean = 0.0L;
        long double benchmark_mean = 0.0L;
        long double covariance_sum = 0.0L;
        long double benchmark_m2 = 0.0L;
        long double active_mean = 0.0L;
        long double active_m2 = 0.0L;
        bool statistics_finite = true;
        for (std::size_t i = 0; i < n; ++i)
        {
            const long double strategy =
                static_cast<long double>(strategy_returns_[i]);
            const long double benchmark =
                static_cast<long double>(benchmark_returns_[i]);
            const long double count = static_cast<long double>(i + 1);
            const long double strategy_delta = strategy - strategy_mean;
            const long double benchmark_delta = benchmark - benchmark_mean;
            strategy_mean += strategy_delta / count;
            benchmark_mean += benchmark_delta / count;
            covariance_sum += strategy_delta * (benchmark - benchmark_mean);
            benchmark_m2 += benchmark_delta * (benchmark - benchmark_mean);

            const long double active = strategy - benchmark;
            const long double active_delta = active - active_mean;
            active_mean += active_delta / count;
            active_m2 += active_delta * (active - active_mean);
            if (!std::isfinite(strategy_mean)
                || !std::isfinite(benchmark_mean)
                || !std::isfinite(covariance_sum)
                || !std::isfinite(benchmark_m2)
                || !std::isfinite(active_mean)
                || !std::isfinite(active_m2))
            {
                statistics_finite = false;
                break;
            }
        }
        if (statistics_finite && benchmark_m2 >= 0.0L && active_m2 >= 0.0L)
        {
            const long double divisor = static_cast<long double>(n - 1);
            const long double covariance = covariance_sum / divisor;
            const long double benchmark_variance = benchmark_m2 / divisor;
            const long double beta = benchmark_variance > 0.0L
                ? covariance / benchmark_variance : 0.0L;
            const long double alpha = strategy_mean - beta * benchmark_mean;
            const long double tracking_error = std::sqrt(active_m2 / divisor);
            const long double information_ratio = tracking_error > 0.0L
                ? active_mean / tracking_error : 0.0L;
            const auto beta_result = finite_double(beta);
            const auto alpha_result = finite_double(alpha);
            const auto tracking_result = finite_double(tracking_error);
            const auto information_result = finite_double(information_ratio);
            if (beta_result && alpha_result && tracking_result
                && information_result)
            {
                r.beta = *beta_result;
                r.alpha = *alpha_result;
                r.tracking_error = *tracking_result;
                r.information_ratio = *information_result;
            }
            else
                statistics_finite = false;
        }
        else
            statistics_finite = false;
        if (!statistics_finite)
        {
            r.benchmark_valid = false;
            r.benchmark_symbol.clear();
            r.benchmark_reason = "non_finite_benchmark_statistics";
        }
    }

    r.per_symbol = per_symbol_;
    r.per_strategy = per_strategy_;
    r.contains_exploratory_execution = contains_exploratory_execution_;

    return r;
}

AnalyticsReport Analytics::generate_report() const
{
    AnalyticsReport r = snapshot();

    r.equity_curve = equity_curve_;
    r.trade_returns = trade_returns_;
    r.trades = trades_;
    if (r.benchmark_valid && r.benchmark_symbol == benchmark_symbol_)
        r.benchmark_equity_curve = benchmark_curve_;

    return r;
}

void Analytics::print_report() const
{
    std::cout << tt::render_report(generate_report());
}

void Analytics::export_csv(const std::string& equity_path, const std::string& trades_path) const
{
    {
        std::ofstream f(equity_path);
        f << "timestamp_ms,equity\n";
        for (const auto& pt : equity_curve_)
        {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                pt.timestamp.time_since_epoch()).count();
            f << ms << "," << std::fixed << std::setprecision(2) << pt.equity << "\n";
        }
    }

    {
        std::ofstream f(trades_path);
        f << "row_kind,timestamp_ms,order_id,fill_id,side,quantity,fill_price,intended_price,reference_price,reference_timestamp_ms,"
             "execution_model,execution_reason,exploratory,modeled_spread_bps,modeled_impact_bps,"
             "fill_probability,modeled_latency_ns,commission,pnl,symbol,strategy\n";
        for (const auto& t : trades_)
        {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                t.timestamp.time_since_epoch()).count();
            f << "physical_fill_leg," << ms << ","
              << t.order_id << ","
              << t.fill_id << ","
              << (t.side == order_side::buy ? "BUY" : "SELL") << ","
              << t.quantity << ","
              << std::fixed << std::setprecision(6) << t.fill_price << ","
              << t.intended_price << ","
              << t.reference_price << ","
              << std::chrono::duration_cast<std::chrono::milliseconds>(
                     t.reference_timestamp.time_since_epoch()).count() << ","
              << fill_execution_model_name(t.execution_model) << ","
              << fill_execution_reason_name(t.execution_reason) << ","
              << (t.execution_is_exploratory ? "true" : "false") << ","
              << t.modeled_spread_bps << ","
              << t.modeled_impact_bps << ","
              << t.fill_probability << ","
              << t.modeled_latency.count() << ","
              << t.commission << ","
              << std::setprecision(2) << t.pnl << ","
              << t.symbol << ","
              << t.strategy_name << "\n";
        }
    }
}

void Analytics::export_json(const std::string& path) const
{
    const auto report = generate_report();
    std::ofstream output(path);
    if (!output.is_open())
    {
        std::cerr << "  Failed to open output file: " << path << "\n";
        return;
    }
    output << report.to_results_json() << '\n';
}
