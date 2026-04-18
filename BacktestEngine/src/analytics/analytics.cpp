#include "analytics.h"
#include "report_generator.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>

Analytics::Analytics(double initial_cash, std::size_t rolling_window, double risk_free_rate)
    : initial_cash_(initial_cash), cash_(initial_cash),
      rolling_window_(rolling_window), risk_free_rate_(risk_free_rate),
      prev_equity_(initial_cash), peak_equity_(initial_cash) {}

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
        case event_type::l2_update:
        case event_type::cancel:
        case event_type::amend:
        case event_type::rejection:
            break;
    }
}

void Analytics::on_market(const market_event& m)
{
    last_close_ = m.get_close();

    if (!first_price_set_)
    {
        first_price_ = m.get_close();
        first_price_set_ = true;
    }

    market_events_total_++;
    bool has_position = std::abs(position_qty_) > 1e-12;
    if (has_position)
        market_events_in_position_++;

    // Mark-to-market works for both long (qty > 0) and short (qty < 0).
    double equity = cash_;
    if (has_position)
        equity += position_qty_ * last_close_;

    equity_curve_.push_back({m.get_timestamp(), equity});

    // Benchmark equity curve (buy-and-hold from initial_cash_)
    if (first_price_set_ && first_price_ > 0.0)
    {
        double bh_equity = initial_cash_ * (m.get_close() / first_price_);
        benchmark_curve_.push_back({m.get_timestamp(), bh_equity});
    }

    // Per-bar returns for alpha/beta calculation
    if (prev_equity_ > 0.0 && equity_curve_.size() > 1)
    {
        double strat_ret = (equity - prev_equity_) / prev_equity_;
        strategy_returns_.push_back(strat_ret);

        if (benchmark_curve_.size() > 1)
        {
            double prev_bh = benchmark_curve_[benchmark_curve_.size() - 2].equity;
            double curr_bh = benchmark_curve_.back().equity;
            double bh_ret = (prev_bh > 0.0) ? (curr_bh - prev_bh) / prev_bh : 0.0;
            benchmark_returns_.push_back(bh_ret);
        }

        // Rolling window: track equity returns
        rolling_returns_.push_back(strat_ret);
        if (rolling_returns_.size() > rolling_window_)
            rolling_returns_.pop_front();
    }
    prev_equity_ = equity;

    // Running max drawdown
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
    // In tick-streaming mode on_market never fires, so without this arm
    // last_close_ stays 0 (making final_equity ignore open-position MtM) and
    // first_price_ is never set (zeroing out buy_and_hold_return). We update
    // only those two fields here — equity_curve_/strategy_returns_ stay bar-
    // granular because per-tick pushes would explode the report on live feeds
    // with 10^5+ ticks/min.
    last_close_ = t.get_price();
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

    // Tick-to-trade latency: read the hot-path stamp set when the book matched.
    // We deliberately do not re-sample the clock here — in threaded presets
    // this runs on the stats worker after the event crossed a ring buffer.
    if (f.get_latency_ns() > 0)
    {
        int64_t lat = f.get_latency_ns();
        tick_to_trade_ns_.update(static_cast<double>(lat));
        if (tick_to_trade_ns_.n == 1 || lat < tick_to_trade_min_ns_)
            tick_to_trade_min_ns_ = lat;
        if (lat > tick_to_trade_max_ns_)
            tick_to_trade_max_ns_ = lat;
    }

    // Slippage: intended vs actual fill price. Direction-adjust so positive
    // = adverse (paid more on a buy, received less on a sell).
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

    // Resolve strategy name from order
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

    // Closing leg: existing position sign is opposite to the fill side.
    // Handles long→sell-close, short→buy-close, and partial/full closes.
    // A flip (e.g. long 3, sell 5) is handled by the opening leg below
    // running after qty_left is decremented by close_qty.
    if (std::abs(position_qty_) > 1e-12 && position_qty_ * side_sign < 0.0)
    {
        double pos_sign = (position_qty_ > 0.0) ? 1.0 : -1.0;
        double prev_abs = std::abs(position_qty_);
        double close_qty = std::min(prev_abs, qty_left);

        // Gross: long profits when exit > entry, short profits when exit < entry.
        double gross = (fill_price - avg_entry_price_) * close_qty * pos_sign;

        // Pro-rata commissions: split this fill's commission across close/open
        // legs, and charge the share of open-side commissions accumulated while
        // building the closed portion.
        double close_comm = commission * (close_qty / filled_qty);
        double open_comm_share = total_open_commission_ * (close_qty / prev_abs);
        double pnl = gross - close_comm - open_comm_share;

        total_open_commission_ -= open_comm_share;

        // Cash flow: sell-to-close-long adds proceeds; buy-to-close-short spends.
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
        return_stats_.update(pnl);
        if (pnl < 0.0) downside_stats_.update(pnl);

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

    // Opening / adding / flipping leg: any remaining qty opens or adds to a
    // position in the fill's direction (long on buy, short on sell).
    if (qty_left > 1e-12)
    {
        double open_comm = commission * (qty_left / filled_qty);
        double prev_abs = std::abs(position_qty_);

        avg_entry_price_ = (avg_entry_price_ * prev_abs + fill_price * qty_left)
                         / (prev_abs + qty_left);
        position_qty_ += side_sign * qty_left;
        total_open_commission_ += open_comm;

        // Stamp entry_time_ only when starting from flat (prev_abs ≈ 0).
        if (prev_abs < 1e-12)
            entry_time_ = f.get_timestamp();

        // Cash: opening long spends, opening short receives proceeds.
        cash_ -= side_sign * qty_left * fill_price + open_comm;
    }

    trades_.push_back(rec);
}

double Analytics::rolling_sharpe() const
{
    if (rolling_returns_.size() < 2) return 0.0;

    double sum = 0.0;
    for (double r : rolling_returns_) sum += r;
    double mean = sum / static_cast<double>(rolling_returns_.size());

    // Per-bar risk-free rate
    double rf_per_bar = (rolling_window_ > 0) ? risk_free_rate_ / static_cast<double>(rolling_window_) : 0.0;
    double excess_mean = mean - rf_per_bar;

    double sq_sum = 0.0;
    for (double r : rolling_returns_)
    {
        double d = r - mean;
        sq_sum += d * d;
    }
    double stddev = std::sqrt(sq_sum / static_cast<double>(rolling_returns_.size() - 1));
    return (stddev > 0.0) ? excess_mean / stddev : 0.0;
}

double Analytics::rolling_max_drawdown() const
{
    if (rolling_returns_.empty()) return 0.0;

    // Reconstruct relative equity from rolling returns
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

    // Slippage
    r.avg_slippage = (slippage_count_ > 0) ? total_slippage_ / static_cast<double>(slippage_count_) : 0.0;
    r.avg_slippage_signed = (slippage_count_ > 0)
        ? total_slippage_signed_ / static_cast<double>(slippage_count_) : 0.0;
    r.avg_adverse_slippage = (adverse_count_ > 0)
        ? total_adverse_slippage_ / static_cast<double>(adverse_count_) : 0.0;
    r.avg_favorable_slippage = (favorable_count_ > 0)
        ? total_favorable_slippage_ / static_cast<double>(favorable_count_) : 0.0;
    r.adverse_slippage_count = adverse_count_;
    r.favorable_slippage_count = favorable_count_;

    // Tick-to-trade latency
    r.tick_to_trade_samples = static_cast<std::size_t>(tick_to_trade_ns_.n);
    r.avg_tick_to_trade_ns = tick_to_trade_ns_.mean;
    r.min_tick_to_trade_ns = tick_to_trade_min_ns_;
    r.max_tick_to_trade_ns = tick_to_trade_max_ns_;

    // Exposure
    r.time_in_market_pct = (market_events_total_ > 0)
        ? (static_cast<double>(market_events_in_position_) / static_cast<double>(market_events_total_)) * 100.0
        : 0.0;
    r.avg_holding_period_ms = (holding_count_ > 0) ? total_holding_ms_ / static_cast<double>(holding_count_) : 0.0;

    // Trade breakdown from running accumulators
    if (!trade_returns_.empty())
    {
        r.win_rate = static_cast<double>(win_count_) / static_cast<double>(trade_returns_.size()) * 100.0;
        r.avg_win = (win_count_ > 0) ? total_win_ / static_cast<double>(win_count_) : 0.0;
        std::size_t losses = trade_returns_.size() - win_count_;
        r.avg_loss = (losses > 0) ? total_loss_ / static_cast<double>(losses) : 0.0;
        r.profit_factor = (total_loss_ > 0.0) ? total_win_ / total_loss_ : (total_win_ > 0.0 ? 1e9 : 0.0);
        r.largest_winner = largest_winner_;
        r.largest_loser = largest_loser_;
    }

    // Per-bar risk-free rate for Sharpe/Sortino adjustment
    double rf_per_trade = 0.0;
    if (risk_free_rate_ != 0.0 && market_events_total_ > 0)
        rf_per_trade = risk_free_rate_ / static_cast<double>(market_events_total_);

    // Sharpe and Sortino from Welford accumulators (O(1), no iteration)
    if (return_stats_.n > 1)
    {
        double excess_mean = return_stats_.mean - rf_per_trade;
        r.sharpe_ratio = (return_stats_.stddev() > 0.0)
            ? excess_mean / return_stats_.stddev() : 0.0;
    }
    if (downside_stats_.n > 1)
    {
        double excess_mean = return_stats_.mean - rf_per_trade;
        r.sortino_ratio = (downside_stats_.stddev() > 0.0)
            ? excess_mean / downside_stats_.stddev() : 0.0;
    }
    else if (return_stats_.n > 1 && return_stats_.mean > 0.0)
    {
        r.sortino_ratio = 1e9;
    }

    // Max drawdown from running accumulator
    r.max_drawdown = max_drawdown_ * 100.0;

    // Calmar ratio
    r.calmar_ratio = (r.max_drawdown > 0.0)
        ? (r.cumulative_return * 100.0) / r.max_drawdown
        : 0.0;

    // Rolling metrics
    r.rolling_sharpe = rolling_sharpe();
    r.rolling_max_drawdown = rolling_max_drawdown() * 100.0;

    // Buy-and-hold benchmark
    if (first_price_set_ && last_close_ > 0.0 && first_price_ > 0.0)
        r.buy_and_hold_return = (last_close_ - first_price_) / first_price_;

    r.strategy_vs_benchmark = r.cumulative_return - r.buy_and_hold_return;

    // Alpha, beta, information ratio, tracking error
    if (strategy_returns_.size() > 1 && benchmark_returns_.size() == strategy_returns_.size())
    {
        std::size_t n = strategy_returns_.size();

        // Means
        double s_mean = 0.0, b_mean = 0.0;
        for (std::size_t i = 0; i < n; ++i)
        {
            s_mean += strategy_returns_[i];
            b_mean += benchmark_returns_[i];
        }
        s_mean /= static_cast<double>(n);
        b_mean /= static_cast<double>(n);

        // Covariance(strategy, benchmark) and Var(benchmark)
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

        // Tracking error and information ratio
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

    // Per-symbol and per-strategy attribution
    r.per_symbol = per_symbol_;
    r.per_strategy = per_strategy_;

    return r;
}

AnalyticsReport Analytics::generate_report() const
{
    // Start with snapshot (all scalar metrics)
    AnalyticsReport r = snapshot();

    // Add full vectors (only in final report, not mid-run snapshots)
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
    // Equity curve CSV
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

    // Trade log CSV
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
        R"({"initial_equity":%.2f,"final_equity":%.2f,"cumulative_return":%.6f,)"
        R"("sharpe_ratio":%.6f,"sortino_ratio":%.6f,"max_drawdown":%.6f,"calmar_ratio":%.6f,)"
        R"("rolling_sharpe":%.6f,"rolling_max_drawdown":%.6f,)"
        R"("win_rate":%.6f,"profit_factor":%.6f,"total_trades":%zu,)"
        R"("avg_win":%.6f,"avg_loss":%.6f,"largest_winner":%.6f,"largest_loser":%.6f,)"
        R"("time_in_market_pct":%.4f,"avg_slippage":%.6f,)"
        R"("buy_and_hold_return":%.6f,"strategy_vs_benchmark":%.6f,)"
        R"("alpha":%.6f,"beta":%.6f,"information_ratio":%.6f,"tracking_error":%.6f)",
        r.initial_equity, r.final_equity, r.cumulative_return,
        r.sharpe_ratio, r.sortino_ratio, r.max_drawdown, r.calmar_ratio,
        r.rolling_sharpe, r.rolling_max_drawdown,
        r.win_rate, r.profit_factor, r.total_trades,
        r.avg_win, r.avg_loss, r.largest_winner, r.largest_loser,
        r.time_in_market_pct, r.avg_slippage,
        r.buy_and_hold_return, r.strategy_vs_benchmark,
        r.alpha, r.beta, r.information_ratio, r.tracking_error);

    f << "{" << (buf + 1);  // skip the leading '{' from buf since we write our own

    // Equity curve array
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

    // Trade log array
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

    // Per-symbol attribution
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

    // Per-strategy attribution
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
