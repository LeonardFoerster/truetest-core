// Engine lifecycle: construction/shutdown support, object-pool prewarm/drain,
// checkpoint write/restore, strategy wiring, and Monte Carlo trial reset.
// Extracted mechanically from engine.cpp (Phase 1 TU split); behavior unchanged.
#include "engine.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>

void engine::prewarm_object_pools()
{
    const auto& pw = config_.pool_prewarm;

    auto setup = [&](const char* name, std::size_t min_blocks, auto& pool) {
        pool.set_pool_name(name);
        pool.ensure_min_blocks(min_blocks);
        pool.set_forbid_runtime_grow(pw.forbid_runtime_grow);
    };

    setup("market_pool",      pw.market_blocks,      market_pool_);
    setup("tick_pool",        pw.tick_blocks,        tick_pool_);
    setup("order_pool",       pw.order_blocks,       order_pool_);
    setup("fill_pool",        pw.fill_blocks,        fill_pool_);
    setup("l2_update_pool",   pw.l2_update_blocks,   l2_update_pool_);
    setup("l2_snapshot_pool", pw.l2_snapshot_blocks, l2_snapshot_pool_);
    setup("rejection_pool",   pw.rejection_blocks,   rejection_pool_);
    setup("cancel_pool",      pw.cancel_blocks,      cancel_pool_);
    setup("amend_pool",       pw.amend_blocks,       amend_pool_);
    setup("funding_pool",     pw.funding_blocks,     funding_pool_);

    std::size_t total_event_slots =
        market_pool_.capacity_slots() + tick_pool_.capacity_slots()
        + order_pool_.capacity_slots() + fill_pool_.capacity_slots()
        + l2_update_pool_.capacity_slots() + l2_snapshot_pool_.capacity_slots()
        + rejection_pool_.capacity_slots() + cancel_pool_.capacity_slots()
        + amend_pool_.capacity_slots() + funding_pool_.capacity_slots()
        ;

    std::size_t cb_slots = pw.control_block_slots;
    if (cb_slots == 0)
        cb_slots = total_event_slots;

    const std::size_t cb_blocks =
        (cb_slots + ControlBlockPool::slots_per_block() - 1)
        / ControlBlockPool::slots_per_block();

    control_block_pool_.set_pool_name("control_block_pool");
    control_block_pool_.ensure_min_blocks(std::max(cb_blocks, std::size_t{1}));
    control_block_pool_.set_forbid_runtime_grow(pw.forbid_runtime_grow);

    auto wire_cb = [&](auto& pool) {
        pool.set_control_block_pool(&control_block_pool_);
    };
    wire_cb(market_pool_);
    wire_cb(tick_pool_);
    wire_cb(order_pool_);
    wire_cb(fill_pool_);
    wire_cb(l2_update_pool_);
    wire_cb(l2_snapshot_pool_);
    wire_cb(rejection_pool_);
    wire_cb(cancel_pool_);
    wire_cb(amend_pool_);
    wire_cb(funding_pool_);

    // Orderbook orders: pool bodies only; CBs stay on heap (lambda deleter
    // + order type exceed the 64-byte CB slot on some libstdc++ builds).
    orderbook_registry_.set_order_pool_config(nullptr,
                                              pw.orderbook_order_blocks,
                                              pw.forbid_runtime_grow);
}

void engine::drain_object_pool_returns() noexcept
{
    market_pool_.drain_deferred_returns();
    order_pool_.drain_deferred_returns();
    fill_pool_.drain_deferred_returns();
    tick_pool_.drain_deferred_returns();
    l2_update_pool_.drain_deferred_returns();
    l2_snapshot_pool_.drain_deferred_returns();
    rejection_pool_.drain_deferred_returns();
    cancel_pool_.drain_deferred_returns();
    amend_pool_.drain_deferred_returns();
    funding_pool_.drain_deferred_returns();
    control_block_pool_.drain_deferred_returns();
}

void engine::write_checkpoint_if_due(std::size_t event_count)
{
    if (checkpoint_mgr_) checkpoint_mgr_->write_if_due(portfolio_, event_count);
}

void engine::restore_from_checkpoint()
{
    if (checkpoint_mgr_) checkpoint_mgr_->restore(portfolio_);
}

void engine::set_strategy(std::shared_ptr<IStrategy> strategy)
{
    if (!strategy) return;
    strategy_ = std::move(strategy);

    for (const auto& [symbol, pos] : portfolio_.get_positions()) {
        strategy_->set_position_open(symbol, std::abs(pos.qty) > 1e-12);
    }
}

void engine::switch_symbol(const std::string& new_symbol)
{
    // MarketSeries read/write API (docs/internal/data-pipeline.md#D-02) — no public SoA fields.
    data_handler_->set_all_bar_symbols(new_symbol);

    strategy_->set_position_open(new_symbol, false);
}

void engine::reset_for_next_trial(uint64_t new_seed)
{
    // Reset for MC object reuse. See core/docs/internal/engine-decomposition.md (Phase 0 notes + future waves).
    // State owned by future extracted collaborators will be cleared via their clear/reset hooks.
    // Reset main portfolio (cash, positions, lots)
    portfolio_.reset();

    // Reset analytics (very expensive to recreate)
    // Use the configured initial balance when available (falls back to 10000 if not set yet)
    double initial = (config_.initial_balance > 0.0) ? config_.initial_balance : 10000.0;
    analytics_.reset(initial);

    // Reset exit manager
    exit_manager_.reset();

    // Reset order tracker (important for isolation)
    order_tracker_.reset();

    // Reset risk manager
    risk_manager_.reset();

    // Update seed in config for any RNGs
    config_.seed = new_seed;

    // Reset some counters / state
    last_mid_price_.store(0.0, std::memory_order_release);
    last_mark_symbol_.clear();
    prepare_mark_prices_for_run();
    fills_->reset_soft_post_fill_breaches();
    data_rows_rejected_ = 0;

    // Clear orderbook registry (L2 state from previous trial)
    orderbook_registry_.clear();

    // Adapters / pending DAY ids / execution_adapters_ are NOT fully cleared
    // here. MC still uses a fresh engine per trial (see MonteCarloController);
    // do not claim full in-place reuse readiness until those are reset too.

    // Reset market maker and adverse selection trackers
    market_maker_.reset();
    adverse_selection_.reset();

    // Reset per-symbol caches that can leak state between trials
    if (instrument_spec_cache_) instrument_spec_cache_->clear();
    l2_seeded_symbols_.clear();

    if (dashboard_builder_) dashboard_builder_->clear_for_mc_reset();

    // Reset tick aggregator (prevents partial bar leakage between trials)
    if (tick_aggregator_)
    {
        tick_aggregator_->reset();
    }

    // Clear UI/dashboard caches (harmless and cheap for headless MC runs)
    if (dashboard_builder_) {
        // the builder owns them now; for reset we can request refresh or clear via public if exposed, but for now force reinit on next
        // since state moved, the builder will handle in its own reset if we add later.
    }

    // Phase 4 MC reuse hardening: clear attribution for clean per-trial
    // isolation (opener/strategy attribution must not leak between
    // independent trials). Canonical owner is attribution_
    // (OrderAttributionStore) as of the OrderIntentProcessor preparatory
    // extraction.
    attribution_->clear();
    // pending_cancels_ owned by OrderIntentProcessor as of Phase 3 — clear
    // via its own narrow reset hook.
    orders_->clear_pending_cancels();

    // Clear shadow_tracker for per-trial isolation (divergence tracking not
    // relevant across MC trials; see MC controller comment).
    if (shadow_tracker_) shadow_tracker_->reset();

    // Opportunistic drain for clean in_use() counters for the next trial.
    // Does not solve escaped shared_ptrs from previous trial (those are a
    // contract violation for the MC controller / test harness).
    drain_object_pool_returns();

    // Terminal halt survives MC reuse; a halted process never re-arms
    // provider callbacks.
    const bool may_arm = !halt_flag_.load(std::memory_order_acquire);
    provider_callbacks_armed_.store(may_arm, std::memory_order_release);
    if (callbacks_armed_flag_)
        callbacks_armed_flag_->store(may_arm, std::memory_order_release);

    // Re-arm pool alive guards for MC reuse of the engine instance.
    // IMPORTANT: Callers (MC controller, tests) must ensure no shared_ptr<Event>
    // or pooled objects from the *prior epoch/trial* are still held by external
    // observers when reset_for_next_trial is called. Our Returner epoch check
    // makes late drops from old epochs safe (they leak instead of UAF), but
    // outstanding live objects from prior trial violate the reuse contract and
    // may observe stale state in rings/portfolio until drained.
    market_pool_.rearm_for_reuse();
    order_pool_.rearm_for_reuse();
    fill_pool_.rearm_for_reuse();
    tick_pool_.rearm_for_reuse();
    l2_update_pool_.rearm_for_reuse();
    l2_snapshot_pool_.rearm_for_reuse();
    rejection_pool_.rearm_for_reuse();
    cancel_pool_.rearm_for_reuse();
    amend_pool_.rearm_for_reuse();
    funding_pool_.rearm_for_reuse();

    // Phase 4 MC safety: if any pool still reports in_use() > 0 here, it means
    // there were escaped shared_ptrs from the prior trial. Our late-drop
    // logic will leak them on drop rather than UAF, but this indicates a
    // contract violation by the caller (MC harness or test). Log in debug.
#ifndef NDEBUG
    auto check_in_use = [](const char* name, auto& pool) {
        auto u = pool.in_use();
        if (u != 0) {
            std::fprintf(stderr,
                "[reset_for_next_trial] WARNING: %s still has %zu in_use() "
                "(escaped objects from prior epoch?)\n", name, u);
        }
    };
    check_in_use("market", market_pool_);
    check_in_use("order", order_pool_);
    check_in_use("fill", fill_pool_);
    // ... (others similar; abbreviated)
#endif

    // Note: Rings, workers, event_logger_, and dashboard timing are left mostly
    // untouched (as before). Workers repopulate via ring events on next fills/orders.
    // Full ring/worker reset is complex and usually unnecessary for MC reuse of
    // the engine instance (Phase B). Core objects (portfolio, analytics, exit_manager,
    // etc.) are now fully reset to enable broader reuse.
}
