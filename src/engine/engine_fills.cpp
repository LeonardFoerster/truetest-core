// Engine order/market-pipeline fill-adjacent glue that stays engine-owned:
// venue bracket metadata draining and provider funding-update draining.
// Originally "Engine fill accounting" (mechanical Phase 1 TU split); the
// Phase 2 domain-processor extraction (2026-08) moved the canonical fill
// pipeline itself (process_adapter_fills, stamp_fill_attribution,
// handle_engine_fill, dispatch_fill_to_strategy) to FillProcessor
// (fill_processor.{h,cpp}), reachable via engine's fills_ member; the
// OrderIntentProcessor Phase 1 extraction (2026-08) moved
// drain_async_submit_results to order_intent_processor.{h,cpp}, reachable
// via engine's orders_ member. See core/docs/internal/engine-decomposition.md
// "Phase 2: Domain Processors" and the "OrderIntentProcessor Preparation
// Report" for the call-site mapping.
#include "engine.h"
#include "execution/async_support.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>

void engine::drain_venue_bracket_meta()
{
    if (!config_.provider) return;
    auto adapter = config_.provider->get_execution_adapter();
    auto* cap = adapter ? adapter->get_async_support() : nullptr;
    if (!cap) return;

    std::vector<synth_meta> meta;
    if (!cap->poll_synth_meta(meta)) return;

    for (const auto& m : meta)
    {
        attribution_->note_bracket_leg(m.engine_order_id, m.opener_order_id,
                                       m.strategy_name);
    }
}

bool engine::drain_provider_funding_updates() noexcept
{
    if (!provider_funding_ingress_)
        return true;

    provider_funding_update update;
    bool applied_any = false;
    while (provider_funding_ingress_->try_pop(update))
    {
        if (update.event_time_ms <= 0
            || !std::isfinite(update.cash_delta)
            || update.cash_delta == 0.0
            || update.symbol_size == 0
            || update.symbol_size > provider_funding_update::symbol_capacity
            || update.why != provider_funding_update::reason::funding_fee)
        {
            trigger_halt("provider funding ingress produced an invalid update");
            return false;
        }

        try
        {
            // The prior loop iteration releases its pooled funding event only
            // after publish_event returns. Reclaim that deferred return before
            // acquiring the next retained ingress record.
            drain_object_pool_returns();
            const auto ts = std::chrono::system_clock::time_point{
                std::chrono::milliseconds{update.event_time_ms}};
            auto funding = acquire_pooled(
                funding_pool_, ts, update.symbol_view(), 0.0,
                update.cash_delta, std::string_view{"FUNDING_FEE"});
            log_event(*funding);
            publish_event(funding);
            // Keep the engine-owned risk/accounting view current in every
            // preset. Threaded worker Analytics instances receive their own
            // copy through publish_event.
            analytics_.on_event(funding);
            applied_any = true;
        }
        catch (...)
        {
            // acquire_pooled already latches pool exhaustion. Cover every
            // other construction/audit/ring exception without allowing it to
            // escape a provider callback or teardown path.
            trigger_halt("provider funding update could not be applied");
            return false;
        }
    }

    if (applied_any && dashboard_builder_)
    {
        try
        {
            dashboard_builder_->request_dashboard_refresh();
            dashboard_builder_->refresh_if_due();
        }
        catch (...)
        {
            trigger_halt("provider funding dashboard refresh failed");
            return false;
        }
    }

    // Close the producer/consumer race: an enqueue that observed a full ring
    // while we were draining must still be terminal even if all retained
    // records were consumed successfully.
    if (provider_funding_ingress_->failed())
    {
        trigger_halt("provider funding ingress overflow or malformed update");
        return false;
    }
    return true;
}

// drain_async_submit_results moved to
// OrderIntentProcessor::drain_async_submit_results (Phase 1) — see
// order_intent_processor.cpp. Call sites (process()/unwind_positions()
// internally; cancel_order() and the market pipeline externally) now say
// orders_->drain_async_submit_results(...).
