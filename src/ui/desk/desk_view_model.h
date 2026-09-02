#pragma once

#include "ui/dashboard_snapshot.h"
#include "ui/desk/desk_state.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace truetest::ui::desk {

struct MarketWatchRow
{
    std::string symbol;
    std::optional<double> mark;
    std::optional<double> bid;
    std::optional<double> ask;
    std::optional<double> mid;
    std::optional<double> spread;
    std::optional<double> spread_bps;
    std::optional<double> microprice;
    // Display unit, not the snapshot's [-1, 1] ratio.
    std::optional<double> imbalance_pct;
    double position_qty = 0.0;
    std::size_t working_buy_orders = 0;
    std::size_t working_sell_orders = 0;
    bool selected = false;
};

struct PositionViewRow
{
    std::string symbol;
    double quantity = 0.0;
    std::optional<double> break_even_price;
    std::optional<double> mark;
    std::optional<double> notional;
    std::optional<double> unrealized_pnl;
    std::optional<double> unrealized_pnl_pct;
    bool selected = false;
};

struct OrderViewRow
{
    std::uint64_t order_id = 0;
    std::string symbol;
    char side = '?';
    char type = '?';
    double quantity = 0.0;
    std::optional<double> price;
    std::optional<double> trigger_price;
    std::optional<double> distance_bps;
    std::optional<std::int64_t> age_seconds;
    std::string strategy;
    std::string status;
    bool selected = false;
};

struct ProtectionViewRow
{
    std::uint64_t opener_order_id = 0;
    std::string symbol;
    char side = '?';
    double quantity = 0.0;
    std::optional<double> entry;
    std::optional<double> mark;
    std::optional<double> stop_loss;
    std::optional<double> take_profit;
    std::optional<double> distance_to_stop_bps;
    std::optional<double> distance_to_take_profit_bps;
    bool venue_managed = false;
    std::optional<std::int64_t> age_seconds;
};

struct FillViewRow
{
    std::chrono::system_clock::time_point timestamp{};
    std::string symbol;
    char side = '?';
    double quantity = 0.0;
    double price = 0.0;
    double fee = 0.0;
    std::string source;
};

struct AccountView
{
    double cash = 0.0;
    double initial_balance = 0.0;
    std::optional<double> equity;
    std::optional<double> total_pnl;
    std::optional<double> realized_pnl;
    std::optional<double> unrealized_pnl;
    std::optional<double> gross_exposure;
    std::optional<double> effective_leverage;
    std::optional<double> current_drawdown_pct;
    std::optional<double> max_drawdown_pct;
};

struct CommandCenterViewModel
{
    AccountView account;
    std::vector<MarketWatchRow> market_watch;
    std::vector<PositionViewRow> positions;
    std::vector<OrderViewRow> orders;
    std::vector<ProtectionViewRow> protection;
    std::vector<FillViewRow> fills;

    std::string selected_symbol;
    std::optional<std::int64_t> snapshot_age_ms;
    bool snapshot_stale = false;
};

CommandCenterViewModel build_command_center_view_model(
    const dashboard_snapshot& snapshot, const DeskState& state,
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

void sort_market_watch(std::vector<MarketWatchRow>& rows, MarketWatchSort sort, bool descending);

}  // namespace truetest::ui::desk
