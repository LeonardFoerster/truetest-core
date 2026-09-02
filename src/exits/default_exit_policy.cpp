#include "exits/default_exit_policy.h"

#include <cmath>
#include <utility>

namespace truetest::exits {
namespace {

constexpr double kQtyEps = 1e-12;

bool platform_has_any_protection(const default_exit_params& p)
{
    return p.sl_pct > 0.0 || p.tp_pct > 0.0 || p.trail_pct > 0.0;
}

void apply_trail(exit_intent& ei, const default_exit_params& p)
{
    if (p.trail_pct > 0.0)
        ei.trailing_pct = p.trail_pct;
}

} // namespace

bool is_position_reducing(const order_event& order, double net_qty)
{
    // Explicit closer for a prior opener.
    const auto oid = order.get_order_id();
    const auto opener = order.get_opener_order_id();
    if (opener != 0 && (oid == 0 || opener != oid))
        return true;

    // An un-attributed opposite-side order can cross through flat. Only its
    // closing portion is position-reducing; the residual opens exposure and
    // must receive the configured platform safety floor.
    const double qty = order.get_quantity();
    if (net_qty > kQtyEps && order.get_side() == order_side::sell)
        return qty <= net_qty + kQtyEps;
    if (net_qty < -kQtyEps && order.get_side() == order_side::buy)
        return qty <= -net_qty + kQtyEps;
    return false;
}

bool intents_have_stop_loss(const std::vector<exit_intent>& intents)
{
    for (const auto& ei : intents)
        if (ei.stop_loss.has_value())
            return true;
    return false;
}

bool intents_have_take_profit(const std::vector<exit_intent>& intents)
{
    for (const auto& ei : intents)
        if (ei.take_profit.has_value())
            return true;
    return false;
}

bool intents_have_trail(const std::vector<exit_intent>& intents)
{
    for (const auto& ei : intents)
        if (ei.trailing_pct.has_value())
            return true;
    return false;
}

std::optional<exit_intent> make_platform_exit_intent(const order_event& order,
                                                     const default_exit_params& p)
{
    if (!platform_has_any_protection(p))
        return std::nullopt;

    const double entry = order.get_price();
    const double qty = order.get_quantity();
    if (!(entry > 0.0) || !(qty > 0.0) || !std::isfinite(entry) || !std::isfinite(qty))
        return std::nullopt;

    const bool is_long = (order.get_side() == order_side::buy);
    exit_intent ei;
    if (is_long)
    {
        ei = make_long_exit_intent(order.get_symbol(), entry, qty,
                                   p.sl_pct > 0.0 ? p.sl_pct : 0.0,
                                   p.tp_pct > 0.0 ? p.tp_pct : 0.0);
        // make_* leaves unset optionals when pct==0 only if we pass 0 —
        // helpers set only when sl_pct > 0 / tp_pct > 0.
    }
    else
    {
        ei = make_short_exit_intent(order.get_symbol(), entry, qty,
                                    p.sl_pct > 0.0 ? p.sl_pct : 0.0,
                                    p.tp_pct > 0.0 ? p.tp_pct : 0.0);
    }

    // If both SL and TP disabled, helpers return intent with neither set —
    // still useful for trail-only.
    if (p.sl_pct <= 0.0)
        ei.stop_loss.reset();
    if (p.tp_pct <= 0.0)
        ei.take_profit.reset();

    apply_trail(ei, p);

    if (!ei.stop_loss && !ei.take_profit && !ei.trailing_pct)
        return std::nullopt;

    return ei;
}

std::vector<exit_intent> apply_default_exit_policy(
    const default_exit_params& p,
    const order_event& order,
    double net_qty,
    std::vector<exit_intent> strategy_intents)
{
    if (p.mode == exit_policy_mode::strategy_only)
        return strategy_intents;

    // Never arm platform protection on position-reducing (signal) closes.
    if (is_position_reducing(order, net_qty))
    {
        if (p.mode == exit_policy_mode::engine_only)
            return {}; // drop strategy intents on closers too under engine_only
        return strategy_intents;
    }

    if (!strategy_intents.empty())
    {
        if (p.mode == exit_policy_mode::engine_only)
        {
            auto platform = make_platform_exit_intent(order, p);
            if (platform)
                return {*platform};
            return {};
        }
        if (p.mode == exit_policy_mode::union_mode)
        {
            default_exit_params missing_p;
            missing_p.mode = exit_policy_mode::union_mode;
            missing_p.sl_pct = (p.sl_pct > 0.0 && !intents_have_stop_loss(strategy_intents)) ? p.sl_pct : 0.0;
            missing_p.tp_pct = (p.tp_pct > 0.0 && !intents_have_take_profit(strategy_intents)) ? p.tp_pct : 0.0;
            missing_p.trail_pct = (p.trail_pct > 0.0 && !intents_have_trail(strategy_intents)) ? p.trail_pct : 0.0;

            if (auto platform_missing = make_platform_exit_intent(order, missing_p))
            {
                strategy_intents.push_back(std::move(*platform_missing));
            }
        }
        return strategy_intents;
    }

    // Strategy provided no intents: only arm if platform protection is explicitly configured
    auto platform = make_platform_exit_intent(order, p);
    if (platform)
        return {*platform};
    return {};
}

std::optional<exit_policy_mode> parse_exit_policy_mode(std::string_view s)
{
    if (s == "floor") return exit_policy_mode::floor;
    if (s == "strategy_only" || s == "strategy-only") return exit_policy_mode::strategy_only;
    if (s == "engine_only" || s == "engine-only") return exit_policy_mode::engine_only;
    if (s == "union") return exit_policy_mode::union_mode;
    return std::nullopt;
}

const char* exit_policy_mode_name(exit_policy_mode m)
{
    switch (m)
    {
    case exit_policy_mode::floor: return "floor";
    case exit_policy_mode::strategy_only: return "strategy_only";
    case exit_policy_mode::engine_only: return "engine_only";
    case exit_policy_mode::union_mode: return "union";
    }
    return "floor";
}

} // namespace truetest::exits
