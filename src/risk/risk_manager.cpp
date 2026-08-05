#include "risk_manager.h"
#include "../analytics/analytics.h"

#include <cmath>

namespace {

constexpr double kQtyEps = 1e-12;

double signed_order_quantity(const order_event& order)
{
    return (order.get_side() == order_side::buy)
        ? order.get_quantity()
        : -order.get_quantity();
}

double position_price(const position& pos)
{
    return std::abs(pos.qty) > kQtyEps
        ? std::abs(pos.cost_basis / pos.qty)
        : 0.0;
}

double valuation_price(const order_event& order, const position* pos)
{
    if (order.get_price() > 0.0)
        return order.get_price();
    return pos ? position_price(*pos) : 0.0;
}

double projected_position_notional(const order_event& order,
                                   const position* pos)
{
    const double current_qty = pos ? pos->qty : 0.0;
    const double projected_qty = current_qty + signed_order_quantity(order);
    const double price = valuation_price(order, pos);
    return std::abs(projected_qty) * price;
}

bool reduces_position_exposure(const order_event& order, const position* pos)
{
    if (!pos)
        return false;
    const double current_qty = pos->qty;
    const double projected_qty = current_qty + signed_order_quantity(order);
    return std::abs(projected_qty) < std::abs(current_qty) - kQtyEps;
}

} // namespace

RiskManager::RiskManager(risk_limits limits)
    : limits_(std::move(limits)) {}

void RiskManager::prune_old_entries(std::deque<timestamped_entry>& entries,
                                    std::chrono::system_clock::time_point cutoff)
{
    while (!entries.empty() && entries.front().ts < cutoff)
        entries.pop_front();
}

void RiskManager::update_daily_reset(std::chrono::system_clock::time_point now)
{
    if (daily_reset_tp_ == std::chrono::system_clock::time_point{})
    {
        auto tt = std::chrono::system_clock::to_time_t(now);
        std::tm utc{};
        gmtime_r(&tt, &utc);
        utc.tm_hour = limits_.daily_reset_hour;
        utc.tm_min = 0;
        utc.tm_sec = 0;
        auto boundary = std::chrono::system_clock::from_time_t(timegm(&utc));
        if (boundary <= now)
            boundary += std::chrono::hours(24);
        daily_reset_tp_ = boundary;
    }

    if (now >= daily_reset_tp_)
    {
        daily_loss_ = 0.0;
        daily_start_equity_ = 0.0;
        while (daily_reset_tp_ <= now)
            daily_reset_tp_ += std::chrono::hours(24);
    }
}

risk_action RiskManager::check_order(const order_event& order,
                                     const portfolio& port,
                                     const risk_snapshot& snap)
{
    const auto& positions = port.get_positions();
    const auto it = positions.find(order.get_symbol());
    const position* current_pos = (it != positions.end()) ? &it->second : nullptr;
    const bool reducing_exposure = reduces_position_exposure(order, current_pos);
    const double projected_notional = projected_position_notional(order, current_pos);

    // Portfolio DD: block new risk only. Reduce-only / exit orders must still
    // pass so inventory can be flattened after a breach (soft backtest and
    // live halt paths both need this — otherwise exits are rejected too).
    if (!reducing_exposure &&
        snap.max_drawdown / 100.0 >= limits_.max_drawdown)
        return risk_action::halt;

    if (static_cast<int>(snap.total_orders - snap.total_fills) >= limits_.max_open_orders)
        return risk_action::reject;

    if (!reducing_exposure &&
        projected_notional > limits_.max_position_value)
        return risk_action::reject;

    // Phase 2.3 - max position as % of equity
    if (!reducing_exposure &&
        limits_.max_position_pct_of_equity > 0.0 && snap.equity > 0.0) {
        double max_notional = snap.equity * limits_.max_position_pct_of_equity;
        if (projected_notional > max_notional)
            return risk_action::reject;
    }

    double projected_total_exposure = 0.0;
    for (const auto& [sym, pos] : positions)
    {
        if (sym == order.get_symbol())
            projected_total_exposure += projected_notional;
        else
            projected_total_exposure += std::abs(pos.cost_basis);
    }
    if (it == positions.end())
        projected_total_exposure += projected_notional;

    if (!reducing_exposure &&
        projected_total_exposure > limits_.max_portfolio_exposure)
        return risk_action::reject;

    // Phase 2.3 - portfolio-wide % of equity
    if (!reducing_exposure &&
        limits_.max_position_pct_of_equity > 0.0 && snap.equity > 0.0) {
        double max_portfolio_notional = snap.equity * limits_.max_position_pct_of_equity;
        if (projected_total_exposure > max_portfolio_notional)
            return risk_action::reject;
    }

    // Phase 2.4 - spread circuit breaker (populated in Analytics from L2 snapshots when --depth-stream is active)
    if (limits_.max_spread_bps > 0.0 && snap.current_spread_bps > limits_.max_spread_bps) {
        // Severe breaches (e.g. > 2x limit) escalate to halt to stop trading in obviously broken books
        if (snap.current_spread_bps > limits_.max_spread_bps * 2.0) {
            return risk_action::halt;
        }
        return risk_action::reject;
    }

    // Funding rate circuit breaker (rate can be fed via Analytics::set_current_funding_rate_8h
    // from the provider when ACCOUNT_UPDATE or dedicated funding rate messages are parsed)
    if (limits_.max_funding_8h_rate > 0.0 && snap.current_funding_8h_rate > limits_.max_funding_8h_rate) {
        if (snap.current_funding_8h_rate > limits_.max_funding_8h_rate * 1.5) {
            return risk_action::halt;
        }
        return risk_action::reject;
    }

    if (limits_.max_orders_per_minute > 0)
    {
        auto cutoff = order.get_timestamp() - std::chrono::seconds(60);
        prune_old_entries(order_timestamps_, cutoff);
        if (static_cast<int>(order_timestamps_.size()) >= limits_.max_orders_per_minute)
            return risk_action::reject;
        order_timestamps_.push_back({order.get_timestamp()});
    }

    if (!reducing_exposure && limits_.max_daily_loss > 0.0)
    {
        update_daily_reset(order.get_timestamp());
        if (daily_loss_ >= limits_.max_daily_loss)
            return risk_action::halt;
    }

    return risk_action::pass;
}

risk_action RiskManager::check_post_fill(const fill_event& fill,
                                         const portfolio& /* port */,
                                         const risk_snapshot& snap)
{
    if (snap.max_drawdown / 100.0 >= limits_.max_drawdown)
        return risk_action::halt;

    if (snap.has_last_trade &&
        snap.last_trade_pnl < -limits_.max_loss_per_trade)
        return risk_action::halt;

    if (limits_.max_trades_per_hour > 0 &&
        static_cast<int>(trade_timestamps_.size()) >= limits_.max_trades_per_hour)
        return risk_action::halt;

    if (limits_.max_daily_loss > 0.0)
    {
        update_daily_reset(fill.get_timestamp());
        // Include realized trading losses. Analytics trade PnL is already net
        // of opening/closing commissions, so do not add fill commissions again.
        if (snap.has_last_trade && snap.last_trade_seq != 0 &&
            snap.last_trade_pnl < 0.0 &&
            snap.last_trade_seq != last_daily_trade_seq_added_)
        {
            daily_loss_ += -snap.last_trade_pnl;
            last_daily_trade_seq_added_ = snap.last_trade_seq;
        }
        if (daily_loss_ >= limits_.max_daily_loss)
            return risk_action::halt;
    }

    return risk_action::pass;
}

// Legacy overloads - collapse the heavy AnalyticsReport to the thin
// risk_snapshot and dispatch into the real path so logic lives in one
// place.
risk_action RiskManager::check_order(const order_event& order,
                                     const portfolio& port,
                                     const AnalyticsReport& snap)
{
    risk_snapshot rs;
    rs.max_drawdown = snap.max_drawdown;
    rs.total_orders = snap.total_orders;
    rs.total_fills  = snap.total_fills;
    // Phase 2 fields (best effort from full report; modern path uses risk_snapshot directly)
    rs.equity = snap.final_equity;  // approximate
    return check_order(order, port, rs);
}

risk_action RiskManager::check_post_fill(const fill_event& fill,
                                         const portfolio& port,
                                         const AnalyticsReport& snap)
{
    risk_snapshot rs;
    rs.max_drawdown = snap.max_drawdown;
    if (!snap.trades.empty())
	    {
	        rs.has_last_trade = true;
	        rs.last_trade_pnl = snap.trades.back().pnl;
	        rs.last_trade_seq = snap.trades.size();
	    }
    rs.equity = snap.final_equity;
    return check_post_fill(fill, port, rs);
}

void RiskManager::on_fill(const fill_event& fill)
{
    if (limits_.max_trades_per_hour > 0)
    {
        auto cutoff = fill.get_timestamp() - std::chrono::hours(1);
        prune_old_entries(trade_timestamps_, cutoff);
        trade_timestamps_.push_back({fill.get_timestamp()});
    }

    if (limits_.max_daily_loss > 0.0)
        update_daily_reset(fill.get_timestamp());
}

// Phase A (MC object reuse)
void RiskManager::reset()
{
    order_timestamps_.clear();
    trade_timestamps_.clear();
    daily_loss_ = 0.0;
    daily_start_equity_ = 0.0;
    daily_reset_tp_ = {};
    last_daily_trade_seq_added_ = 0;
}
