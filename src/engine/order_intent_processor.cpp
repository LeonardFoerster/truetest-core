// OrderIntentProcessor implementation. Phase 1 moved verbatim (behavior-
// preserving) engine::process_order/unwind_positions (engine_orders.cpp),
// engine::drain_async_submit_results (engine_fills.cpp), and
// engine::marked_account_equity (engine_market.cpp). Phase 2 adds
// engine::route_order, check_pending_stops, sweep_resting_limits,
// deliver_mm_book_trades, evaluate_exits (both overloads),
// finalize_strategy_route, register_strategy_exit_intent,
// resolve_instrument_spec/apply_instrument_spec (all former
// engine_orders.cpp), and the domain-glue half of
// engine::drain_pending_orders + mid_for_symbol (former engine_pending.cpp).
// Phase 3 adds engine::cancel_order/modify_order (former engine_orders.cpp)
// and engine::drain_final_pending + cancel_day_orders, consolidated into
// finalize_end_of_stream (former engine_pending.cpp) — the last of the
// order-domain boundary. pending_stops_/pending_cancels_ become genuinely
// owned here in Phase 3 (previously engine-owned references) now that every
// reader and writer of each lives in this class.
// See the "OrderIntentProcessor Preparation Report" and its Phase 1/2/3
// implementation deliverables for the call-site mapping and ordering
// evidence.
//
// LIVE-SAFETY SURFACE: see order_intent_processor.h + scripts/check-live-safety-freeze.sh.
#include "order_intent_processor.h"

#include "dashboard_snapshot_builder.h"
#include "analytics/shadow_tracker.h"
#include "execution/execution_adapter.h"
#include "execution/async_support.h"
#include "execution/latency_model.h"
#include "types/order_id.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <utility>

OrderIntentProcessor::OrderIntentProcessor(
    portfolio& port,
    OrderTracker& order_tracker,
    RiskManager& risk_manager,
    IRiskCheck* risk_check,
    Analytics& analytics,
    IOrderAuditSink& audit_sink,
    ExecutionRouter& router,
    FillProcessor& fills,
    OrderAttributionStore& attribution,
    PendingOrderScheduler& scheduler,
    truetest::exits::ExitManager& exit_manager,
    InstrumentSpecCache& instrument_spec_cache,
    MarketMaker& market_maker,
    OrderbookRegistry& orderbook_registry,
    std::unordered_map<std::string, std::shared_ptr<IExecutionAdapter>>& execution_adapters,
    ObjectPool<order_event>& order_pool,
    ObjectPool<rejection_event>& rejection_pool,
    ObjectPool<cancel_event>& cancel_pool,
    ObjectPool<amend_event>& amend_pool,
    const std::atomic<bool>& halt_flag,
    const std::atomic<bool>& pause_all,
    std::atomic<double>& last_mid_price,
    const std::string& last_mark_symbol,
    const std::unordered_map<std::string, mark_point>& last_mark_prices,
    std::mutex& last_mark_prices_mu,
    const std::chrono::system_clock::time_point& last_sim_time,
    const std::chrono::system_clock::time_point& last_decision_ts,
    const std::unordered_set<std::string>& l2_seeded_symbols,
    const bool& mm_threaded,
    DashboardSnapshotBuilder* dashboard_builder,
    ShadowTracker* shadow_tracker,
    portfolio* exchange_portfolio,
    const engine_config& config,
    EngineHotPathSink& hotpath
    )
    : portfolio_(port), order_tracker_(order_tracker), risk_manager_(risk_manager),
      risk_check_(risk_check), analytics_(analytics), audit_sink_(audit_sink),
      router_(router), fills_(fills), attribution_(attribution),
      pending_scheduler_(scheduler), exit_manager_(exit_manager),
      instrument_spec_cache_(instrument_spec_cache), market_maker_(market_maker),
      orderbook_registry_(orderbook_registry),
      execution_adapters_(execution_adapters), order_pool_(order_pool),
      rejection_pool_(rejection_pool), cancel_pool_(cancel_pool), amend_pool_(amend_pool),
      halt_flag_(halt_flag), pause_all_(pause_all), last_mid_price_(last_mid_price),
      last_mark_symbol_(last_mark_symbol), last_mark_prices_(last_mark_prices),
      last_mark_prices_mu_(last_mark_prices_mu), last_sim_time_(last_sim_time),
      last_decision_ts_(last_decision_ts),

      l2_seeded_symbols_(l2_seeded_symbols), mm_threaded_(mm_threaded),
      dashboard_builder_(dashboard_builder), shadow_tracker_(shadow_tracker),
      exchange_portfolio_(exchange_portfolio), config_(config), hotpath_(hotpath)
{
}

bool OrderIntentProcessor::emit_terminal_transition(
    std::uint64_t order_id,
    const std::string& symbol,
    double qty,
    order_status terminal,
    std::chrono::system_clock::time_point authoritative_ts)
{
    // ========================================================================
    // F-02 — THE single point where an order leaves the active lifecycle
    // without filling. docs/todos/11-F-forensic-lifecycle-audit.md.
    //
    // Before this existed, route() parked bar-delayed orders and returned
    // `pending`, so finalize_route took the NON-rejected path and armed the
    // exit intent. The actual rejection happened one bar later inside
    // drain_due -> process(), which has no finalize_route and told nobody:
    // the strategy kept its optimistic entry gate set and never traded
    // again (traced: 1,713,190 silent bars after one drawdown reject), and
    // the exit intent stayed in ExitManager::pending_ forever.
    //
    // Two things must happen for every terminal non-fill outcome, on every
    // path, or the same trap reopens the next time a rejection rule is
    // added:
    //   (i)  the owning strategy is told the position did not change, so an
    //        optimistic entry_pending state resets to flat;
    //   (ii) the exit intent registered for that order is released.
    //
    // Every set_status(..., rejected/cancelled/expired) in this translation
    // unit goes through here. The invariant test asserts that.
    // ========================================================================
    if (order_id == 0) return false;

    const auto state_before = order_tracker_.get_order_status(order_id);
    if (authoritative_ts.time_since_epoch().count() > 0)
    {
        const auto validation = order_tracker_.validate_lifecycle(
            order_id, terminal, authoritative_ts);
        if (validation.benign_noop()) return false;
        if (!validation.applied()
            || !order_tracker_.commit_lifecycle(validation))
        {
            hotpath_.trigger_halt(
                "authoritative venue lifecycle transition is invalid");
            return false;
        }
    }
    else
    {
        order_tracker_.set_status(order_id, terminal);
    }
    if (dashboard_builder_) dashboard_builder_->erase_open_order(order_id);

    const std::uint64_t opener = attribution_.opener_for(order_id);
    const bool protective_terminal =
        exit_manager_.on_protective_close_terminal(
            order_id, [&](const truetest::exits::ExitManager::protective_exit_view& protective)
            {
                audit_sink_.record_exit_lifecycle(exit_lifecycle_record{
                    order_id, order_id, protective.opener_order_id, 0,
                    {}, {}, {}, {}, protective.requested_qty,
                    protective.filled_qty, protective.remaining_qty,
                    protective.reason, state_before, terminal,
                    protective.symbol.data(), protective.strategy_name.data(),
                    "terminal_non_fill", "terminal"});
            });
    if (protective_terminal)
    {
        // A bracket/forced flatten was already fired and a venue, filter, or
        // risk path refused it. Retrying a protective close after a terminal
        // outcome is forbidden: it can duplicate a venue order. Restore only
        // the accounting reservation inside ExitManager, emit an auditable
        // terminal state, and halt the process for operator reconciliation.
        hotpath_.trigger_halt("protective exit reached terminal non-fill state");
    }
    else if (opener == 0 || opener == order_id)
    {
        // An opener died. Its intent can never be promoted, so release it
        // instead of leaving it in pending_ to leak (F-06) and, on order-id
        // reuse, phantom-arm on an unrelated entry.
        exit_manager_.cancel(order_id);
    }
    else
    {
        // A closer died. The quantity it reserved came out of the opener's
        // remaining size when the bracket fired; give it back so the lot is
        // not silently understated.
        exit_manager_.release_close_reservation(opener, qty);
    }

    if (!symbol.empty())
    {
        fills_.notify_position_change_all(
            symbol, portfolio_.position_open(symbol),
            /*sweep_flat_brackets=*/false);
    }
    return true;
}

double OrderIntentProcessor::marked_account_equity(std::string_view current_symbol,

                                                    double current_mark) const
{
    double equity = portfolio_.get_cash();
    const auto& positions = portfolio_.get_positions();
    if (positions.empty())
    {
        note_marked_equity(equity);
        return equity;
    }

    if (positions.size() == 1 && current_mark > 0.0)
    {
        const auto& [symbol, position] = *positions.begin();
        if (symbol == current_symbol)
        {
            const double eq = equity + position.qty * current_mark;
            note_marked_equity(eq);
            return eq;
        }
    }

    std::lock_guard<std::mutex> lk(last_mark_prices_mu_);
    for (const auto& [symbol, position] : positions)
    {
        if (std::abs(position.qty) <= 1e-12)
            continue;
        double position_mark = 0.0;
        if (symbol == current_symbol && current_mark > 0.0)
            position_mark = current_mark;
        else if (auto it = last_mark_prices_.find(symbol);
                 it != last_mark_prices_.end() && it->second.usable())
            position_mark = it->second.price;
        else
            return std::numeric_limits<double>::quiet_NaN();
        equity += position.qty * position_mark;
    }
    note_marked_equity(equity);
    return equity;
}

void OrderIntentProcessor::note_marked_equity(double equity) const
{
    // F-05a: this is the engine's authoritative marked-equity pass — it runs
    // on the engine thread before every strategy callback, so it is the one
    // place that sees the account cross zero regardless of which loop is
    // driving. Latch only; the engine does not auto-liquidate here (that
    // behaviour change is F-05b's design decision, not a side effect of
    // observing).
    if (!std::isfinite(equity) || equity > 0.0) return;
    if (portfolio_.is_bankrupt()) return;
    portfolio_.observe_marked_equity(equity);
    analytics_.mark_bankrupt(equity);
    std::fprintf(stderr,
        "[engine] F-05a: account equity reached %.2f — the run is past "
        "bankruptcy and every metric after this point is invalid.\n", equity);
}


bool OrderIntentProcessor::process(const std::shared_ptr<order_event>& o,
                                   std::size_t& event_count,
                                   bool& halt_requested)
{
    // S3: process-wide halt is terminal for ALL submit call sites, including
    // check_pending_stops / pending drains that bypass route_order.
    if (halt_flag_.load(std::memory_order_acquire))
    {
        halt_requested = true;
        return false;
    }

    if (!o->has_submit_ts())
        o->set_submit_ts(last_sim_time_);

    // ========================================================================
    // CANONICAL HOT-PATH ORDERING (Phase 3 deepdive cleanup; preserved
    // verbatim across the Phase 1 OrderIntentProcessor extraction — see the
    // Preparation Report's before/after call-flow deliverable).
    // This documents the enforced sequence for order + fill processing.
    // All run_* paths (bar/tick/stream/replay), evaluate_exits, unwind, etc.
    // should follow this for consistent per-lot state, shadow divergence,
    // publish to rings/workers, and cache updates.
    //
    // 1. Venue pre-trade risk (FuturesRiskCheck / risk_check_) — reject only.
    // 2. RiskManager pre-order check (can halt).
    // 3. route_order (assigns id, register_order_meta for opener/strategy,
    //    instrument spec, stop pending, or submit) — still engine-owned,
    //    calls orders_->process(...) for the immediate/staged-release paths.
    // 4. adapter->submit_order (paper or live); also submit to shadow provider
    //    adapter for dual tracking.
    // 5. adapter->poll_fills → for each fill:
    //      - stamp_fill_attribution (rich opener/strategy from meta or fe)
    //      - order_tracker / cache status
    //      - log + publish order status if needed
    //      - portfolio_.on_fill (rich, with opener/strategy)  [core lot update]
    //      - dispatch_fill_to_strategy
    //      - adverse_selection_.on_fill
    //      - exit_manager_.on_fill (rich)  [arm/cancel brackets per opener]
    //      - risk_manager_.on_fill
    //      - QuestDB record_fill (rich)
    //      - notify_position_change_all (multi-lot aware)
    //      - publish_event(fill) + analytics_.on_event
    //      - shadow_tracker on_sim (if shadow)
    //      - post_fill risk check (can halt + unwind)
    // 6. Shadow dual: separate poll of provider's exchange_adapter fills →
    //      shadow_tracker on_exchange + exchange_portfolio on_fill (rich)
    //      + exchange_analytics
    // 7. evaluate_exits (price or bar) — can emit closes that recurse via
    //      route_order/process (keeps lot/opener discipline).
    // 8. Cache updates for dashboard (open_orders, recent_fills) + rings
    //      publish for workers (after core state for snapshot coherence).
    //
    // Invariants: order attribution registered before any fill can reference
    // it. L2 updates reach queue models before trades (via apply_l2 before
    // on_trade in adapters). No new allocs/JSON on hot path. Multi-lot uses
    // opener_order_id discipline; single-lot may use bulk cancel in notify.
    // ========================================================================

    {
        // R3: one authoritative pass produces the candidate's mark, the
        // account equity, and the mark-to-market instrument/portfolio views.
        // Before R3 this took two separate walks over the position map under
        // two separate lock acquisitions, and the two could disagree.
        auto snap = analytics_.risk_view();
        build_authoritative_risk_view(*o, snap);
        snap.portfolio.daily_realized_loss = risk_manager_.daily_realized_loss();

        // 0.0 when the mark is missing — the same "unusable mark" signal the
        // venue check already fails closed on. A stale-but-present mark is
        // still handed over, exactly as before R3.
        const double order_mark = snap.instrument.mark_price;
        const double marked_equity = snap.portfolio.equity;

        // Venue-specific pre-trade check (futures notional / leverage /
        // liquidation distance) runs first. Refusals here are pure
        // rejections — no halt semantics, since these caps describe
        // the operator's prudent-trading envelope, not a market-wide
        // risk-of-ruin trigger.
        if (risk_check_)
        {
            auto vd = risk_check_->evaluate_with_account_equity(
                *o, portfolio_, order_mark, marked_equity);
            if (!vd.allow)
            {
                const std::string reason_str =
                    "venue risk check refused: " + vd.reason;
                auto rej = acquire_pooled(rejection_pool_,
                    o->get_timestamp(), o->get_symbol(),
                    o->get_order_id(), reason_str);
                hotpath_.log_event(*rej);
                hotpath_.publish_event(rej);
                // Migrated to sink (PR-04)
                audit_sink_.record_order_submitted(*o, "rejected");
                // Use stack buffer for audit detail to avoid string temp on this path (pooled still needs string)
                char venue_reason[128];
                std::snprintf(venue_reason, sizeof(venue_reason), "venue risk check refused: %s", vd.reason.c_str());
                audit_sink_.record_rejection(*o, "venue_risk_reject", venue_reason);
                emit_terminal_transition(o->get_order_id(), o->get_symbol(),
                                         o->get_quantity(), order_status::rejected);

                // Reject, not halt — engine continues. The cap describes
                // what this operator considers prudent, not a market-wide
                // risk-of-ruin condition that should stop everything.
                return true;
            }
        }

        // This candidate has not been transitioned into the active lifecycle
        // yet, so the ledger count is the exact pre-trade capacity.
        auto existing_active_orders = order_tracker_.active_count();
        // A pending stop already owns exactly one slot before it fires. Its
        // conversion to a market/limit order is not a second candidate. Its
        // remaining quantity is likewise already inside the ledger's pending
        // exposure, so subtract it from the candidate's own side to avoid
        // double-counting the same order in the worst case.
        if (order_tracker_.is_active(o->get_order_id()))
        {
            if (existing_active_orders > 0)
                --existing_active_orders;
            const double already_pending =
                order_tracker_.pending_qty(o->get_order_id());
            double& same_side = (o->get_side() == order_side::buy)
                ? snap.instrument.open_buy_qty
                : snap.instrument.open_sell_qty;
            same_side = std::max(0.0, same_side - already_pending);
            if (snap.instrument.open_order_count > 0)
                --snap.instrument.open_order_count;
        }
        o->set_pretrade_open_order_count(existing_active_orders);
        risk_rule rule = risk_rule::none;
        auto action = risk_manager_.check_order(*o, portfolio_, snap,
                                                existing_active_orders, &rule);
        // Backtest research only: portfolio risk breaches reject the trade —
        // never stop the market replay. Live/shadow keep terminal halt even if
        // the soft flag was left true by misconfiguration.
        const bool soft_pf = config_.risk_soft_portfolio_limits
            && config_.mode == engine_mode::backtest;
        if (action == risk_action::halt && soft_pf)
            action = risk_action::reject;
        if (action == risk_action::halt || action == risk_action::reject)
        {
            const char* reason = (action == risk_action::halt)
                ? "risk limit breached - engine halted"
                : (soft_pf
                       ? "order rejected by risk manager (soft portfolio limits)"
                       : "order rejected by risk manager");
            // Stable machine-readable rule id for the audit trail (R3 §10).
            const char* rule_code = to_string(rule);

            auto rej = acquire_pooled(rejection_pool_,
                o->get_timestamp(), o->get_symbol(), o->get_order_id(), reason);
            hotpath_.log_event(*rej);
            hotpath_.publish_event(rej);

            // Migrated to sink (PR-04)
            audit_sink_.record_order_submitted(*o, "rejected");
            audit_sink_.record_rejection(*o, rule_code, reason);
            audit_sink_.record_event(
                "risk_decision",
                o->get_symbol().c_str(),
                o->get_strategy_name().c_str(),
                o->get_order_id(),
                (action == risk_action::halt) ? "halt" : "reject",
                reason,
                rule_code
            );

            emit_terminal_transition(o->get_order_id(), o->get_symbol(),
                                     o->get_quantity(), order_status::rejected);
            if (action == risk_action::halt)

            {
                // Terminal process-wide halt: set halt_flag_ so DataBridge,
                // L2 dispatch, and run loops all stop — not just the local
                // halt_requested out-param (S3: halt is write-once terminal).
                if (config_.risk_unwind)
                    unwind_positions(event_count);
                hotpath_.trigger_halt(reason);
                halt_requested = true;
                return false;
            }
            return true;
        }
    }

    auto adapter = router_.resolve_adapter(o->get_symbol());
    const bool async_submit = router_.is_async_submit(adapter.get());

    // Authoritative ledger registration happens before the order can go live
    // (and before any fill can reference it): symbol, side, quantity and
    // price are what makes the open-order state a risk input rather than a
    // bare status flag. Idempotent — stop conversions and pending releases
    // re-enter here with the same id.
    if (!order_tracker_.register_order(*o))
    {
        hotpath_.trigger_halt(
            "authoritative order registration failed before submission");
        halt_requested = true;
        return false;
    }
    order_tracker_.set_status(o->get_order_id(),
        async_submit ? order_status::pending : order_status::open);
    if (dashboard_builder_) {
        dashboard_builder_->cache_open_order(*o);
        if (async_submit)
            dashboard_builder_->update_open_order_status(o->get_order_id(), "submit_pending");
    }
    hotpath_.log_event(*o);
    if (halt_flag_.load(std::memory_order_acquire))
    {
        emit_terminal_transition(o->get_order_id(), o->get_symbol(),
                                 o->get_quantity(), order_status::rejected);
        halt_requested = true;
        return false;
    }

    hotpath_.publish_event(o);
    analytics_.on_event(o);

    // Migrated to sink (PR-04)
    audit_sink_.record_order_submitted(*o, "pending");
    if (!async_submit)
    {
        audit_sink_.record_status_transition(o->get_order_id(),
            order_status::pending, order_status::open);
    }
    audit_sink_.record_event(
        "order_intent",
        o->get_symbol().c_str(),
        o->get_strategy_name().c_str(),
        o->get_order_id(),
        "info",
        "order generated by strategy",
        "{}"
    );

    adapter->set_mid_price(last_mid_price_.load(std::memory_order_relaxed));
    adapter->set_l2_seeded(l2_seeded_symbols_.count(o->get_symbol()) > 0);

    router_.submit(*o, adapter.get());

    drain_async_submit_results(adapter.get());

    if (config_.mode == engine_mode::shadow && config_.provider)
    {
        auto exchange_adapter = config_.provider->get_execution_adapter();
        if (exchange_adapter)
        {
            if (!fills_.register_shadow_order(*o))
            {
                halt_requested = true;
                return false;
            }
            exchange_adapter->submit_order(*o);
            drain_async_submit_results(exchange_adapter.get());
        }
    }

    if (!fills_.process_adapter_fills(adapter, event_count, halt_requested))
        return false;

    if (config_.mode == engine_mode::shadow && config_.provider)
    {
        auto exchange_adapter = config_.provider->get_execution_adapter();
        if (exchange_adapter)
        {
            drain_async_submit_results(exchange_adapter.get());
            if (!fills_.process_shadow_exchange_fills(
                    exchange_adapter, halt_requested))
                return false;
        }
    }

    event_count++;
    return true;
}

void OrderIntentProcessor::unwind_positions(std::size_t& event_count)
{
    // Snapshot open lots before iterating — each fill mutates lots_ and positions_.
    struct unwind_target
    {
        std::string symbol;
        order_side  close_side;
        double      qty;
        uint64_t    opener_id;
    };

    std::vector<unwind_target> targets;
    std::unordered_set<std::string> symbols_with_lots;

    for (const auto& [id, l] : portfolio_.get_lots())
    {
        if (l.qty_open >= 1e-12)
        {
            const order_side close_side = (l.side == order_side::buy)
                ? order_side::sell : order_side::buy;
            targets.push_back({l.symbol, close_side, l.qty_open, id});
            symbols_with_lots.insert(l.symbol);
        }
    }

    // Fallback: if a symbol has open position in portfolio_.get_positions()
    // but no lots (legacy/un-lotted), close the netted position as well.
    for (const auto& [symbol, pos] : portfolio_.get_positions())
    {
        if (symbols_with_lots.count(symbol) == 0 && std::abs(pos.qty) >= 1e-12)
        {
            const order_side close_side = (pos.qty > 0.0)
                ? order_side::sell : order_side::buy;
            targets.push_back({symbol, close_side, std::abs(pos.qty), 0});
        }
    }

    // An unwind is caused by the currently processed market/risk event. Its
    // simulation timestamp is the authoritative lifecycle clock in replay;
    // wall time would make otherwise identical backtests hash differently.
    if (!targets.empty()
        && last_sim_time_.time_since_epoch().count() == 0)
    {
        hotpath_.trigger_halt(
            "risk unwind refused without an authoritative event timestamp");
        return;
    }
    const auto now = last_sim_time_;
    for (const auto& target : targets)
    {
        auto close_order = acquire_pooled(order_pool_, order_event(
            now, target.symbol, order_type::market, target.close_side,
            target.qty, last_mid_price_.load(std::memory_order_relaxed)));
        close_order->set_order_id(OrderIdGenerator::next());
        close_order->set_opener_order_id(target.opener_id);
        close_order->set_strategy_name("__engine_unwind__");

        if (!order_tracker_.register_order(*close_order))
        {
            hotpath_.trigger_halt(
                "risk unwind order registration failed");
            return;
        }
        attribution_.register_order(*close_order);
        order_tracker_.set_status(close_order->get_order_id(), order_status::open);
        if (dashboard_builder_) dashboard_builder_->cache_open_order(*close_order);
        hotpath_.log_event(*close_order);
        hotpath_.publish_event(close_order);
        analytics_.on_event(close_order);

        // Unconditional via audit_sink (replaces questdb guard + #ifdef).
        audit_sink_.record_order_submitted(*close_order, "pending");
        audit_sink_.record_status_transition(close_order->get_order_id(),
            order_status::pending, order_status::open, "risk_unwind");

        auto adapter = router_.resolve_adapter(target.symbol);
        adapter->set_mid_price(last_mid_price_.load(std::memory_order_relaxed));
        adapter->set_l2_seeded(l2_seeded_symbols_.count(target.symbol) > 0);

        router_.submit(*close_order, adapter.get());

        drain_async_submit_results(adapter.get());

        bool unwind_halt = false;
        // Already in halt/unwind — use the same transactional delivery gate,
        // but suppress recursive post-fill risk in the economic pipeline.
        (void)fills_.process_adapter_fills(
            adapter, event_count, unwind_halt, fill_context::risk_unwind);
    }
}

void OrderIntentProcessor::drain_async_submit_results(IExecutionAdapter* adapter)
{
    auto* cap = adapter ? adapter->get_async_support() : nullptr;
    if (!cap) return;

    std::vector<submit_result> results;
    (void)cap->poll_submit_results(results);

    for (const auto& sr : results)
    {
        if (sr.op == submit_result::operation::submit)
        {
            if (sr.uncertain)
            {
                hotpath_.trigger_halt("venue order outcome is ambiguous after request write");
                audit_sink_.record_status_transition(
                    sr.engine_id, order_status::pending, order_status::pending,
                    "ambiguous post-write submit; terminal halt and reconcile required");
                continue;
            }
            if (sr.fatal)
            {
                hotpath_.trigger_halt("order mutation refused because safety prerequisites failed");
                continue;
            }
            if (sr.ok)
            {
                // The REST response acknowledges only the command. Economic
                // lifecycle state advances exclusively from authoritative
                // user-data evidence delivered below.
                if (dashboard_builder_)
                    dashboard_builder_->update_open_order_status(
                        sr.engine_id, "venue_ack_pending");
                continue;
            }
            if (!order_tracker_.is_active(sr.engine_id)) continue;

            auto rej = acquire_pooled(rejection_pool_,
                std::chrono::system_clock::now(), sr.symbol, sr.engine_id,
                "submit failed: " + sr.error);
            hotpath_.log_event(*rej);
            hotpath_.publish_event(rej);
            emit_terminal_transition(sr.engine_id, sr.symbol,
                                     order_tracker_.pending_qty(sr.engine_id),
                                     order_status::rejected);

            // Unconditional via audit_sink using the single record_rejection shape
            // (the rich order_event overload). For async submit transport errors
            // we synthesize a minimal stack order_event carrying the identity we have
            // (id + symbol + looked-up strategy). qty/price/side are best-effort zeros
            // (the sink path will record zeros for qty/price as before).
            // Strategy lookup mirrors the pattern used for cancellations in the same drain.
            const std::string& strat = attribution_.strategy_for(sr.engine_id);
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
            audit_sink_.record_status_transition(sr.engine_id,
                order_status::pending, order_status::rejected,
                transport_msg);
            audit_sink_.record_rejection(ghost, "transport_error", sr.error.c_str());
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
            hotpath_.trigger_halt("venue cancel outcome is ambiguous after request write");
            if (dashboard_builder_)
                dashboard_builder_->update_open_order_status(sr.engine_id, "cancel_unknown");
        }
        else if (sr.fatal)
        {
            hotpath_.trigger_halt("cancel refused because safety prerequisites failed");
            if (dashboard_builder_)
                dashboard_builder_->update_open_order_status(sr.engine_id, "cancel_refused");
        }
        else if (sr.ok)
        {
            if (dashboard_builder_)
                dashboard_builder_->update_open_order_status(
                    sr.engine_id, "cancel_pending");
        }
        else
        {
            if (dashboard_builder_) dashboard_builder_->update_open_order_status(sr.engine_id, "cancel_failed");
        }

        if (!sr.ok && !sr.uncertain && !sr.fatal
            && meta_it != pending_cancels_.end())
            pending_cancels_.erase(meta_it);
    }

    venue_lifecycle_event lifecycle;
    while (cap->poll_lifecycle_event(lifecycle))
    {
        const auto* tracked = order_tracker_.find(lifecycle.engine_order_id);
        if (!tracked)
        {
            hotpath_.trigger_halt(
                "authoritative venue lifecycle references unknown order");
            continue;
        }
        const std::string& symbol = order_tracker_.symbol_of(*tracked);
        const auto before = tracked->status;

        if (lifecycle.transition == venue_order_transition::acknowledged)
        {
            const auto validation = order_tracker_.validate_lifecycle(
                lifecycle.engine_order_id, order_status::open,
                lifecycle.exchange_ts);
            if (validation.benign_noop()) continue;
            if (!validation.applied()
                || !order_tracker_.commit_lifecycle(validation))
            {
                hotpath_.trigger_halt(
                    "authoritative venue ACK transition is invalid");
                continue;
            }
            if (dashboard_builder_)
                dashboard_builder_->update_open_order_status(
                    lifecycle.engine_order_id, "open");
            audit_sink_.record_status_transition(
                lifecycle.engine_order_id, before, order_status::open,
                "authoritative user-data ACK");
            continue;
        }

        order_status terminal = order_status::unknown;
        switch (lifecycle.transition)
        {
        case venue_order_transition::canceled:
            terminal = order_status::cancelled;
            break;
        case venue_order_transition::rejected:
            terminal = order_status::rejected;
            break;
        case venue_order_transition::expired:
            terminal = order_status::expired;
            break;
        case venue_order_transition::amended:
        case venue_order_transition::cancel_rejected:
        case venue_order_transition::amend_rejected:
            hotpath_.trigger_halt(
                "venue lifecycle transition is explicitly unsupported");
            continue;
        case venue_order_transition::acknowledged:
            continue;
        }

        const double pending = order_tracker_.pending_qty(
            lifecycle.engine_order_id);
        if (!emit_terminal_transition(
                lifecycle.engine_order_id, symbol, pending, terminal,
                lifecycle.exchange_ts))
            continue;

        if (terminal == order_status::cancelled)
        {
            auto meta_it = pending_cancels_.find(lifecycle.engine_order_id);
            const std::string reason =
                meta_it != pending_cancels_.end() && !meta_it->second.reason.empty()
                    ? meta_it->second.reason
                    : "authoritative venue cancel";
            auto cancel_ev = acquire_pooled(
                cancel_pool_, lifecycle.exchange_ts, symbol,
                lifecycle.engine_order_id, reason);
            hotpath_.log_event(*cancel_ev);
            hotpath_.publish_event(cancel_ev);
            if (!config_.has_async_analytics()) analytics_.on_event(cancel_ev);
            if (meta_it != pending_cancels_.end())
                audit_sink_.record_cancellation(
                    lifecycle.engine_order_id, symbol.c_str(),
                    attribution_.strategy_for(
                        lifecycle.engine_order_id).c_str(),
                    reason.c_str());
            if (meta_it != pending_cancels_.end())
                pending_cancels_.erase(meta_it);
        }
        audit_sink_.record_status_transition(
            lifecycle.engine_order_id, before, terminal,
            "authoritative user-data lifecycle");
    }
}

// ============================================================================
// Phase 2: routing, triggering, exits — see order_intent_processor.h.
// ============================================================================

const instrument_spec* OrderIntentProcessor::resolve_instrument_spec(const std::string& symbol)
{
    // instrument_spec_cache_ is a reference (never null) — the former
    // engine::resolve_instrument_spec's null check guarded a unique_ptr that
    // engine's constructor always populates unconditionally before orders_
    // exists, so that branch was already unreachable in practice; the
    // reference type now makes the invariant static instead of runtime.
    return instrument_spec_cache_.resolve_instrument_spec(symbol);
}

bool OrderIntentProcessor::apply_instrument_spec(order_event& o, const instrument_spec& spec) const
{
    return instrument_spec_cache_.apply_instrument_spec(o, spec);
}

void OrderIntentProcessor::build_authoritative_risk_view(
    const order_event& order, risk_snapshot& snap) const
{
    // Sim clock, never wall clock: mark ages must be deterministic under
    // replay. Falls back to the order's own timestamp before the first
    // market event of a run.
    const auto now = (last_sim_time_.time_since_epoch().count() != 0)
        ? last_sim_time_ : order.get_timestamp();

    std::lock_guard<std::mutex> lk(last_mark_prices_mu_);
    truetest::risk::build_risk_view(
        snap, order.get_symbol(), portfolio_, order_tracker_, now,
        config_.risk.max_mark_age_ms,
        [this, now](const std::string& symbol) -> mark_point {
            if (auto it = last_mark_prices_.find(symbol);
                it != last_mark_prices_.end())
                return it->second;
            // The hot loops publish the current symbol's mark to the atomic
            // one step before the map; same observation, so use it rather
            // than reporting a missing mark for the symbol being traded.
            if (symbol == last_mark_symbol_)
            {
                const double px =
                    last_mid_price_.load(std::memory_order_relaxed);
                if (std::isfinite(px) && px > 0.0)
                    return mark_point{px, now};
            }
            return {};
        });
}

double OrderIntentProcessor::mid_for_symbol(const std::string& symbol) const
{
    {
        std::lock_guard<std::mutex> lk(last_mark_prices_mu_);
        if (auto it = last_mark_prices_.find(symbol);
            it != last_mark_prices_.end() && it->second.usable())
            return it->second.price;
    }
    return std::numeric_limits<double>::quiet_NaN();
}

bool OrderIntentProcessor::route(order_event& order,
                                 const std::chrono::system_clock::time_point& sim_time,
                                 std::size_t& event_count, bool& halt_requested,
                                 bool anchor_immediate)
{
    // Terminal halt gate: refuse new submits even if a call site forgot to
    // re-check halt_flag_ (e.g. L2 multi-strategy loop after primary halt).
    if (halt_flag_.load(std::memory_order_acquire))
    {
        halt_requested = true;
        return false;
    }

    // Operator-pause gate: intercept here so every strategy call site is
    // covered by one branch. Strategies still run (so analytics + lots
    // stay live for fills already in flight), but no new orders reach
    // the venue. The intent's order_id stays 0; finalize_route drains
    // exit intents and resyncs optimistic position gates.
    if (pause_all_.load(std::memory_order_acquire))
    {
        (void)sim_time; (void)event_count; (void)halt_requested;
        return true;
    }

    order.set_order_id(OrderIdGenerator::next());
    if (order.get_signal_id() == 0)
        order.set_signal_id(order.get_order_id());
    // F-08: stamp the decision clock before anything downstream reads it.
    // A caller that already knows better (replay, a venue-originated order)
    // keeps its own value.
    if (!order.has_decision_ts() &&
        last_decision_ts_.time_since_epoch().count() != 0)
        order.set_decision_ts(last_decision_ts_);
    if (order.get_protective_exit_ticket() != 0 &&
        !exit_manager_.bind_protective_exit(order.get_protective_exit_ticket(),
                                            order.get_order_id()))
    {
        // A close can be returned by an evaluation pass whose opener became
        // flat re-entrantly (for example, another fill drained during book
        // recentering). The ticket is then intentionally gone; suppress this
        // stale command rather than submitting an unowned close through zero.
        audit_sink_.record_event("exit_lifecycle", order.get_symbol().c_str(),
            order.get_strategy_name().c_str(), order.get_order_id(),
            "stale_suppressed", "protective exit ticket already resolved", "{}");
        return true;
    }
    if (order.get_protective_exit_ticket() != 0)
    {
        order.set_submit_ts(sim_time);
        if (const auto protective =
                exit_manager_.protective_exit_for_order(order.get_order_id()))
        {
            audit_sink_.record_exit_lifecycle(exit_lifecycle_record{
                order.get_signal_id(), order.get_order_id(),
                protective->opener_order_id, 0, order.get_decision_ts(),
                order.get_submit_ts(), order.get_earliest_eligible_ts(), {},
                protective->requested_qty, protective->filled_qty,
                protective->remaining_qty, protective->reason,
                order_status::unknown, order_status::pending,
                protective->symbol.data(), protective->strategy_name.data(),
                "pending", "submitted"});
        }
    }
    if (auto* spec = resolve_instrument_spec(order.get_symbol()))
    {
        if (!apply_instrument_spec(order, *spec))
        {
            const char* reason = "order rejected by venue filter (min_qty/min_notional)";
            auto rej = acquire_pooled(rejection_pool_,
                order.get_timestamp(), order.get_symbol(),
                order.get_order_id(), reason);
            hotpath_.log_event(*rej);
            hotpath_.publish_event(rej);
            // Unconditional via audit_sink (replaces questdb guard + #ifdef + dead total_rejections_).
            audit_sink_.record_order_submitted(order, "rejected");
            audit_sink_.record_rejection(order, "venue_filter", reason);
            emit_terminal_transition(order.get_order_id(), order.get_symbol(),
                                     order.get_quantity(), order_status::rejected);
            (void)event_count;
            if (halt_flag_.load(std::memory_order_acquire))
            {
                halt_requested = true;
                return false;
            }
            return true;
        }
    }

    // A fired engine-owned close reserves this quantity before route().  A
    // second strategy close may use only the unreserved remainder; admitting
    // an overlapping close is how a delayed opposite signal previously
    // crossed an otherwise flat lot into a fresh reverse position.
    if (order.get_protective_exit_ticket() == 0)
    {
        const auto pos_it = portfolio_.get_positions().find(order.get_symbol());
        const double position_qty = pos_it != portfolio_.get_positions().end()
            ? pos_it->second.qty : 0.0;
        const auto effect = classify_inventory_effect(
            order.get_side(), order.get_quantity(), position_qty);
        const double reserved = exit_manager_.reserved_close_qty(
            order.get_symbol(), order.get_side());
        if (effect == inventory_effect::reducing && reserved > 1e-12 &&
            order.get_quantity() > std::max(0.0, std::abs(position_qty) - reserved) + 1e-12)
        {
            constexpr const char* reason =
                "order rejected: protective close already reserves this lot";
            auto rej = acquire_pooled(rejection_pool_, order.get_timestamp(),
                order.get_symbol(), order.get_order_id(), reason);
            hotpath_.log_event(*rej);
            hotpath_.publish_event(rej);
            audit_sink_.record_order_submitted(order, "rejected");
            audit_sink_.record_rejection(order, "close_reservation", reason);
            emit_terminal_transition(order.get_order_id(), order.get_symbol(),
                                     order.get_quantity(), order_status::rejected);
            return true;
        }
    }

    // Reserve the authoritative lifecycle slot before an order can be queued
    // by latency/bar-delay or staged as a stop. This is the sole capacity
    // check at route time; process() subtracts this candidate when it
    // performs the remaining venue and portfolio checks. Registration must
    // precede the slot reservation so a staged order already carries its
    // quantity in the ledger's pending exposure.
    if (!order_tracker_.register_order(order))
    {
        hotpath_.trigger_halt(
            "authoritative order registration failed during routing");
        halt_requested = true;
        return false;
    }
    // Attribution is committed only after the authoritative identity/terms
    // ledger accepted the order. A rejected registration must leave no ghost
    // metadata that a later unknown fill could accidentally inherit.
    attribution_.register_order(order);
    const auto existing_active_orders = order_tracker_.active_count();
    if (order.get_protective_exit_ticket() == 0 &&
        risk_manager_.open_order_limit_reached(existing_active_orders))
    {
        const char* reason = "order rejected by risk manager (max open orders)";
        auto rej = acquire_pooled(rejection_pool_, order.get_timestamp(),
            order.get_symbol(), order.get_order_id(), reason);
        hotpath_.log_event(*rej);
        hotpath_.publish_event(rej);
        audit_sink_.record_order_submitted(order, "rejected");
        audit_sink_.record_rejection(order,
            to_string(risk_rule::max_open_orders), reason);
        emit_terminal_transition(order.get_order_id(), order.get_symbol(),
                                 order.get_quantity(), order_status::rejected);
        if (halt_flag_.load(std::memory_order_acquire))
        {
            halt_requested = true;
            return false;
        }
        return true;

    }
    order.set_pretrade_open_order_count(existing_active_orders);
    order_tracker_.set_status(order.get_order_id(), order_status::pending);

    if (order.get_order_type() == order_type::stop ||
        order.get_order_type() == order_type::stop_limit)
    {
        // A staged stop occupies capacity while it can still execute. Its
        // later conversion keeps the same id and therefore the same slot.
        order_tracker_.set_status(order.get_order_id(), order_status::pending);
        pending_stops_.push_back(acquire_pooled(order_pool_,order));
        return true;
    }

    if (anchor_immediate)
    {
        // Bracket fire: fill where it triggered. Re-center the synthetic
        // book at the anchored fire price (SL/TP level or gap open) within
        // the trigger bar and execute immediately — deferring through
        // execution_bar_delay would discard the fire price and fill at
        // wherever the next bar happens to open. Same convention as the
        // native stop path in check_pending_stops.
        const double ref = order.get_price();
        const double bar_mid = last_mid_price_;
        if (ref > 0.0 && ref != bar_mid)
        {
            auto ob = orderbook_registry_.get_or_create(order.get_symbol());
            if (!mm_threaded_ &&
                !l2_seeded_symbols_.count(order.get_symbol()))
            {
                auto mm_trades = market_maker_.replenish(
                    ob, ref, /*update_history=*/false);
                deliver_mm_book_trades(order.get_symbol(), mm_trades,
                                       sim_time, event_count, halt_requested);
            }
            last_mid_price_ = ref;
        }
        order.set_earliest_eligible_ts(sim_time);
        auto order_ptr = acquire_pooled(order_pool_,order);
        if (order.get_tif() == time_in_force::day)
            pending_scheduler_.mark_day_order(order.get_symbol(), order.get_order_id());
        const bool ok = process(order_ptr, event_count, halt_requested);
        last_mid_price_ = bar_mid;
        return ok;
    }

    if (config_.latency_model)
    {
        auto latency = config_.latency_model->get_order_latency();
        order.set_earliest_eligible_ts(sim_time + latency);
        auto pooled = acquire_pooled(order_pool_, order);
        const auto seq = pending_scheduler_.next_seq();
        pending_scheduler_.schedule_latency(std::move(pooled), seq);
        return true;
    }

    if (config_.execution_bar_delay > 0)
    {
        // The symbol-event scheduler is authoritative. The release boundary
        // stamps the actual future observation time before venue submission.
        order.set_earliest_eligible_ts(sim_time);
        if (pending_scheduler_.bar_delay_capacity_exhausted())
        {
            constexpr const char* reason =
                "order rejected: delayed-order capacity exhausted";
            auto rej = acquire_pooled(rejection_pool_, order.get_timestamp(),
                order.get_symbol(), order.get_order_id(), reason);
            hotpath_.log_event(*rej);
            hotpath_.publish_event(rej);
            if (!config_.has_async_analytics()) analytics_.on_event(rej);
            audit_sink_.record_rejection(order, "capacity_reject", reason);
            emit_terminal_transition(order.get_order_id(), order.get_symbol(),
                                     order.get_quantity(), order_status::rejected);
            return true;

        }
        auto pooled = acquire_pooled(order_pool_, order);
        const auto seq = pending_scheduler_.next_seq();
        pending_scheduler_.schedule_bar_delay(std::move(pooled), seq,
                                              config_.execution_bar_delay);
        return true;
    }

    order.set_earliest_eligible_ts(sim_time);
    auto order_ptr = acquire_pooled(order_pool_,order);
    if (order.get_tif() == time_in_force::day)
        pending_scheduler_.mark_day_order(order.get_symbol(), order.get_order_id());
    return process(order_ptr, event_count, halt_requested);
}

void OrderIntentProcessor::check_pending_stops(std::string_view event_symbol,
                                               double open, double high, double low,
                                               const std::chrono::system_clock::time_point& sim_time,
                                               std::size_t& event_count, bool& halt_requested)
{
    // last_mid_price_ is the bar close when this runs in the bar loops;
    // restored after each anchored fill so subsequent processing keeps
    // the close reference. Tick callers pass open == high == low ==
    // last_mid_price_, making the anchor a no-op there.
    const double bar_mid = last_mid_price_;

    auto it = pending_stops_.begin();
    while (it != pending_stops_.end() &&
           !halt_requested &&
           !halt_flag_.load(std::memory_order_acquire))
    {
        auto& stop = *it;
        if (stop->get_symbol() != event_symbol)
        {
            ++it;
            continue;
        }
        bool triggered = false;

        if (stop->get_side() == order_side::buy && high >= stop->get_stop_price())
            triggered = true;
        else if (stop->get_side() == order_side::sell && low <= stop->get_stop_price())
            triggered = true;

        if (triggered)
        {
            // Fill reference: the stop price, or the bar open when the
            // bar gapped through the stop. Never the close — that is
            // intra-bar look-ahead and deviates from the convention of
            // filling where the stop was hit.
            const double ref = (stop->get_side() == order_side::buy)
                ? ((open >= stop->get_stop_price()) ? open : stop->get_stop_price())
                : ((open <= stop->get_stop_price()) ? open : stop->get_stop_price());

            last_mid_price_ = ref;
            if (ref != bar_mid)
            {
                // Re-center the synthetic book at the trigger so the
                // converted order walks depth priced around ref, not
                // around the previous close. Skipped for real L2 depth
                // and under the threaded MM preset (the worker owns the
                // book there).
                auto ob = orderbook_registry_.get_or_create(stop->get_symbol());
                if (!mm_threaded_ &&
                    !l2_seeded_symbols_.count(stop->get_symbol()))
                {
                    auto mm_trades = market_maker_.replenish(
                        ob, ref, /*update_history=*/false);
                    deliver_mm_book_trades(stop->get_symbol(), mm_trades,
                                           sim_time, event_count, halt_requested);
                }
            }

            if (stop->get_order_type() == order_type::stop)
            {
                auto market_order = acquire_pooled(order_pool_,
                    sim_time, stop->get_symbol(), order_type::market,
                    stop->get_side(), stop->get_quantity(), stop->get_stop_price(),
                    time_in_force::ioc);
                market_order->set_order_id(stop->get_order_id());
                market_order->set_strategy_name(stop->get_strategy_name());
                market_order->set_opener_order_id(stop->get_opener_order_id());
                market_order->set_recv_ns(stop->get_recv_ns());
                market_order->set_earliest_eligible_ts(sim_time);
                if (market_order->get_tif() == time_in_force::day)
                    pending_scheduler_.mark_day_order(market_order->get_symbol(), market_order->get_order_id());
                if (!process(market_order, event_count, halt_requested))
                {
                    last_mid_price_ = bar_mid;
                    return;
                }
            }
            else
            {
                auto limit_order = acquire_pooled(order_pool_,
                    sim_time, stop->get_symbol(), order_type::limit,
                    stop->get_side(), stop->get_quantity(), stop->get_price(),
                    stop->get_tif());
                limit_order->set_order_id(stop->get_order_id());
                limit_order->set_strategy_name(stop->get_strategy_name());
                limit_order->set_opener_order_id(stop->get_opener_order_id());
                limit_order->set_recv_ns(stop->get_recv_ns());
                limit_order->set_earliest_eligible_ts(sim_time);
                if (limit_order->get_tif() == time_in_force::day)
                    pending_scheduler_.mark_day_order(limit_order->get_symbol(), limit_order->get_order_id());
                if (!process(limit_order, event_count, halt_requested))
                {
                    last_mid_price_ = bar_mid;
                    return;
                }
            }
            last_mid_price_ = bar_mid;
            it = pending_stops_.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

double OrderIntentProcessor::sweep_resting_limits(const std::string& symbol,
                                                  double low, double high,
                                                  const std::chrono::system_clock::time_point& ts,
                                                  std::size_t& event_count, bool& halt_requested,
                                                  double bar_volume)
{
    auto it = execution_adapters_.find(symbol);
    if (it == execution_adapters_.end() || !it->second)
        return 0.0;
    // Virtual dispatch (same capability surface as deliver_mm_book_trades).
    if (it->second->sweep_resting_range(symbol, low, high, ts, bar_volume))
    {
        const double consumed = std::clamp(
            it->second->last_sweep_fill_qty(), 0.0,
            std::max(0.0, bar_volume));
        fills_.process_adapter_fills(it->second, event_count, halt_requested);
        return consumed;
    }
    return 0.0;
}

void OrderIntentProcessor::deliver_mm_book_trades(const std::string& symbol, const trades& trs,
                                                  const std::chrono::system_clock::time_point& ts,
                                                  std::size_t& event_count, bool& halt_requested)
{
    if (trs.empty())
        return;
    // Only adapters that already exist can hold resting strategy orders;
    // don't create one just to deliver MM-vs-MM crossings.
    // Virtual dispatch: LocalBookAdapter records fills; HybridExecutor
    // forwards to its inner book adapter; live bridges no-op.
    auto it = execution_adapters_.find(symbol);
    if (it == execution_adapters_.end() || !it->second)
        return;
    it->second->on_book_trades(trs, ts);
    fills_.process_adapter_fills(it->second, event_count, halt_requested);
}

bool OrderIntentProcessor::evaluate_exits(const std::string& symbol, double px,
                                          std::chrono::system_clock::time_point ts,
                                          std::size_t& event_count,
                                          std::int64_t recv_ns)
{
    // See canonical sequence comment in process(). Closes emitted here go
    // through route (which registers attribution) + process to maintain
    // per-lot / opener discipline and full state propagation.
    if (route_exit_flatten_requests(symbol, event_count, recv_ns))
        return true;
    auto closes = exit_manager_.on_price(symbol, px, ts);
    if (closes.empty()) return false;

    for (auto& close : closes)
    {
        close.set_recv_ns(recv_ns);
        bool halt = false;
        route(close, ts, event_count, halt, /*anchor_immediate=*/true);
        if (halt) return true;
    }
    return false;
}

bool OrderIntentProcessor::evaluate_exits(const std::string& symbol,
                                          double open, double low, double high, double close,
                                          std::chrono::system_clock::time_point ts,
                                          std::size_t& event_count,
                                          std::int64_t recv_ns)
{
    // See canonical sequence comment in process(). Bar fires go through
    // route for consistent attribution registration and full propagation,
    // anchored at the fire price computed within the trigger bar.
    if (route_exit_flatten_requests(symbol, event_count, recv_ns))
        return true;
    auto fires = exit_manager_.on_bar(symbol, open, low, high, close, ts);
    if (fires.empty()) return false;

    for (auto& c : fires)
    {
        c.set_recv_ns(recv_ns);
        bool halt = false;
        route(c, ts, event_count, halt, /*anchor_immediate=*/true);
        if (halt) return true;
    }
    return false;
}

bool OrderIntentProcessor::route_exit_flatten_requests(
    const std::string& symbol,
    std::size_t& event_count,
    std::int64_t recv_ns)
{
    // F-01(a): ExitManager refused to arm a bracket because the entry
    // slippage reached the trade's own designed stop distance. Shifting the
    // stop by that much would put it through its trigger the instant it was
    // armed (the phantom stop-outs F-01 documents); keeping the unshifted
    // stop would leave a lot whose realized risk exceeds its budget. Neither
    // is acceptable, so the lot is closed at the current mark instead.
    //
    // The request captures the fill observation that invalidated its premise.
    // Do not drain another symbol's request, and do not re-price this at the
    // current bar close: both would turn a bar-open fill into future data.
    if (!exit_manager_.has_flatten_requests()) return false;

    auto reqs = exit_manager_.take_flatten_requests_for(symbol);
    for (const auto& r : reqs)
    {
        if (!(r.qty > 0.0) || !(r.trigger_price > 0.0))
        {
            hotpath_.trigger_halt("protective slippage flatten has no valid trigger mark");
            return true;
        }
        order_event close(r.trigger_ts, r.symbol, order_type::market, r.close_side,
                          r.qty, r.trigger_price);
        close.set_opener_order_id(r.opener_order_id);
        close.set_strategy_name(r.strategy_name);
        close.set_protective_exit_ticket(r.protective_exit_ticket);
        close.set_exit_reason(order_exit_reason::slippage_flatten);
        close.set_recv_ns(recv_ns);
        bool halt = false;
        route(close, r.trigger_ts, event_count, halt, /*anchor_immediate=*/true);
        if (halt) return true;
    }
    return false;
}

void OrderIntentProcessor::finalize_route(IStrategy& strategy,

                                          const std::string& strategy_name,
                                          const order_event& order,
                                          bool halted,
                                          std::optional<double> pre_route_net_qty)
{
    if (halted)
    {
        // Drop pending exit intents; do not arm brackets after terminal halt.
        (void)strategy.take_pending_exit_intents();
        return;
    }

    const uint64_t oid = order.get_order_id();
    if (oid == 0)
    {
        // Paused / never assigned — drain intents and unlock optimistic gates.
        (void)strategy.take_pending_exit_intents();
        fills_.notify_position_change_all(order.get_symbol(),
                                          portfolio_.position_open(order.get_symbol()));
        return;
    }

    const auto st = order_tracker_.get_order_status(oid);
    if (st == order_status::rejected)
    {
        // Venue/risk reject after id assignment — do not arm exits; resync gates.
        (void)strategy.take_pending_exit_intents();
        fills_.notify_position_change_all(order.get_symbol(),
                                          portfolio_.position_open(order.get_symbol()));
        return;
    }

    register_strategy_exit_intent(strategy, strategy_name, order, pre_route_net_qty);
}

void OrderIntentProcessor::register_strategy_exit_intent(IStrategy& strategy,
                                                          const std::string& strategy_name,
                                                          const order_event& order,
                                                          std::optional<double> pre_route_net_qty)
{
    const std::uint64_t order_id = order.get_order_id();
    if (order_id == 0)
    {
        // Opener not assigned (pause/drop) — drain so intents cannot leak
        // and re-arm on a later unrelated entry.
        (void)strategy.take_pending_exit_intents();
        return;
    }
    auto intents = strategy.take_pending_exit_intents();

    // Platform floor / engine_only / union: attach protective SL/TP for any
    // strategy that omitted them. Position-reducing signal closes are skipped
    // so death-cross sells do not arm inverted short brackets.
    double net_qty = 0.0;
    if (pre_route_net_qty.has_value())
    {
        net_qty = *pre_route_net_qty;
    }
    else if (!strategy_name.empty())
    {
        net_qty = portfolio_.get_strategy_position_qty(strategy_name, order.get_symbol());
    }
    else
    {
        const auto& positions = portfolio_.get_positions();
        auto it = positions.find(order.get_symbol());
        if (it != positions.end())
            net_qty = it->second.qty;
    }
    intents = truetest::exits::apply_default_exit_policy(
        config_.exit_defaults, order, net_qty, std::move(intents));

    for (auto& intent : intents)
    {
        intent.opener_order_id = order_id;
        intent.strategy_name   = strategy_name;
        exit_manager_.register_pending(std::move(intent));
    }
}

void OrderIntentProcessor::drain_due(
    const std::chrono::system_clock::time_point& sim_time,
    std::size_t& event_count, bool& halt_requested,
    std::string_view event_symbol)
{
    // Preserve the event-loop mid (open/tick of the current symbol). Each
    // pending order may belong to a different symbol; fill mid and MM
    // re-center must track the order's marks, not the event's (EL-MULTISYM-MID).
    // F-01(b): open this observation's exit-evaluation window before any
    // delayed order can fill inside it. Every bracket armed from a fill in
    // this window carries this epoch, so this window's on_bar knows the bar
    // open is a price printed before the bracket existed and refuses to
    // anchor a gap fill there. drain_due is the one call that precedes
    // evaluate_exits on every market path (bar, tick, L2, streaming, batch),
    // which is why the window opens here.
    exit_manager_.begin_evaluation_window();


    const double event_mid = last_mid_price_.load(std::memory_order_relaxed);
    auto submit = [&](const std::shared_ptr<order_event>& order) {

        if (!order)
            return true;
        const auto& sym = order->get_symbol();
        const double mid = mid_for_symbol(sym);
        if (!std::isfinite(mid) || mid <= 0.0)
        {
            hotpath_.trigger_halt("pending order has no valid same-symbol mark");
            halt_requested = true;
            return false;
        }
        last_mid_price_.store(mid, std::memory_order_release);

        if (!mm_threaded_ && !l2_seeded_symbols_.count(sym))
        {
            auto ob = orderbook_registry_.get_or_create(sym);
            auto mm_trades = market_maker_.replenish(
                ob, mid, /*update_history=*/false);
            deliver_mm_book_trades(sym, mm_trades, sim_time,
                                   event_count, halt_requested);
            if (halt_requested || halt_flag_.load(std::memory_order_acquire))
                return false;
        }

        if (order->get_tif() == time_in_force::day)
            pending_scheduler_.mark_day_order(sym, order->get_order_id());
        return process(order, event_count, halt_requested);
    };

    while (pending_scheduler_.latency_due(sim_time) &&
           !halt_requested &&
           !halt_flag_.load(std::memory_order_acquire))
    {
        auto order = pending_scheduler_.pop_due_latency();
        if (!submit(order))
        {
            last_mid_price_.store(event_mid, std::memory_order_release);
            return;
        }
    }
    if (halt_requested || halt_flag_.load(std::memory_order_acquire))
    {
        last_mid_price_.store(event_mid, std::memory_order_release);
        return;
    }

    // A non-empty ready buffer means a prior submission stopped on a terminal
    // halt before its remaining due orders could be restored. Never resume or
    // silently discard that retained state.
    if (pending_scheduler_.has_retained_ready())
    {
        hotpath_.trigger_halt("delayed-order scheduler retained ready orders after halt");
        halt_requested = true;
        last_mid_price_.store(event_mid, std::memory_order_release);
        return;
    }

    // compact_bar_delay_due does the capacity pre-check + the stable,
    // allocation-free single-pass compaction (survivors retain insertion
    // order in the delayed buffer; due entries retain insertion order in the
    // prewarmed ready buffer). Submission happens only after compaction so an
    // order routed by a submission can use the slot just released and cannot
    // be counted by the same observation.
    if (!pending_scheduler_.compact_bar_delay_due(event_symbol))
    {
        hotpath_.trigger_halt("delayed-order scheduler ready capacity exhausted");
        halt_requested = true;
        last_mid_price_.store(event_mid, std::memory_order_release);
        return;
    }

    for (std::size_t i = 0; i < pending_scheduler_.ready_count(); ++i)
    {
        if (halt_requested || halt_flag_.load(std::memory_order_acquire))
        {
            pending_scheduler_.retain_ready_suffix(i);
            last_mid_price_.store(event_mid, std::memory_order_release);
            return;
        }
        auto order = pending_scheduler_.take_ready_order(i);
        // Bar-delay fills belong to the observation that released them, not
        // to the signal timestamp plus an artificial nanosecond.
        order->set_earliest_eligible_ts(sim_time);
        if (!submit(order))
        {
            // The failed order matches the old erase-before-submit behavior.
            // Restore every not-yet-submitted due order in global insertion
            // order when bounded capacity permits. This is a linear in-place
            // merge; it allocates nothing. If a pathological re-entrant route
            // consumed those slots, retain the suffix in ready_ for the final
            // fail-closed expiry path instead of growing or dropping it.
            const std::size_t first_unsubmitted = i + 1;
            pending_scheduler_.retain_ready_suffix(first_unsubmitted);
            last_mid_price_.store(event_mid, std::memory_order_release);
            return;
        }
    }
    pending_scheduler_.clear_ready();
    last_mid_price_.store(event_mid, std::memory_order_release);
}

// ============================================================================
// Phase 3: cancel/modify, end-of-stream lifecycle — see order_intent_processor.h.
// ============================================================================

bool OrderIntentProcessor::cancel(const std::string& symbol, uint64_t order_id,
                                  const std::string& reason)
{
    auto adapter = router_.resolve_adapter(symbol);

    drain_async_submit_results(adapter.get());
    if (halt_flag_.load(std::memory_order_acquire))
        return false;

    bool cancelled = adapter->cancel_order(order_id);

    if (cancelled && adapter->supports_async_submit())
    {
        pending_cancels_[order_id] = pending_cancel_meta{symbol, reason};
        if (dashboard_builder_) dashboard_builder_->update_open_order_status(order_id, "cancel_pending");
        return true;
    }

    if (!cancelled)
    {
        auto it = std::remove_if(pending_stops_.begin(), pending_stops_.end(),
            [order_id](const std::shared_ptr<order_event>& o) {
                return o->get_order_id() == order_id;
            });
        if (it != pending_stops_.end())
        {
            pending_stops_.erase(it, pending_stops_.end());
            cancelled = true;
        }
    }

    if (cancelled)
    {
        emit_terminal_transition(order_id, symbol,
                                 order_tracker_.pending_qty(order_id),
                                 order_status::cancelled);
        // Prefer last sim time (EL-CANCEL-WALLCLOCK); wall clock only if no event yet.
        const auto cancel_ts = (last_sim_time_.time_since_epoch().count() != 0)
            ? last_sim_time_
            : std::chrono::system_clock::now();
        auto cancel_ev = acquire_pooled(cancel_pool_,
            cancel_ts, symbol, order_id, reason);
        hotpath_.log_event(*cancel_ev);
        hotpath_.publish_event(cancel_ev);
        if (!config_.has_async_analytics())
            analytics_.on_event(cancel_ev);
        // Unconditional via audit_sink (replaces questdb guard + #ifdef).
        //
        // Late-fill safety note (unchanged from the pre-move original): a
        // successful cancel here — synchronous local/paper adapter cancel,
        // or the async cancel-pending branch above — records the engine's
        // own lifecycle transition. It does NOT retroactively guarantee no
        // fill for order_id can still arrive: a venue-originated fill
        // already in flight when the cancel request was written is a
        // documented residual risk this move does not alter, fix, or
        // paper over. See the repository's trading-logic audit for the
        // full caveat; do not "fix" it here.
        audit_sink_.record_cancellation(order_id, symbol.c_str(),
            attribution_.strategy_for(order_id).c_str(),
            reason.empty() ? "manual" : reason.c_str());
        audit_sink_.record_status_transition(order_id,
            order_status::open, order_status::cancelled, reason.empty() ? nullptr : reason.c_str());
    }

    return cancelled;
}

bool OrderIntentProcessor::modify(const std::string& symbol, uint64_t order_id,
                                  double new_price, double new_total_qty)
{
    // Public amend semantics are NEW TOTAL quantity.  A venue/local adapter
    // receives only the derived remaining quantity after confirmed fills.
    // This distinction is mandatory for a partially-filled order: total=6
    // after cumulative fills=4 means remaining=2, never a replacement body
    // of 6 that could take the economic total to 10.
    if (halt_flag_.load(std::memory_order_acquire) ||
        pause_all_.load(std::memory_order_acquire))
        return false;

    auto adapter = router_.resolve_adapter(symbol);
    if (!adapter)
        return false;
    drain_async_submit_results(adapter.get());
    if (halt_flag_.load(std::memory_order_acquire))
        return false;

    const auto* tracked = order_tracker_.find(order_id);
    if (!tracked || order_tracker_.symbol_of(*tracked) != symbol)
        return false;

    // A synchronous bool-returning adapter cannot safely represent a pending
    // venue amend/ACK/reject lifecycle.  Current live bridges deliberately do
    // not implement modify_order; only an acknowledged resting limit is
    // admitted here. Other types/states remain explicit fail-closed.
    if ((tracked->status != order_status::open
         && tracked->status != order_status::partially_filled)
        || tracked->type != order_type::limit)
        return false;

    const auto now = last_sim_time_.time_since_epoch().count() != 0
        ? last_sim_time_ : tracked->updated_ts;
    if (now.time_since_epoch().count() == 0)
        return false;

    // Normalize and venue-filter the requested TOTAL terms exactly once,
    // before any adapter mutation.  The normalized values are the only terms
    // allowed into risk, venue state, ledger, event log, and reporting.
    order_event normalized_total(
        now, symbol, tracked->type, tracked->side,
        new_total_qty, new_price, time_in_force::gtc);
    normalized_total.set_order_id(order_id);
    const auto* spec = resolve_instrument_spec(symbol);
    if (spec)
    {
        if (!apply_instrument_spec(normalized_total, *spec))
            return false;
    }

    const auto amend_validation = order_tracker_.validate_amend(
        order_id, symbol, normalized_total.get_price(),
        normalized_total.get_quantity());
    if (!amend_validation.applied())
        return false;

    const double new_remaining_qty =
        amend_validation.new_remaining_qty();
    if (!std::isfinite(new_remaining_qty)
        || !(new_remaining_qty > OrderTracker::qty_epsilon))
        return false;

    // Risk evaluates the full projected state: current portfolio + every
    // other pending order + this amended remainder. Remove the old remainder
    // for this order from the authoritative view before presenting the
    // replacement candidate, otherwise the same order is counted twice.
    order_event projected(
        now, symbol, tracked->type, tracked->side,
        new_remaining_qty, normalized_total.get_price(), time_in_force::gtc);
    projected.set_order_id(order_id);
    projected.set_strategy_name(attribution_.strategy_for(order_id));

    // Venue replacement filters apply to the quantity that will actually
    // remain on the book, not to the new economic total (which still includes
    // already confirmed fills). Re-normalizing silently would desynchronise
    // the adapter quantity from the ledger total, so any lot-size change is a
    // rejection rather than an implicit second amendment.
    if (spec)
    {
        const double normalized_remaining = spec->lot_size > 0.0
            ? floor_qty_to_lot(new_remaining_qty, spec->lot_size)
            : new_remaining_qty;
        const double scale = std::max(1.0, std::abs(new_remaining_qty));
        if (!std::isfinite(normalized_remaining)
            || std::abs(normalized_remaining - new_remaining_qty)
                > OrderTracker::qty_epsilon * scale
            || !meets_min_qty(new_remaining_qty, spec->min_qty)
            || !meets_min_notional(
                new_remaining_qty, normalized_total.get_price(),
                spec->min_notional))
            return false;
    }

    auto snap = analytics_.risk_view();
    build_authoritative_risk_view(projected, snap);
    snap.portfolio.daily_realized_loss = risk_manager_.daily_realized_loss();
    const double old_pending = order_tracker_.pending_qty(order_id);
    double& same_side = tracked->side == order_side::buy
        ? snap.instrument.open_buy_qty
        : snap.instrument.open_sell_qty;
    same_side = std::max(0.0, same_side - old_pending);
    if (snap.instrument.open_order_count > 0)
        --snap.instrument.open_order_count;
    auto other_active_orders = order_tracker_.active_count();
    if (other_active_orders > 0)
        --other_active_orders;
    projected.set_pretrade_open_order_count(other_active_orders);

    // Venue-specific risk remains ordered before generic portfolio risk.
    if (risk_check_)
    {
        const auto venue_decision = risk_check_->evaluate_with_account_equity(
            projected, portfolio_, snap.instrument.mark_price,
            snap.portfolio.equity);
        if (!venue_decision.allow)
            return false;
    }

    risk_rule rule = risk_rule::none;
    auto amend_risk_action = risk_manager_.check_order(
        projected, portfolio_, snap, other_active_orders, &rule);
    const bool soft_portfolio_limit = config_.risk_soft_portfolio_limits
        && config_.mode == engine_mode::backtest;
    if (amend_risk_action == risk_action::halt && soft_portfolio_limit)
        amend_risk_action = risk_action::reject;
    if (amend_risk_action == risk_action::halt)
    {
        audit_sink_.record_event(
            "risk_decision", symbol.c_str(),
            projected.get_strategy_name().c_str(), order_id, "halt",
            "amend rejected by risk manager; engine halted", to_string(rule));
        hotpath_.trigger_halt("amend risk limit breached - engine halted");
        return false;
    }
    if (amend_risk_action == risk_action::reject)
    {
        audit_sink_.record_event(
            "risk_decision", symbol.c_str(),
            projected.get_strategy_name().c_str(), order_id, "reject",
            "amend rejected by risk manager", to_string(rule));
        return false;
    }

    // Reserve the only fallible engine resource before the adapter commit.
    // Pool exhaustion therefore cannot leave venue/local book state ahead of
    // the ledger and event stream.
    auto amend_ev = acquire_pooled(
        amend_pool_, now, symbol, order_id,
        normalized_total.get_price(), normalized_total.get_quantity());

    const bool modified = adapter->modify_order(
        order_id, normalized_total.get_price(), new_remaining_qty);

    if (modified)
    {
        // The engine loop is the sole ledger writer. A revision mismatch here
        // means an adapter re-entered or another lifecycle mutation crossed
        // this supposedly synchronous commit. The venue may already differ,
        // so this is terminal reconciliation, never a recoverable false.
        if (!order_tracker_.commit_amend(amend_validation))
        {
            audit_sink_.record_event(
                "order_lifecycle", symbol.c_str(),
                projected.get_strategy_name().c_str(), order_id,
                "amend_commit_fault",
                "adapter amended but authoritative ledger token became stale",
                "{}");
            hotpath_.trigger_halt(
                "adapter amend committed but ledger commit failed");
            return false;
        }
        hotpath_.log_event(*amend_ev);
        hotpath_.publish_event(amend_ev);
        if (!config_.has_async_analytics())
            analytics_.on_event(amend_ev);
        audit_sink_.record_amendment(order_id, symbol.c_str(),
            amend_validation.old_price, amend_validation.new_price,
            amend_validation.old_total_qty,
            amend_validation.new_total_qty, now);

        if (dashboard_builder_)
        {
            normalized_total.set_strategy_name(
                projected.get_strategy_name());
            dashboard_builder_->cache_open_order(normalized_total);
            if (tracked->status == order_status::partially_filled)
                dashboard_builder_->update_open_order_status(order_id, "partial");
        }
    }

    return modified;
}

void OrderIntentProcessor::finalize_end_of_stream(std::size_t& event_count, bool& halt_requested)
{
    // No future market observation exists at EOF. Submitting here fabricates
    // both liquidity and a causal timestamp. Expire every never-submitted
    // candidate and release its authoritative lifecycle slot instead.
    (void)halt_requested;
    auto expire = [&](const std::shared_ptr<order_event>& order) {
        if (!order)
            return;
        const auto& sym = order->get_symbol();
        const auto order_id = order->get_order_id();
        // R3: EOS expiry is its own terminal state. It is not an operator
        // cancel, and conflating the two hid genuine expiries in the audit.
        emit_terminal_transition(order_id, sym, order->get_quantity(),
                                 order_status::expired);
        const auto ts = last_sim_time_.time_since_epoch().count() != 0

            ? last_sim_time_ : order->get_timestamp();
        constexpr const char* reason =
            "backtest_eos_without_future_market_event";
        auto cancel_ev = acquire_pooled(cancel_pool_, ts, sym, order_id, reason);
        hotpath_.log_event(*cancel_ev);
        hotpath_.publish_event(cancel_ev);
        if (!config_.has_async_analytics())
            analytics_.on_event(cancel_ev);
        audit_sink_.record_status_transition(order_id,
            order_status::pending, order_status::expired, reason);
        ++event_count;
    };

    // Former engine::drain_final_pending. Enumerates latency-queue pop-all,
    // then surviving bar-delayed orders, then any retained ready entries, in
    // that exact order (the scheduler's own expire_all — see its header for
    // the ordering guarantee).
    pending_scheduler_.expire_all(expire);

    // Former engine::cancel_day_orders. Route through the full cancel path
    // so Hybrid/QueueAware paper state is cleared (book-only cancel left
    // QueueAware DAY limits as zombies).
    for (const auto& [symbol, oid] : pending_scheduler_.day_orders())
        cancel(symbol, oid, "day_tif_eos");
    pending_scheduler_.clear_day_orders();
    // Latency models only schedule cancels; without a flush, DAY residuals stay
    // in live_quote_count until advance_time. At EOS force-apply pending cancels.
    if (config_.latency_model)
    {
        const auto flush_ts = std::chrono::system_clock::time_point::max();
        router_.advance_all(flush_ts);
    }
}
