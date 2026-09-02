#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace truetest::ui::desk {

// Deliberately separate from ui::operator_actions. These are future
// per-position/per-order intents, not a path around the attended global
// pause/flatten/kill safety seam. DeskApp receives an empty instance today.
struct ClosePositionRequest
{
    std::string symbol;
    std::optional<double> quantity;
};

struct ProtectionRequest
{
    std::string symbol;
    std::optional<double> stop_loss;
    std::optional<double> take_profit;
};

struct CancelOrderRequest
{
    std::uint64_t order_id = 0;
    std::string symbol;
};

struct AmendOrderRequest
{
    std::uint64_t order_id = 0;
    std::string symbol;
    std::optional<double> quantity;
    std::optional<double> price;
};

struct DeskTradeActions
{
    std::function<void(const ClosePositionRequest&)> close_position;
    std::function<void(const ProtectionRequest&)> change_protection;
    std::function<void(const CancelOrderRequest&)> cancel_order;
    std::function<void(const AmendOrderRequest&)> amend_order;
};

struct DeskTradeActionCapabilities
{
    bool close_position = false;
    bool change_protection = false;
    bool cancel_order = false;
    bool amend_order = false;
};

inline DeskTradeActionCapabilities
derive_trade_action_capabilities(const DeskTradeActions& actions) noexcept
{
    return {
        .close_position = static_cast<bool>(actions.close_position),
        .change_protection = static_cast<bool>(actions.change_protection),
        .cancel_order = static_cast<bool>(actions.cancel_order),
        .amend_order = static_cast<bool>(actions.amend_order),
    };
}

}  // namespace truetest::ui::desk
