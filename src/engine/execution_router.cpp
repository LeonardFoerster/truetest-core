#include "execution_router.h"

#include "execution/queue_aware_book_adapter.h"
// NOTE: no concrete bridge / local / shadow includes here.
// Capability queries go through the IExecutionAdapter base.

ExecutionRouter::ExecutionRouter(
    OrderbookRegistry& ob_reg,
    const engine_config& cfg,
    std::unordered_set<std::string>& l2_seeded,
    IProvider* provider,
    std::unordered_map<uint64_t, pending_cancel_meta>& pending_cancels_ref,
    std::unordered_map<uint64_t, order_meta>& order_meta_ref,
    std::unordered_map<std::string, std::shared_ptr<IExecutionAdapter>>& adapters_ref
)
    : adapters_(adapters_ref)
    , ob_reg_(ob_reg)
    , cfg_(cfg)
    , l2_seeded_(l2_seeded)
    , provider_(provider)
    , pending_cancels_(pending_cancels_ref)
    , order_meta_(order_meta_ref)
{
    // adapters_ (ref) populated on resolve; creation logic now here (moved from engine get_adapter).
}

std::shared_ptr<IExecutionAdapter> ExecutionRouter::resolve_adapter(const std::string& symbol) noexcept
{
    auto it = adapters_.find(symbol);
    if (it != adapters_.end())
        return it->second;

    std::shared_ptr<IExecutionAdapter> adapter;
    if (cfg_.mode != engine_mode::shadow &&
        cfg_.provider && cfg_.provider->has_execution())
    {
        adapter = cfg_.provider->get_execution_adapter();
    }
    if (!adapter)
    {
        auto ob = ob_reg_.get_or_create(symbol);

        if (cfg_.maker_queue_model)
        {
            // Use queue-aware paper execution for passive limits
            auto qa = std::make_shared<QueueAwareBookAdapter>(
                cfg_.maker_queue_model,
                cfg_.fee_model,
                cfg_.latency_model);
            adapter = qa;
        }
        else
        {
            auto local = std::make_shared<LocalBookAdapter>(
                ob, cfg_.fee_model, cfg_.fill_model,
                cfg_.seed != 0 ? static_cast<unsigned>(cfg_.seed + 2) : cfg_.fill_rng_seed,
                cfg_.market_aggression, cfg_.qty_scale,
                cfg_.latency_model, cfg_.impact_model,
                cfg_.walked_book_impact);
            if (cfg_.debug_fills)
                local->set_debug_fills(true, cfg_.debug_fills_budget);
            adapter = local;
        }
    }

    adapters_[symbol] = adapter;
    return adapter;
}

bool ExecutionRouter::is_async_submit(IExecutionAdapter* a) const noexcept
{
    return a && a->supports_async_submit();
}

void ExecutionRouter::submit(const order_event& o, IExecutionAdapter* a) noexcept
{
    if (a)
        a->submit_order(o);
}

void ExecutionRouter::drain_submit_results(IExecutionAdapter* a) noexcept
{
    // Skeleton: full drain (incl. ExecutionBridge::poll_submit_results + meta) moved later.
    (void)a;
}

bool ExecutionRouter::poll_fills(IExecutionAdapter* a, std::vector<fill_event>& out) noexcept
{
    if (a)
        return a->poll_fills(out);
    return false;
}

void ExecutionRouter::submit_to_exchange_shadow(const order_event& o) noexcept
{
    // Skeleton no-op. Shadow dual-submit lives in engine until moved.
    (void)o;
}

// Iteration moved from engine (net reduction + central). Includes provider adapter.
void ExecutionRouter::advance_all(std::chrono::system_clock::time_point ts) noexcept
{
    for (auto& [_, ad] : adapters_)
        if (ad) ad->advance_time(ts);
    if (provider_)
    {
        if (auto pa = provider_->get_execution_adapter())
            pa->advance_time(ts);
    }
}

void ExecutionRouter::on_l2_snapshot(const std::string& symbol,
                                     const std::vector<std::pair<double, double>>& bids,
                                     const std::vector<std::pair<double, double>>& asks) noexcept
{
    for (auto& [_, ad] : adapters_)
        if (ad) ad->on_l2_snapshot(symbol, bids, asks);
    if (provider_)
        if (auto pa = provider_->get_execution_adapter())
            pa->on_l2_snapshot(symbol, bids, asks);
}

void ExecutionRouter::on_l2_update(const std::string& symbol, order_side os,
                                   double price, double new_qty) noexcept
{
    for (auto& [_, ad] : adapters_)
        if (ad) ad->on_l2_update(symbol, os, price, new_qty);
    if (provider_)
        if (auto pa = provider_->get_execution_adapter())
            pa->on_l2_update(symbol, os, price, new_qty);
}
