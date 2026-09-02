#pragma once

// OrderAttributionStore — narrow leaf collaborator (engine-decomposition
// preparatory extraction; see core/docs/internal/engine-decomposition.md
// "Phase 3 Candidate work" and the OrderIntentProcessor Preparation Report
// this file implements §7 / the constructor-cycle finding of).
//
// Canonical owner of per-order attribution metadata: which opener_order_id a
// given order_id belongs to, and which strategy emitted it. This is the sole
// owner of what was engine::order_meta_ before this extraction.
//
// Deliberately zero dependencies beyond core/event.h (order_event is the
// foundational vocabulary type every layer uses, not a subsystem): no
// engine, no FillProcessor, no OrderIntentProcessor, no RiskManager, no
// Portfolio, no Provider, no ExecutionRouter, no Dashboard, no QuestDB, no
// strategy objects. Both OrderIntentProcessor and FillProcessor hold a
// reference to the same instance — this is precisely what removes the
// OrderIntentProcessor -> FillProcessor -> OrderIntentProcessor construction
// cycle a raw-order_meta_-inside-OrderIntentProcessor design would otherwise
// create (Preparation Report §7; see order_intent_processor.h for the
// completed extraction this store now serves).
//
// LIVE-SAFETY SURFACE: this file carries logic previously inside frozen
// engine_orders.cpp / engine_fills.cpp / engine_market.cpp / engine_lifecycle.cpp.
// See scripts/check-live-safety-freeze.sh.

#include "core/event.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

class OrderAttributionStore final
{
public:
    // Registers/overwrites attribution for o.get_order_id(). opener_order_id
    // defaults to the order's own id when it carries no explicit opener —
    // exact former engine::register_order_meta semantics, verbatim.
    void register_order(const order_event& o);

    // Venue-bracket leg attribution (former engine::drain_venue_bracket_meta
    // write site: `order_meta_[m.engine_order_id] = order_meta{m.opener_order_id, m.strategy_name};`).
    // Distinct call site, same map, same semantics.
    void note_bracket_leg(uint64_t engine_order_id, uint64_t opener_order_id,
                          std::string strategy_name);

    // Exact former engine::lookup_opener / engine::lookup_strategy_name /
    // FillProcessor::lookup_opener / FillProcessor::lookup_strategy_name
    // semantics: 0 / empty-string sentinel on miss, no exceptions.
    uint64_t opener_for(uint64_t order_id) const;
    const std::string& strategy_for(uint64_t order_id) const;
    std::size_t size() const noexcept { return map_.size(); }

    // Phase A (MC object reuse): clears all attribution for the next trial.
    // Former: `order_meta_.clear();` in engine::reset_for_next_trial.
    void clear();

private:
    // Moved from execution_router.h with the store extraction. Keep the
    // representation private; collaborators use the two sentinel-returning
    // lookup methods above.
    struct order_meta
    {
        uint64_t opener_order_id = 0;
        std::string strategy_name;
    };

    std::unordered_map<uint64_t, order_meta> map_;
};
