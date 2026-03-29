#include "analytics.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>

Analytics::Analytics(double initial_cash)
    : initial_cash_(initial_cash), cash_(initial_cash), peak_equity_(initial_cash) {}

void Analytics::on_event(const event_pointer& ev)
{
    switch (ev->get_type())
    {
        case event_type::market:
            on_market(*std::static_pointer_cast<market_event>(ev));
            break;
        case event_type::order:
            on_order(*std::static_pointer_cast<order_event>(ev));
            break;
        case event_type::fill:
            on_fill(*std::static_pointer_cast<fill_event>(ev));
            break;
        case event_type::signal:
        case event_type::tick:
        case event_type::l2_snapshot:
        case event_type::l2_update:
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
    if (in_position_)
        market_events_in_position_++;

    // Record equity: cash + mark-to-market of open position
    double equity = cash_;
    if (in_position_)
        equity += position_qty_ * last_close_;

    equity_curve_.push_back({m.get_timestamp(), equity});

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

void Analytics::on_order(const order_event& o)
{
    order_prices_[o.get_order_id()] = o.get_price();
    total_orders_++;
}

void Analytics::on_fill(const fill_event& f)
{
    total_fills_++;

    // Slippage: intended vs actual fill price
    double intended = 0.0;
    auto it = order_prices_.find(f.get_order_id());
    if (it != order_prices_.end())
    {
        intended = it->second;
        double slip = std::abs(f.get_fill_price() - intended);
        total_slippage_ += slip;
        slippage_count_++;
    }

    trade_record rec;
    rec.order_id = f.get_order_id();
    rec.side = f.get_side();
    rec.quantity = f.get_filled_quantity();
    rec.fill_price = f.get_fill_price();
    rec.commission = f.get_commission();
    rec.intended_price = intended;
    rec.timestamp = f.get_timestamp();
    rec.pnl = 0.0;

    if (f.get_side() == order_side::buy && !in_position_)
    {
        in_position_ = true;
        entry_price_ = f.get_fill_price();
        position_qty_ = f.get_filled_quantity();
        entry_time_ = f.get_timestamp();
        cash_ -= f.get_total_cost();
    }
    else if (f.get_side() == order_side::sell && in_position_)
    {
        double exit_cost = f.get_total_cost();
        double entry_cost = position_qty_ * entry_price_;
        double pnl = exit_cost - entry_cost - f.get_commission();

        // Find the matching buy's commission from trades
        for (auto rit = trades_.rbegin(); rit != trades_.rend(); ++rit)
        {
            if (rit->side == order_side::buy && rit->order_id != f.get_order_id())
            {
                pnl -= rit->commission;
                break;
            }
        }

        rec.pnl = pnl;
        trade_returns_.push_back(pnl);

        // Update streaming accumulators
        return_stats_.update(pnl);
        if (pnl < 0.0)
            downside_stats_.update(pnl);

        if (pnl > 0.0)
        {
            win_count_++;
            total_win_ += pnl;
            if (pnl > largest_winner_)
                largest_winner_ = pnl;
        }
        else
        {
            total_loss_ += std::abs(pnl);
            if (pnl < largest_loser_)
                largest_loser_ = pnl;
        }

        // Holding period
        auto hold_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            f.get_timestamp() - entry_time_).count();
        total_holding_ms_ += static_cast<double>(hold_ms);
        holding_count_++;

        cash_ += f.get_total_cost();
        in_position_ = false;
        position_qty_ = 0;
        entry_price_ = 0.0;
    }

    trades_.push_back(rec);
}

AnalyticsReport Analytics::snapshot() const
{
    AnalyticsReport r;
    r.initial_equity = initial_cash_;
    r.final_equity = cash_;
    if (in_position_)
        r.final_equity += position_qty_ * last_close_;

    r.cumulative_return = (r.final_equity - initial_cash_) / initial_cash_;
    r.total_orders = total_orders_;
    r.total_fills = total_fills_;
    r.total_trades = trade_returns_.size();

    // Slippage
    r.avg_slippage = (slippage_count_ > 0) ? total_slippage_ / static_cast<double>(slippage_count_) : 0.0;

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

    // Sharpe and Sortino from Welford accumulators (O(1), no iteration)
    if (return_stats_.n > 1)
    {
        r.sharpe_ratio = (return_stats_.stddev() > 0.0)
            ? return_stats_.mean / return_stats_.stddev() : 0.0;
    }
    if (downside_stats_.n > 1)
    {
        r.sortino_ratio = (downside_stats_.stddev() > 0.0)
            ? return_stats_.mean / downside_stats_.stddev() : 0.0;
    }
    else if (return_stats_.n > 1 && return_stats_.mean > 0.0)
    {
        // All trades profitable → no downside deviation → effectively infinite Sortino.
        // Cap at 1e9 to avoid JSON/display issues while still ranking correctly.
        r.sortino_ratio = 1e9;
    }

    // Max drawdown from running accumulator
    r.max_drawdown = max_drawdown_ * 100.0;

    // Calmar ratio
    r.calmar_ratio = (r.max_drawdown > 0.0)
        ? (r.cumulative_return * 100.0) / r.max_drawdown
        : 0.0;

    // Buy-and-hold benchmark
    if (first_price_set_ && last_close_ > 0.0 && first_price_ > 0.0)
        r.buy_and_hold_return = (last_close_ - first_price_) / first_price_;

    r.strategy_vs_benchmark = r.cumulative_return - r.buy_and_hold_return;

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

    return r;
}

void Analytics::print_report() const
{
    auto r = generate_report();

    std::cout << "\n";
    std::cout << "  ============================================\n";
    std::cout << "    Analytics Report\n";
    std::cout << "  ============================================\n\n";

    std::cout << std::fixed << std::setprecision(2);

    std::cout << "  Returns\n";
    std::cout << "  -------\n";
    std::cout << "    Initial Equity:      " << r.initial_equity << "\n";
    std::cout << "    Final Equity:        " << r.final_equity << "\n";
    std::cout << "    Cumulative Return:   " << r.cumulative_return * 100.0 << "%\n";
    std::cout << "    Buy & Hold Return:   " << r.buy_and_hold_return * 100.0 << "%\n";
    std::cout << "    Strategy vs B&H:     " << r.strategy_vs_benchmark * 100.0 << "%\n";
    std::cout << "\n";

    std::cout << "  Risk\n";
    std::cout << "  ----\n";
    std::cout << std::setprecision(4);
    std::cout << "    Sharpe Ratio:        " << r.sharpe_ratio << "\n";
    std::cout << "    Sortino Ratio:       " << r.sortino_ratio << "\n";
    std::cout << std::setprecision(2);
    std::cout << "    Max Drawdown:        " << r.max_drawdown << "%\n";
    std::cout << std::setprecision(4);
    std::cout << "    Calmar Ratio:        " << r.calmar_ratio << "\n";
    std::cout << "\n";

    std::cout << "  Execution Quality\n";
    std::cout << "  -----------------\n";
    std::cout << std::setprecision(6);
    std::cout << "    Avg Slippage:        " << r.avg_slippage << "\n";
    std::cout << "    Total Orders:        " << r.total_orders << "\n";
    std::cout << "    Total Fills:         " << r.total_fills << "\n";
    std::cout << "\n";

    std::cout << "  Exposure\n";
    std::cout << "  --------\n";
    std::cout << std::setprecision(2);
    std::cout << "    Time in Market:      " << r.time_in_market_pct << "%\n";
    std::cout << "    Avg Holding Period:  " << r.avg_holding_period_ms << " ms\n";
    std::cout << "\n";

    std::cout << "  Trade Breakdown\n";
    std::cout << "  ---------------\n";
    std::cout << "    Total Trades:        " << r.total_trades << "\n";
    std::cout << "    Win Rate:            " << r.win_rate << "%\n";
    std::cout << "    Avg Win:             " << r.avg_win << "\n";
    std::cout << "    Avg Loss:            " << r.avg_loss << "\n";
    std::cout << std::setprecision(4);
    std::cout << "    Profit Factor:       " << r.profit_factor << "\n";
    std::cout << std::setprecision(2);
    std::cout << "    Largest Winner:      " << r.largest_winner << "\n";
    std::cout << "    Largest Loser:       " << r.largest_loser << "\n";
    std::cout << "\n";
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
        f << "timestamp_ms,order_id,side,quantity,fill_price,intended_price,commission,pnl\n";
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
              << std::setprecision(2) << t.pnl << "\n";
        }
    }
}
