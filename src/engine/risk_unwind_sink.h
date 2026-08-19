#pragma once

// IRiskUnwindSink — narrow interface for FillProcessor to request emergency
// liquidation after a post-fill risk breach, per the "OrderIntentProcessor
// Preparation Report" §9 ("Callback / interface recommendation") and the
// Phase 2 deliverable's FillProcessor-interaction analysis.
//
// Why this exists instead of a direct OrderIntentProcessor& on FillProcessor:
// FillProcessor is constructed BEFORE OrderIntentProcessor (OrderIntentProcessor
// takes FillProcessor& — the reverse construction order is impossible). A
// concrete reference to something not yet constructed cannot be bound at
// FillProcessor's construction time, so *some* deferred-dereference channel
// is structurally required — the same construction-order proof already
// applied to OrderAttributionStore in the preparatory extraction. `engine`
// implements this interface (alongside EngineHotPathSink) and its own
// request_unwind() forwards to orders_->unwind_positions(...), dereferencing
// orders_ only when the interface method is actually *invoked* — deep in
// FillProcessor::handle_fill's post-fill-risk branch, long after every
// engine member (including orders_) is fully constructed — never at bind
// time. This replaces the former std::function<void(std::size_t&)>
// request_unwind callback with a named, explicit, zero-capture interface
// reference of the same runtime cost (one indirect call either way).
//
// Deliberately single-method: not a generic callback bag. No engine&, no
// EngineContext, no service locator.
//
// LIVE-SAFETY SURFACE: the sole channel through which FillProcessor's
// post-fill risk-halt path reaches emergency liquidation. See
// scripts/check-live-safety-freeze.sh.

#include <cstddef>

class IRiskUnwindSink
{
public:
    virtual ~IRiskUnwindSink() = default;

    // Unwind every open position (emergency liquidation). Must run BEFORE
    // the caller's subsequent trigger_halt — see the Phase 2 deliverable's
    // "unwind ordering evidence" for the exact sequence this preserves.
    virtual void request_unwind(std::size_t& event_count) = 0;
};
