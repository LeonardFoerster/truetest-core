#include "ui/desk/desk_view_model.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace truetest::ui::desk {
namespace {

constexpr double kEpsilon = 1e-12;

std::optional<double> usable(bool available, double value)
{
    return available && std::isfinite(value) ? std::optional<double>{value} : std::nullopt;
}

std::optional<double> usable_price(double value)
{
    return value > 0.0 && std::isfinite(value) ? std::optional<double>{value} : std::nullopt;
}

std::optional<double> reference_price(const MarketWatchRow& market)
{
    if (market.mark) return market.mark;
    return market.mid;
}

}  // namespace

void sort_market_watch(std::vector<MarketWatchRow>& rows, MarketWatchSort sort, bool descending)
{
    std::stable_sort(
        rows.begin(), rows.end(), [sort, descending](const auto& left, const auto& right) {
            const auto compare_key = [&](std::optional<double> lhs,
                                         std::optional<double> rhs) {
                // Missing/non-finite values are always last. A symbol
                // tie-breaker makes the relation total and keeps NaN out of
                // std::stable_sort's strict-weak-order precondition.
                if (lhs.has_value() != rhs.has_value()) return lhs.has_value();
                if (lhs && rhs && *lhs != *rhs)
                    return descending ? *lhs > *rhs : *lhs < *rhs;
                return left.symbol < right.symbol;
            };
            switch (sort) {
            case MarketWatchSort::mark:
                return compare_key(left.mark, right.mark);
            case MarketWatchSort::spread_bps:
                return compare_key(left.spread_bps, right.spread_bps);
            case MarketWatchSort::position: {
                const auto magnitude = [](double quantity) -> std::optional<double> {
                    return std::isfinite(quantity)
                        ? std::optional<double>{std::abs(quantity)}
                        : std::nullopt;
                };
                return compare_key(magnitude(left.position_qty),
                                   magnitude(right.position_qty));
            }
            case MarketWatchSort::symbol:
                return descending ? left.symbol > right.symbol : left.symbol < right.symbol;
            }
            return false;
        });
}

CommandCenterViewModel build_command_center_view_model(const dashboard_snapshot& snapshot,
                                                       const DeskState& state,
                                                       std::chrono::steady_clock::time_point now)
{
    CommandCenterViewModel view;
    view.selected_symbol = state.selected_symbol;
    view.account.cash = snapshot.cash;
    view.account.initial_balance = snapshot.initial_balance;
    view.account.equity = usable(snapshot.equity_available, snapshot.equity);
    view.account.total_pnl = usable(snapshot.total_pnl_available, snapshot.total_pnl);
    view.account.realized_pnl = usable(snapshot.realized_pnl_available, snapshot.realized_pnl);
    view.account.unrealized_pnl =
        usable(snapshot.unrealized_pnl_available, snapshot.unrealized_pnl);
    view.account.gross_exposure = usable(snapshot.risk.exposure_available, snapshot.risk.exposure);
    if (view.account.gross_exposure && view.account.equity && *view.account.equity > 0.0)
        view.account.effective_leverage = *view.account.gross_exposure / *view.account.equity;
    view.account.current_drawdown_pct =
        usable(snapshot.trend.drawdown_now_available, snapshot.trend.drawdown_now_pct);
    view.account.max_drawdown_pct =
        usable(snapshot.risk.max_drawdown_available, snapshot.risk.max_drawdown_pct);

    std::unordered_map<std::string, MarketWatchRow> markets;
    markets.reserve(snapshot.market_rows.size());
    for (const auto& row : snapshot.market_rows) {
        MarketWatchRow projected;
        projected.symbol = row.symbol;
        projected.mark = usable(row.mark_available, row.mark);
        projected.bid = usable(row.best_bid_available, row.best_bid);
        projected.ask = usable(row.best_ask_available, row.best_ask);
        projected.mid = usable(row.bbo_available, row.mid);
        projected.spread = usable(row.bbo_available, row.spread);
        projected.spread_bps = usable(row.bbo_available, row.spread_bps);
        projected.microprice = usable(row.microprice_available, row.microprice);
        if (const auto imbalance = usable(row.imbalance_available, row.imbalance))
            projected.imbalance_pct = *imbalance * 100.0;
        projected.position_qty = row.position_qty;
        projected.working_buy_orders = row.working_buy_orders;
        projected.working_sell_orders = row.working_sell_orders;
        projected.selected = row.symbol == state.selected_symbol;
        markets.emplace(projected.symbol, std::move(projected));
    }
    for (const auto& [_, row] : markets) {
        // Orders use this lookup map just below to calculate their distance
        // from the same market reference.  Keep its values valid instead of
        // moving them out during this cold-path projection.
        view.market_watch.push_back(row);
    }
    sort_market_watch(view.market_watch, state.market_sort, state.market_sort_descending);

    for (const auto& row : snapshot.positions) {
        PositionViewRow projected;
        projected.symbol = row.symbol;
        projected.quantity = row.qty;
        projected.break_even_price = usable_price(row.avg_entry);
        projected.mark = usable(row.mark_available, row.mark);
        if (projected.mark && std::isfinite(row.qty))
            projected.notional = std::abs(row.qty) * *projected.mark;
        projected.unrealized_pnl = usable(row.unrealized_available, row.unrealized);
        const double basis = std::abs(row.qty * row.avg_entry);
        if (projected.unrealized_pnl && basis > kEpsilon)
            projected.unrealized_pnl_pct = *projected.unrealized_pnl / basis * 100.0;
        projected.selected = row.symbol == state.selected_symbol;
        view.positions.push_back(std::move(projected));
    }

    for (const auto& row : snapshot.open_orders) {
        OrderViewRow projected;
        projected.order_id = row.order_id;
        projected.symbol = row.symbol;
        projected.side = row.side;
        projected.type = row.type;
        projected.quantity = row.qty;
        // Market orders do not have a meaningful resting price.
        if (row.type != 'M') projected.price = usable_price(row.price);
        projected.trigger_price =
            usable(row.trigger_price_available, row.trigger_price);
        const auto distance_price =
            (row.type == 'S' || row.type == 's') && projected.trigger_price
                ? projected.trigger_price
                : projected.price;
        if (distance_price) {
            const auto market = markets.find(row.symbol);
            if (market != markets.end()) {
                if (const auto reference = reference_price(market->second);
                    reference && *reference > 0.0)
                    projected.distance_bps = (*distance_price - *reference) / *reference * 1e4;
            }
        }
        projected.age_seconds =
            row.age_seconds >= 0 ? std::optional<std::int64_t>{row.age_seconds} : std::nullopt;
        projected.strategy = row.strategy_name;
        projected.status = row.status ? row.status : "";
        projected.selected = row.order_id != 0 && row.order_id == state.selected_order_id;
        view.orders.push_back(std::move(projected));
    }

    for (const auto& row : snapshot.brackets) {
        ProtectionViewRow projected;
        projected.opener_order_id = row.opener_order_id;
        projected.symbol = row.symbol;
        projected.side = row.side;
        projected.quantity = row.qty;
        projected.entry = usable_price(row.entry_price);
        projected.mark = usable_price(row.mark);
        if (row.stop_loss)
            projected.stop_loss = usable_price(*row.stop_loss);
        if (row.take_profit)
            projected.take_profit = usable_price(*row.take_profit);
        if (projected.mark && projected.stop_loss) {
            const double direction = row.side == 'S' ? -1.0 : 1.0;
            projected.distance_to_stop_bps =
                direction * (*projected.mark - *projected.stop_loss) / *projected.mark * 1e4;
        }
        if (projected.mark && projected.take_profit) {
            const double direction = row.side == 'S' ? -1.0 : 1.0;
            projected.distance_to_take_profit_bps =
                direction * (*projected.take_profit - *projected.mark) / *projected.mark * 1e4;
        }
        projected.venue_managed = row.venue_managed;
        projected.age_seconds =
            row.age_seconds >= 0 ? std::optional<std::int64_t>{row.age_seconds} : std::nullopt;
        view.protection.push_back(std::move(projected));
    }

    for (const auto& row : snapshot.recent_fills) {
        view.fills.push_back({
            .timestamp = row.ts,
            .symbol = row.symbol,
            .side = row.side,
            .quantity = row.qty,
            .price = row.price,
            .fee = row.fee,
            .source = row.source ? row.source : "",
        });
    }

    if (snapshot.generated_at_available && now >= snapshot.generated_at) {
        view.snapshot_age_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - snapshot.generated_at)
                .count();
        // This deliberately signals an aging coherent engine update, not a
        // fabricated provider last-event age. A stalled refresh is still
        // operationally important and must not read as healthy.
        view.snapshot_stale = *view.snapshot_age_ms > 2'000;
    }
    return view;
}

}  // namespace truetest::ui::desk
