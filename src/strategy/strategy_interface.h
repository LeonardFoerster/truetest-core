#pragma once
#include "../core/event.h"
#include "exits/exit_intent.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <limits>
#include <stdexcept>

struct param_def
{
    std::string name;
    double default_value = 0.0;
    double min_value = -std::numeric_limits<double>::max();
    double max_value = std::numeric_limits<double>::max();
    std::string description;
};

class IStrategy {
public:
    virtual ~IStrategy() = default;
    virtual std::optional<order_event> on_market(const market_event& mkt) = 0;
    virtual std::optional<order_event> on_tick(const tick_event&) { return std::nullopt; }
    virtual std::optional<order_event> on_l2_update(const l2_update_event&) { return std::nullopt; }

    // Legacy net-truth pusher: engine calls this whenever a symbol flips
    // between flat and non-flat on the portfolio's netted book. New
    // strategies should ignore this (the default no-op) and track their
    // own open lots via on_fill. Kept for single-position strategies that
    // still want a simple boolean.
    virtual void set_position_open(const std::string& /*symbol*/, bool /*open*/) {}
    virtual void set_position_open(bool open) { set_position_open("", open); }

    // Fires after the engine updates its own books for a fill that this
    // strategy emitted. `opener_order_id` identifies the lot (equals
    // fill.order_id on an opener, points at the original entry on a
    // closer). Default no-op; strategies tracking their own open lots
    // override to add/remove entries.
    virtual void on_fill(const fill_event& /*fill*/,
                         std::uint64_t /*opener_order_id*/) {}

    // Strategy-declared exit plan (SL/TP/trailing/time). The engine polls
    // this right after each on_market/on_tick/on_l2_update call, stamps
    // each returned intent with the just-submitted opener's order_id, and
    // hands them to the ExitManager. Returning multiple lets a strategy
    // declare TP1/TP2/SL scale-outs per entry.
    virtual std::vector<truetest::exits::exit_intent> take_pending_exit_intents()
    {
        // Default drains the single-optional legacy hook so existing
        // strategies keep working without overriding this method.
        std::vector<truetest::exits::exit_intent> out;
        if (auto one = take_pending_exit_intent())
            out.push_back(std::move(*one));
        return out;
    }

    // Legacy single-intent hook. Override either this or the vector form.
    virtual std::optional<truetest::exits::exit_intent> take_pending_exit_intent()
    {
        return std::nullopt;
    }

    virtual std::vector<param_def> get_param_schema() const { return {}; }

    virtual void set_param(const std::string& key, double value)
    {
        (void)key; (void)value;
        throw std::runtime_error("Unknown parameter: " + key);
    }

    virtual std::vector<std::pair<std::string, double>> get_indicator_values(
        const std::string& /*symbol*/) const
    {
        return {};
    }

    // Phase A/B (MC object reuse): strategies that hold internal state (e.g. indicators,
    // adaptive parameters, RNGs) should override this to reset for the next trial.
    // The seed is provided so RNG-based strategies can be made deterministic across trials.
    // Default is a no-op.
    virtual void reset(uint64_t /*seed*/ = 0) {}

    // Monte Carlo --mc-reuse-objects: only strategies that fully clear trial-local
    // state in reset() may opt in. Default false refuses silent state leak (HIGH-03).
    virtual bool supports_mc_trial_reuse() const { return false; }
};
