#include "analytics.h"
#include "report_generator.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>

namespace {
static const bool kAnalyticsPnlDebug = [] {
    const char* v = std::getenv("TT_ANALYTICS_PNL_DEBUG");
    return v && (*v == '1' || *v == 't' || *v == 'T');
}();
} // namespace

Analytics::Analytics(double initial_cash, std::size_t rolling_window, double risk_free_rate,
                     std::size_t periods_per_year, std::size_t max_equity_points)
    : initial_cash_(initial_cash), cash_(initial_cash),
      rolling_window_(rolling_window), risk_free_rate_(risk_free_rate),
      periods_per_year_(periods_per_year > 0 ? periods_per_year : 252),
      max_equity_points_(max_equity_points > 4 ? max_equity_points : 4),
      prev_equity_(initial_cash), peak_equity_(initial_cash) {}

void Analytics::reserve_hint(std::size_t expected_bars)
{
    // Cap equity curves at the decimation ceiling; returns vectors take
    // the raw hint — they're doubles, 8 bytes each, so even 10M entries
    // is 80 MB, acceptable for backtests at that scale.
    const std::size_t curve_cap = std::min(expected_bars, max_equity_points_);
    equity_curve_.reserve(curve_cap);
    benchmark_curve_.reserve(curve_cap);
    strategy_returns_.reserve(expected_bars);
    benchmark_returns_.reserve(expected_bars);
}

// Phase A (MC object reuse): reset to initial constructed state.
void Analytics::reset(double initial_cash)
{
    initial_cash_ = initial_cash;
    cash_ = initial_cash;
    position_qty_ = 0.0;
    avg_entry_price_ = 0.0;
    total_open_commission_ = 0.0;
    entry_time_ = {};

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

    last_equity_ = 0.0;
    realized_vol_1h_ = 0.0;
    last_mid_price_ = 0.0;
    current_spread_bps_ = 0.0;
    current_funding_8h_rate_ = 0.0;

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

    total_holding_ms_ = 0.0;
    holding_count_ = 0;
    market_events_total_ = 0;
    market_events_in_position_ = 0;

    first_price_ = 0.0;
    first_price_set_ = false;
    prev_bh_equity_ = initial_cash;

    per_symbol_.clear();
    per_strategy_.clear();

    return_stats_.reset();
    downside_stats_.reset();

    peak_equity_ = initial_cash;
    max_drawdown_ = 0.0;

    win_count_ = 0;
    total_win_ = 0.0;
    total_loss_ = 0.0;
    largest_winner_ = 0.0;
    largest_loser_ = 0.0;

    tick_to_trade_ns_.reset();
    tick_to_trade_min_ns_ = 0;
    tick_to_trade_max_ns_ = 0;

    // Note: last_close_ is intentionally left; it will be overwritten on first market event.
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
    // Phase 2.1 — funding cash deltas adjust our internal cash and equity curve.
    // This makes funding visible in reports, TUI sparkline, and risk_view().
    cash_ += fe.get_cash_delta();
    total_funding_pnl_ += fe.get_cash_delta();

    // Mirror the equity calculation from on_market
    bool has_position = std::abs(position_qty_) > 1e-12;
    double equity = cash_;
    if (has_position)
        equity += position_qty_ * last_close_;

    last_equity_ = equity;

    // Record a point so the equity curve (and any downstream reports) shows the funding step
    record_equity_point(equity_curve_, equity_stride_, equity_counter_,
                        {fe.get_timestamp(), equity});
}

void Analytics::on_market(const market_event& m)
{
    last_close_ = m.get_close();

    // Phase 2.3 — track mid and simple realized vol (EWMA of log returns)
    double mid = m.get_close();  // for bar data we use close as proxy for mid
    if (last_mid_price_ > 0.0 && mid > 0.0) {
        double ret = std::log(mid / last_mid_price_);
        double alpha = 0.02;  // ~ 1h half-life rough for 1m bars
        realized_vol_1h_ = alpha * std::abs(ret) + (1.0 - alpha) * realized_vol_1h_;
    }
    last_mid_price_ = mid;

    if (!first_price_set_)
    {
        first_price_ = m.get_close();
        first_price_set_ = true;
    }

    market_events_total_++;
    bool has_position = std::abs(position_qty_) > 1e-12;
    if (has_position)
        market_events_in_position_++;

    double equity = cash_;
    if (has_position)
        equity += position_qty_ * last_close_;

    record_equity_point(equity_curve_, equity_stride_, equity_counter_,
                        {m.get_timestamp(), equity});

    double bh_equity_now = 0.0;
    bool have_bh_now = false;
    if (first_price_set_ && first_price_ > 0.0)
    {
        bh_equity_now = initial_cash_ * (m.get_close() / first_price_);
        have_bh_now = true;
    }

    if (prev_equity_ > 0.0 && market_events_total_ > 1)
    {
        double strat_ret = (equity - prev_equity_) / prev_equity_;
        strategy_returns_.push_back(strat_ret);

        return_stats_.update(strat_ret);
        if (strat_ret < 0.0) downside_stats_.update(strat_ret);

        if (have_bh_now && prev_bh_equity_ > 0.0)
        {
            double bh_ret = (bh_equity_now - prev_bh_equity_) / prev_bh_equity_;
            benchmark_returns_.push_back(bh_ret);
        }

        rolling_returns_.push_back(strat_ret);
        if (rolling_returns_.size() > rolling_window_)
            rolling_returns_.pop_front();
    }
    prev_equity_ = equity;
    if (have_bh_now)
    {
        record_equity_point(benchmark_curve_, bench_stride_, bench_counter_,
                            {m.get_timestamp(), bh_equity_now});
        prev_bh_equity_ = bh_equity_now;
    }

    if (equity > peak_equity_)
        peak_equity_ = equity;

    if (peak_equity_ > 0.0)
    {
        double dd = (peak_equity_ - equity) / peak_equity_;
        if (dd > max_drawdown_)
            max_drawdown_ = dd;
    }
}

void Analytics::on_tick(const tick_event& t)
{
    last_close_ = t.get_price();

    // Phase 2.3 — update vol from tick mid (price)
    double mid = t.get_price();
    if (last_mid_price_ > 0.0 && mid > 0.0) {
        double ret = std::log(mid / last_mid_price_);
        double alpha = 0.02;
        realized_vol_1h_ = alpha * std::abs(ret) + (1.0 - alpha) * realized_vol_1h_;
    }
    last_mid_price_ = mid;

    if (!first_price_set_)
    {
        first_price_ = t.get_price();
        first_price_set_ = true;
    }
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

    double intended = 0.0;
    auto it = order_prices_.find(f.get_order_id());
    if (it != order_prices_.end())
    {
        intended = it->second;
        double raw = f.get_fill_price() - intended;
        double side_sign = (f.get_side() == order_side::buy) ? +1.0 : -1.0;
        double signed_slip = raw * side_sign;
        total_slippage_ += std::abs(raw);
        total_slippage_signed_ += signed_slip;
        if (signed_slip > 0.0) { total_adverse_slippage_ += signed_slip; adverse_count_++; }
        else if (signed_slip < 0.0) { total_favorable_slippage_ += -signed_slip; favorable_count_++; }
        slippage_count_++;
    }

    std::string strat_name;
    auto strat_it = order_strategies_.find(f.get_order_id());
    if (strat_it != order_strategies_.end())
        strat_name = strat_it->second;

    trade_record rec;
    rec.order_id = f.get_order_id();
    rec.side = f.get_side();
    rec.quantity = f.get_filled_quantity();
    rec.fill_price = f.get_fill_price();
    rec.commission = f.get_commission();
    rec.intended_price = intended;
    rec.timestamp = f.get_timestamp();
    rec.pnl = 0.0;
    rec.symbol = f.get_symbol();
    rec.strategy_name = strat_name;

    const double filled_qty = f.get_filled_quantity();
    const double fill_price = f.get_fill_price();
    const double commission = f.get_commission();
    const double side_sign = (f.get_side() == order_side::buy) ? +1.0 : -1.0;
    double qty_left = filled_qty;

    if (std::abs(position_qty_) > 1e-12 && position_qty_ * side_sign < 0.0)
    {
        double pos_sign = (position_qty_ > 0.0) ? 1.0 : -1.0;
        double prev_abs = std::abs(position_qty_);
        double close_qty = std::min(prev_abs, qty_left);

        double gross = (fill_price - avg_entry_price_) * close_qty * pos_sign;

        double close_comm = commission * (close_qty / filled_qty);
        double open_comm_share = total_open_commission_ * (close_qty / prev_abs);
        double pnl = gross - close_comm - open_comm_share;

        if (kAnalyticsPnlDebug) {
            std::fprintf(stderr,
                "[ANALYTICS_PNL_CLOSE] sym=%s side=%s fill_px=%.8f filled_qty=%.6f comm=%.6f "
                "pos_qty=%.6f avg_entry=%.8f open_comm_acc=%.6f "
                "close_qty=%.6f gross=%.8f close_c=%.6f open_share=%.6f pnl=%.8f\n",
                f.get_symbol().c_str(),
                (f.get_side() == order_side::buy ? "BUY" : "SELL"),
                fill_price, filled_qty, commission,
                position_qty_, avg_entry_price_, total_open_commission_,
                close_qty, gross, close_comm, open_comm_share, pnl);
        }

        total_open_commission_ -= open_comm_share;

        cash_ += -side_sign * close_qty * fill_price - close_comm;

        position_qty_ += side_sign * close_qty;
        qty_left -= close_qty;
        if (std::abs(position_qty_) < 1e-12)
        {
            position_qty_ = 0.0;
            avg_entry_price_ = 0.0;
            total_open_commission_ = 0.0;
        }

        rec.pnl = pnl;
        trade_returns_.push_back(pnl);

        if (pnl > 0.0)
        {
            win_count_++;
            total_win_ += pnl;
            if (pnl > largest_winner_) largest_winner_ = pnl;
        }
        else
        {
            total_loss_ += std::abs(pnl);
            if (pnl < largest_loser_) largest_loser_ = pnl;
        }

        {
            auto& sa = per_symbol_[f.get_symbol()];
            sa.total_pnl += pnl;
            sa.trade_count++;
            if (pnl > 0.0) { sa.win_count++; sa.total_win += pnl; }
            else { sa.total_loss += std::abs(pnl); }
        }
        if (!strat_name.empty())
        {
            auto& sa = per_strategy_[strat_name];
            sa.total_pnl += pnl;
            sa.trade_count++;
            if (pnl > 0.0) { sa.win_count++; sa.total_win += pnl; }
            else { sa.total_loss += std::abs(pnl); }
        }

        auto hold_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            f.get_timestamp() - entry_time_).count();
        total_holding_ms_ += static_cast<double>(hold_ms);
        holding_count_++;
    }

    if (qty_left > 1e-12)
    {
        double open_comm = commission * (qty_left / filled_qty);
        double prev_abs = std::abs(position_qty_);

        avg_entry_price_ = (avg_entry_price_ * prev_abs + fill_price * qty_left)
                         / (prev_abs + qty_left);
        position_qty_ += side_sign * qty_left;
        total_open_commission_ += open_comm;

        if (kAnalyticsPnlDebug && prev_abs < 1e-12) {
            std::fprintf(stderr,
                "[ANALYTICS_PNL_OPEN] sym=%s side=%s fill_px=%.8f qty=%.6f comm=%.6f "
                "avg_entry_set=%.8f open_comm_acc=%.6f\n",
                f.get_symbol().c_str(),
                (f.get_side() == order_side::buy ? "BUY" : "SELL"),
                fill_price, qty_left, open_comm, avg_entry_price_, total_open_commission_);
        }

        if (prev_abs < 1e-12)
            entry_time_ = f.get_timestamp();

        cash_ -= side_sign * qty_left * fill_price + open_comm;
    }

    trades_.push_back(rec);
}

void Analytics::on_l2_snapshot(const l2_snapshot_event& ev)
{
    if (ev.bid_count() > 0 && ev.ask_count() > 0) {
        double best_bid = ev.bid(0).price;
        double best_ask = ev.ask(0).price;
        if (best_ask > best_bid && best_bid > 0) {
            double mid = (best_ask + best_bid) / 2.0;
            current_spread_bps_ = ((best_ask - best_bid) / mid) * 10000.0;
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
    // elsewhere. Cheap — we only emit n values into out.
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
    r.final_equity = cash_;
    if (std::abs(position_qty_) > 1e-12)
        r.final_equity += position_qty_ * last_close_;

    r.cumulative_return = (r.final_equity - initial_cash_) / initial_cash_;
    r.total_orders = total_orders_;
    r.total_fills = total_fills_;
    r.total_trades = trade_returns_.size();

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

    r.time_in_market_pct = (market_events_total_ > 0)
        ? (static_cast<double>(market_events_in_position_) / static_cast<double>(market_events_total_)) * 100.0
        : 0.0;
    r.avg_holding_period_ms = (holding_count_ > 0) ? total_holding_ms_ / static_cast<double>(holding_count_) : 0.0;

    if (!trade_returns_.empty())
    {
        r.winning_trades = win_count_;
        r.win_rate = static_cast<double>(win_count_) / static_cast<double>(trade_returns_.size()) * 100.0;
        r.avg_win = (win_count_ > 0) ? total_win_ / static_cast<double>(win_count_) : 0.0;
        std::size_t losses = trade_returns_.size() - win_count_;
        r.avg_loss = (losses > 0) ? total_loss_ / static_cast<double>(losses) : 0.0;
        r.profit_factor = (total_loss_ > 0.0) ? total_win_ / total_loss_ : (total_win_ > 0.0 ? 1e9 : 0.0);
        r.largest_winner = largest_winner_;
        r.largest_loser = largest_loser_;
    }

    const double ppy = static_cast<double>(periods_per_year_);
    const double rf_per_period = (ppy > 0.0) ? risk_free_rate_ / ppy : 0.0;
    const double ann_factor = std::sqrt(ppy);

    if (return_stats_.n > 1)
    {
        double excess_mean = return_stats_.mean - rf_per_period;
        r.sharpe_ratio = (return_stats_.stddev() > 0.0)
            ? (excess_mean / return_stats_.stddev()) * ann_factor : 0.0;
    }
    if (downside_stats_.n > 1)
    {
        double excess_mean = return_stats_.mean - rf_per_period;
        r.sortino_ratio = (downside_stats_.stddev() > 0.0)
            ? (excess_mean / downside_stats_.stddev()) * ann_factor : 0.0;
    }
    else if (return_stats_.n > 1 && return_stats_.mean > rf_per_period)
    {
        r.sortino_ratio = 1e9;
    }

    r.max_drawdown = max_drawdown_ * 100.0;

    const double n_periods = static_cast<double>(strategy_returns_.size());
    if (n_periods > 0.0 && ppy > 0.0 && r.cumulative_return > -1.0)
    {
        r.annualized_return = std::pow(1.0 + r.cumulative_return, ppy / n_periods) - 1.0;
    }
    else
    {
        r.annualized_return = r.cumulative_return;
    }

    r.calmar_ratio = (r.max_drawdown > 0.0)
        ? (r.annualized_return * 100.0) / r.max_drawdown
        : 0.0;

    r.rolling_sharpe = rolling_sharpe();
    r.rolling_max_drawdown = rolling_max_drawdown() * 100.0;

    if (first_price_set_ && last_close_ > 0.0 && first_price_ > 0.0)
        r.buy_and_hold_return = (last_close_ - first_price_) / first_price_;

    r.strategy_vs_benchmark = r.cumulative_return - r.buy_and_hold_return;

    if (strategy_returns_.size() > 1 && benchmark_returns_.size() == strategy_returns_.size())
    {
        std::size_t n = strategy_returns_.size();

        double s_mean = 0.0, b_mean = 0.0;
        for (std::size_t i = 0; i < n; ++i)
        {
            s_mean += strategy_returns_[i];
            b_mean += benchmark_returns_[i];
        }
        s_mean /= static_cast<double>(n);
        b_mean /= static_cast<double>(n);

        double cov = 0.0, var_b = 0.0;
        for (std::size_t i = 0; i < n; ++i)
        {
            double ds = strategy_returns_[i] - s_mean;
            double db = benchmark_returns_[i] - b_mean;
            cov += ds * db;
            var_b += db * db;
        }
        cov /= static_cast<double>(n - 1);
        var_b /= static_cast<double>(n - 1);

        r.beta = (var_b > 0.0) ? cov / var_b : 0.0;
        r.alpha = s_mean - r.beta * b_mean;

        double te_sum = 0.0;
        for (std::size_t i = 0; i < n; ++i)
        {
            double diff = strategy_returns_[i] - benchmark_returns_[i];
            te_sum += (diff - (s_mean - b_mean)) * (diff - (s_mean - b_mean));
        }
        r.tracking_error = std::sqrt(te_sum / static_cast<double>(n - 1));
        r.information_ratio = (r.tracking_error > 0.0)
            ? (s_mean - b_mean) / r.tracking_error : 0.0;
    }

    r.per_symbol = per_symbol_;
    r.per_strategy = per_strategy_;

    return r;
}

AnalyticsReport Analytics::generate_report() const
{
    AnalyticsReport r = snapshot();

    r.equity_curve = equity_curve_;
    r.trade_returns = trade_returns_;
    r.trades = trades_;
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
        f << "timestamp_ms,order_id,side,quantity,fill_price,intended_price,commission,pnl,symbol,strategy\n";
        for (const auto& t : trades_)
        {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                t.timestamp.time_since_epoch()).count();
            f << ms << ","
              << t.order_id << ","
              << (t.side == order_side::buy ? "BUY" : "SELL") << ","
              << t.quantity << ","
              << std::fixed << std::setprecision(6) << t.fill_price << ","
              << t.intended_price << ","
              << t.commission << ","
              << std::setprecision(2) << t.pnl << ","
              << t.symbol << ","
              << t.strategy_name << "\n";
        }
    }
}

void Analytics::export_json(const std::string& path) const
{
    auto r = generate_report();

    std::ofstream f(path);
    if (!f.is_open())
    {
        std::cerr << "  Failed to open output file: " << path << "\n";
        return;
    }

    char buf[2048];
    std::snprintf(buf, sizeof(buf),
        R"({"initial_equity":%.2f,"final_equity":%.2f,"cumulative_return":%.6f,"annualized_return":%.6f,)"
        R"("sharpe_ratio":%.6f,"sortino_ratio":%.6f,"max_drawdown":%.6f,"calmar_ratio":%.6f,)"
        R"("rolling_sharpe":%.6f,"rolling_max_drawdown":%.6f,)"
        R"("win_rate":%.6f,"profit_factor":%.6f,"total_trades":%zu,)"
        R"("avg_win":%.6f,"avg_loss":%.6f,"largest_winner":%.6f,"largest_loser":%.6f,)"
        R"("time_in_market_pct":%.4f,"avg_slippage":%.6f,)"
        R"("buy_and_hold_return":%.6f,"strategy_vs_benchmark":%.6f,)"
        R"("alpha":%.6f,"beta":%.6f,"information_ratio":%.6f,"tracking_error":%.6f)",
        r.initial_equity, r.final_equity, r.cumulative_return, r.annualized_return,
        r.sharpe_ratio, r.sortino_ratio, r.max_drawdown, r.calmar_ratio,
        r.rolling_sharpe, r.rolling_max_drawdown,
        r.win_rate, r.profit_factor, r.total_trades,
        r.avg_win, r.avg_loss, r.largest_winner, r.largest_loser,
        r.time_in_market_pct, r.avg_slippage,
        r.buy_and_hold_return, r.strategy_vs_benchmark,
        r.alpha, r.beta, r.information_ratio, r.tracking_error);

    f << "{" << (buf + 1);

    f << ",\"equity_curve\":[";
    for (std::size_t i = 0; i < r.equity_curve.size(); ++i)
    {
        if (i > 0) f << ",";
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            r.equity_curve[i].timestamp.time_since_epoch()).count();
        char pt[128];
        std::snprintf(pt, sizeof(pt), "[%lld,%.2f]",
            static_cast<long long>(ms), r.equity_curve[i].equity);
        f << pt;
    }
    f << "]";

    f << ",\"trades\":[";
    for (std::size_t i = 0; i < r.trades.size(); ++i)
    {
        if (i > 0) f << ",";
        const auto& t = r.trades[i];
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            t.timestamp.time_since_epoch()).count();
        char tbuf[512];
        std::snprintf(tbuf, sizeof(tbuf),
            R"({"timestamp":%lld,"order_id":%llu,"side":"%s","quantity":%.8g,"fill_price":%.6f,"commission":%.6f,"pnl":%.2f,"symbol":"%s","strategy":"%s"})",
            static_cast<long long>(ms),
            static_cast<unsigned long long>(t.order_id),
            (t.side == order_side::buy) ? "buy" : "sell",
            t.quantity, t.fill_price, t.commission, t.pnl,
            t.symbol.c_str(), t.strategy_name.c_str());
        f << tbuf;
    }
    f << "]";

    f << ",\"per_symbol\":{";
    {
        bool first = true;
        for (const auto& [sym, sa] : r.per_symbol)
        {
            if (!first) f << ",";
            first = false;
            char sbuf[256];
            std::snprintf(sbuf, sizeof(sbuf),
                R"("%s":{"total_pnl":%.2f,"trade_count":%zu,"win_rate":%.2f,"profit_factor":%.4f})",
                sym.c_str(), sa.total_pnl, sa.trade_count, sa.win_rate(), sa.profit_factor());
            f << sbuf;
        }
    }
    f << "}";

    f << ",\"per_strategy\":{";
    {
        bool first = true;
        for (const auto& [name, sa] : r.per_strategy)
        {
            if (!first) f << ",";
            first = false;
            char sbuf[256];
            std::snprintf(sbuf, sizeof(sbuf),
                R"("%s":{"total_pnl":%.2f,"trade_count":%zu,"win_rate":%.2f,"profit_factor":%.4f})",
                name.c_str(), sa.total_pnl, sa.trade_count, sa.win_rate(), sa.profit_factor());
            f << sbuf;
        }
    }
    f << "}";

    f << "}\n";
}
