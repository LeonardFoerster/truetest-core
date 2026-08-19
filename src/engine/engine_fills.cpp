// Engine order/market-pipeline fill-adjacent glue that stays engine-owned for
// now: venue bracket metadata draining, provider funding-update draining, and
// async submit-result draining. Originally "Engine fill accounting"
// (mechanical Phase 1 TU split); as of the Phase 2 domain-processor
// extraction (2026-08) the canonical fill pipeline itself (process_adapter_
// fills, stamp_fill_attribution, handle_engine_fill, dispatch_fill_to_
// strategy) moved to FillProcessor (fill_processor.{h,cpp}), reachable via
// engine's fills_ member. See core/docs/internal/engine-decomposition.md
// "Phase 2: Domain Processors" for the call-site mapping. What remains here
// is order-metadata / provider-drain plumbing that stays engine-owned until
// a future OrderIntentProcessor / MarketEventProcessor extraction step.
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
        order_meta_[m.engine_order_id] = order_meta{
            m.opener_order_id, m.strategy_name};
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

void engine::drain_async_submit_results(IExecutionAdapter* adapter)
{
    auto* cap = adapter ? adapter->get_async_support() : nullptr;
    if (!cap) return;

    std::vector<submit_result> results;
    if (!cap->poll_submit_results(results)) return;

    for (const auto& sr : results)
    {
        if (sr.op == submit_result::operation::submit)
        {
            if (sr.uncertain)
            {
                trigger_halt("venue order outcome is ambiguous after request write");
                audit_sink_->record_status_transition(
                    sr.engine_id, order_status::pending, order_status::pending,
                    "ambiguous post-write submit; terminal halt and reconcile required");
                continue;
            }
            if (sr.fatal)
            {
                trigger_halt("order mutation refused because safety prerequisites failed");
                continue;
            }
            if (sr.ok)
            {
                if (order_tracker_.get_order_status(sr.engine_id) == order_status::pending)
                {
                    order_tracker_.set_status(sr.engine_id, order_status::open);
                    if (dashboard_builder_) dashboard_builder_->update_open_order_status(sr.engine_id, "open");
                    // Unconditional via audit_sink (replaces questdb guard + #ifdef).
                    audit_sink_->record_status_transition(sr.engine_id,
                        order_status::pending, order_status::open,
                        "venue submit acknowledged");
                }
                continue;
            }
            if (!order_tracker_.is_active(sr.engine_id)) continue;

            auto rej = acquire_pooled(rejection_pool_,
                std::chrono::system_clock::now(), sr.symbol, sr.engine_id,
                "submit failed: " + sr.error);
            log_event(*rej);
            publish_event(rej);
            order_tracker_.set_status(sr.engine_id, order_status::rejected);
            if (dashboard_builder_) dashboard_builder_->erase_open_order(sr.engine_id);
            // Unconditional via audit_sink using the single record_rejection shape
            // (the rich order_event overload). For async submit transport errors
            // we synthesize a minimal stack order_event carrying the identity we have
            // (id + symbol + looked-up strategy). qty/price/side are best-effort zeros
            // (the sink path will record zeros for qty/price as before).
            // Strategy lookup mirrors the pattern used for cancellations in the same drain.
            const std::string& strat = lookup_strategy_name(sr.engine_id);
            order_event ghost{
                std::chrono::system_clock::now(),
                sr.symbol,
                order_type::market,
                order_side::buy,
                0.0,
                0.0
            };
            ghost.set_order_id(sr.engine_id);
            if (!strat.empty())
                ghost.set_strategy_name(strat);

            char transport_msg[128];
            std::snprintf(transport_msg, sizeof(transport_msg), "transport_error: %s", sr.error.c_str());
            audit_sink_->record_status_transition(sr.engine_id,
                order_status::pending, order_status::rejected,
                transport_msg);
            audit_sink_->record_rejection(ghost, "transport_error", sr.error.c_str());
            continue;
        }

        auto meta_it = pending_cancels_.find(sr.engine_id);
        const std::string symbol =
            !sr.symbol.empty() ? sr.symbol :
            (meta_it != pending_cancels_.end() ? meta_it->second.symbol : "");
        const std::string reason =
            (meta_it != pending_cancels_.end() && !meta_it->second.reason.empty())
                ? meta_it->second.reason
                : (sr.ok ? "venue cancel acknowledged" : "venue cancel failed");

        if (sr.uncertain)
        {
            trigger_halt("venue cancel outcome is ambiguous after request write");
            if (dashboard_builder_)
                dashboard_builder_->update_open_order_status(sr.engine_id, "cancel_unknown");
        }
        else if (sr.fatal)
        {
            trigger_halt("cancel refused because safety prerequisites failed");
            if (dashboard_builder_)
                dashboard_builder_->update_open_order_status(sr.engine_id, "cancel_refused");
        }
        else if (sr.ok)
        {
            if (order_tracker_.is_active(sr.engine_id))
            {
                order_tracker_.set_status(sr.engine_id, order_status::cancelled);
                if (dashboard_builder_) dashboard_builder_->erase_open_order(sr.engine_id);
                auto cancel_ev = acquire_pooled(cancel_pool_,
                    std::chrono::system_clock::now(), symbol, sr.engine_id, reason);
                log_event(*cancel_ev);
                publish_event(cancel_ev);
                if (!config_.is_threaded())
                    analytics_.on_event(cancel_ev);
                // Unconditional via audit_sink (replaces questdb guard + #ifdef).
                audit_sink_->record_cancellation(sr.engine_id, symbol.c_str(),
                    lookup_strategy_name(sr.engine_id).c_str(),
                    reason.empty() ? "manual" : reason.c_str());
                audit_sink_->record_status_transition(sr.engine_id,
                    order_status::open, order_status::cancelled, reason.empty() ? nullptr : reason.c_str());
            }
        }
        else
        {
            if (dashboard_builder_) dashboard_builder_->update_open_order_status(sr.engine_id, "cancel_failed");
        }

        if (meta_it != pending_cancels_.end())
            pending_cancels_.erase(meta_it);
    }
}
