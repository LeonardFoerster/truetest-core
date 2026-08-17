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

    if (net_qty > kQtyEps && order.get_side() == order_side::sell)
        return true;
    if (net_qty < -kQtyEps && order.get_side() == order_side::buy)
        return true;
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

    auto platform = make_platform_exit_intent(order, p);

    if (p.mode == exit_policy_mode::engine_only)
    {
        if (platform)
            return {*platform};
        return {};
    }

    if (p.mode == exit_policy_mode::union_mode)
    {
        if (!platform)
            return strategy_intents;
        if (strategy_intents.empty())
            return {*platform};

        bool have_sl = false;
        bool have_tp = false;
        bool have_trail = false;
        for (const auto& intent : strategy_intents)
        {
            have_sl = have_sl || intent.stop_loss.has_value();
            have_tp = have_tp || intent.take_profit.has_value();
            have_trail = have_trail || intent.trailing_pct.has_value();
        }

        exit_intent missing = std::move(*platform);
        if (have_sl) missing.stop_loss.reset();
        if (have_tp) missing.take_profit.reset();
        if (have_trail) missing.trailing_pct.reset();
        if (missing.stop_loss || missing.take_profit || missing.trailing_pct)
            strategy_intents.push_back(std::move(missing));
        return strategy_intents;
    }

    // floor
    if (strategy_intents.empty())
    {
        if (platform)
            return {*platform};
        return {};
    }

    // Strategy provided a plan: inject missing SL (and missing TP if configured).
    if (!intents_have_stop_loss(strategy_intents) && platform && platform->stop_loss)
    {
        // Prefer patching the first intent rather than a second full bracket.
        strategy_intents.front().stop_loss = platform->stop_loss;
        if (!strategy_intents.front().reference_entry)
            strategy_intents.front().reference_entry = platform->reference_entry;
    }
    if (!intents_have_take_profit(strategy_intents) && platform && platform->take_profit
        && p.tp_pct > 0.0)
    {
        strategy_intents.front().take_profit = platform->take_profit;
        if (!strategy_intents.front().reference_entry)
            strategy_intents.front().reference_entry = platform->reference_entry;
    }
    if (p.trail_pct > 0.0)
    {
        bool any_trail = false;
        for (const auto& ei : strategy_intents)
            if (ei.trailing_pct)
                any_trail = true;
        if (!any_trail)
            strategy_intents.front().trailing_pct = p.trail_pct;
    }

    return strategy_intents;
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
