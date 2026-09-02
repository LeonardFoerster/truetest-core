#include "exits/exit_manager.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <utility>

namespace truetest::exits {

void ExitManager::begin_evaluation_window()
{
    ++eval_epoch_;
    armed_this_window_.clear();
    engine_drives_windows_ = true;
    sweep_orphan_opener_fills();
}


void ExitManager::register_pending(exit_intent intent)
{
    if (intent.opener_order_id == 0) return;  // cannot key - drop silently
    const std::uint64_t opener = intent.opener_order_id;

    // F-01(a): this opener's bracket was already refused and the lot is
    // being flattened. Registering a sibling intent now would re-arm the
    // exact risk premise that was just declared void.
    //
    // Both this and the orphan lookup below are guarded on empty(): a healthy
    // run never disarms and never orphans, so the steady-state cost of the
    // F-01/F-03 bookkeeping is one predictable branch, not two hash lookups
    // on every single entry.
    if (!disarmed_openers_.empty() && disarmed_openers_.count(opener) != 0)
        return;

    strategy_symbol_key ssk{intent.strategy_name, intent.symbol};

    // F-03: the opener already filled before this intent existed. With
    // execution_bar_delay == 0 the fill happens inside route() while this
    // call runs afterwards in finalize_route, so the pending_ -> armed_
    // promotion on_fill would have performed has already been missed. Arm
    // straight from the recorded fill instead of parking the intent in
    // pending_ to wait for a promotion that cannot come.
    if (!orphan_opener_fills_.empty())
    {
        auto orphan = orphan_opener_fills_.find(opener);
        if (orphan != orphan_opener_fills_.end() && orphan->second.qty > 0.0)
        {
            strategy_symbol_to_openers_.emplace(ssk, opener);
            ++counters_.pending_registered;
            const double fill_qty = orphan->second.qty;
            const double vwap     = orphan->second.vwap;
            const auto   ts       = orphan->second.ts;
            const bool   applied  = orphan->second.remaining_applied;
            if (arm_one(opener, std::move(intent), vwap, fill_qty, ts,
                        /*deferred=*/true) && !applied)
            {
                opener_remaining_qty_[opener] += fill_qty;
                // Re-find: arm_one may have rehashed the map.
                auto again = orphan_opener_fills_.find(opener);
                if (again != orphan_opener_fills_.end())
                    again->second.remaining_applied = true;
            }
            return;
        }
    }

    strategy_symbol_to_openers_.emplace(ssk, opener);

    pending_.emplace(opener, std::move(intent));
    ++counters_.pending_registered;
    enforce_pending_bound();
}

bool ExitManager::shift_entry_relative_levels(exit_intent& intent,
                                              double designed_stop_distance,
                                              double new_entry)
{
    if (!intent.reference_entry || !(*intent.reference_entry > 0.0))
        return true;

    const double delta = new_entry - *intent.reference_entry;
    if (std::abs(delta) <= 1e-15)
    {
        intent.reference_entry = new_entry;
        return true;
    }

    // F-01(a): an entry-relative shift is only meaningful while the slippage
    // is small relative to the designed risk. At |delta| >= designed the
    // shifted stop sits on the far side of the market that produced the fill
    // (a long filled at the ask, stopped at ask - designed, is above the
    // mid), so the bracket is through its own trigger the instant it is
    // armed. Refuse rather than "clamp to something close enough": the
    // condition means the entry should very likely not have been taken.
    if (designed_stop_distance > 0.0 &&
        std::abs(delta) >= designed_stop_distance)
        return false;

    if (intent.stop_loss)   *intent.stop_loss   += delta;
    if (intent.take_profit) *intent.take_profit += delta;
    intent.reference_entry = new_entry;
    return true;
}

bool ExitManager::arm_one(std::uint64_t opener_order_id,
                          exit_intent intent,
                          double fill_price, double fill_qty,
                          std::chrono::system_clock::time_point ts,
                          bool deferred)
{
    // Designed risk distance is measured against the intent as the strategy
    // declared it, before any entry-relative shift.
    const double designed =
        (intent.reference_entry && intent.stop_loss)
            ? std::abs(*intent.reference_entry - *intent.stop_loss)
            : 0.0;

    armed_intent ai;

    ai.intent      = std::move(intent);
    ai.entry_price = fill_price;
    ai.best_price  = fill_price;
    ai.ts_armed    = ts;
    if (armed_this_window_.size() >= kMaxArmedPerWindow)
        armed_this_window_.clear();
    if (armed_this_window_.empty() || armed_this_window_.back() != opener_order_id)
        armed_this_window_.push_back(opener_order_id);

    double frac = ai.intent.qty_fraction;


    if (frac <= 0.0) frac = 1.0;
    if (frac > 1.0)  frac = 1.0;
    ai.intent.qty = fill_qty * frac;

    // Entry-relative brackets: preserve designed |entry - SL/TP| distance
    // when the opener fills away from the intended price. Absolute structure
    // levels leave reference_entry unset and are never shifted.
    if (!shift_entry_relative_levels(ai.intent, designed, fill_price))
    {
        const double slippage =
            ai.intent.reference_entry
                ? std::abs(fill_price - *ai.intent.reference_entry) : 0.0;
        disarm_and_flatten(opener_order_id, ai.intent, fill_qty, fill_price,
                           designed, slippage, ts);
        return false;
    }


    // The venue copy is only built when there is an adapter to hand it to;
    // the backtest/shadow path (no adapter) must not pay for two string
    // copies on every arm.
    const bool first_for_opener =
        bracket_adapter_ && (armed_.count(opener_order_id) == 0);
    exit_intent placed_copy;
    if (first_for_opener) placed_copy = ai.intent;

    armed_.emplace(opener_order_id, std::move(ai));
    ++counters_.armed;
    if (deferred) ++counters_.deferred_arms;

    // Defense-in-depth: hand the venue adapter a copy so it can place
    // resting orders. Empty handles -> engine-side eval is the only path
    // (already armed above; nothing else to do). For multi-intent openers
    // (TP1/TP2/SL scale-outs) we delegate the first one only - adapters that
    // don't model partial brackets natively must short-circuit.
    if (first_for_opener)
    {
        auto handles = bracket_adapter_->place(
            opener_order_id, placed_copy, fill_price);

        if (!handles.empty())
        {
            std::lock_guard<std::mutex> lk(venue_mu_);
            if (handles.sl_exchange_id)
                exchange_to_leg_.emplace(
                    *handles.sl_exchange_id,
                    exchange_leg{opener_order_id, placed_copy.strategy_name,
                                 placed_copy.stop_loss.value_or(0.0)});
            if (handles.tp_exchange_id)
                exchange_to_leg_.emplace(
                    *handles.tp_exchange_id,
                    exchange_leg{opener_order_id, placed_copy.strategy_name,
                                 placed_copy.take_profit.value_or(0.0)});
            handles_.emplace(opener_order_id, std::move(handles));
        }
    }
    return true;
}

void ExitManager::request_flatten(std::uint64_t opener_order_id,
                                  const exit_intent& intent,
                                  double qty, double entry_price,
                                  double designed_stop_distance,
                                  double entry_slippage,
                                  std::chrono::system_clock::time_point trigger_ts)
{
    if (!(qty > 0.0)) return;
    flatten_request req;
    req.symbol                 = intent.symbol;
    req.strategy_name          = intent.strategy_name;
    req.close_side             = intent.close_side;
    req.qty                    = qty;
    req.opener_order_id        = opener_order_id;
    req.entry_price            = entry_price;
    req.designed_stop_distance = designed_stop_distance;
    req.entry_slippage         = entry_slippage;
    // This is the fill observation that invalidated the risk premise.  The
    // caller must not later substitute a bar close (or another symbol's mark)
    // when it finally routes the flatten.
    req.trigger_price          = entry_price;
    req.trigger_ts             = trigger_ts;
    req.protective_exit_ticket = remember_fired_protection(
        opener_order_id, qty, order_exit_reason::slippage_flatten,
        intent.close_side, intent.symbol, intent.strategy_name);
    flatten_requests_.push_back(std::move(req));
    ++counters_.flatten_requests;
}

void ExitManager::disarm_and_flatten(std::uint64_t opener_order_id,
                                     const exit_intent& sample,
                                     double qty, double entry_price,
                                     double designed_stop_distance,
                                     double entry_slippage,
                                     std::chrono::system_clock::time_point trigger_ts)
{
    // Drop everything already armed for this opener - a sibling TP armed on
    // the same void premise must not survive the disarm.
    armed_.erase(opener_order_id);
    pending_.erase(opener_order_id);
    release_venue_bracket(opener_order_id);

    ++counters_.slippage_disarms;
    if (disarmed_openers_.insert(opener_order_id).second)
    {
        std::fprintf(stderr,
            "[ExitManager] F-01: entry slippage %.8f on opener %llu (%s) "
            "reached the designed stop distance %.8f - bracket refused, "
            "flattening %.8f @ %.8f\n",
            entry_slippage,
            static_cast<unsigned long long>(opener_order_id),
            sample.symbol.c_str(), designed_stop_distance, qty, entry_price);
    }
    request_flatten(opener_order_id, sample, qty, entry_price,
                    designed_stop_distance, entry_slippage, trigger_ts);
}

std::vector<ExitManager::flatten_request> ExitManager::take_flatten_requests()
{
    std::vector<flatten_request> out;
    out.swap(flatten_requests_);
    return out;
}

std::vector<ExitManager::flatten_request>
ExitManager::take_flatten_requests_for(std::string_view symbol)
{
    std::vector<flatten_request> out;
    for (auto it = flatten_requests_.begin(); it != flatten_requests_.end(); )
    {
        if (it->symbol == symbol)
        {
            out.push_back(std::move(*it));
            it = flatten_requests_.erase(it);
        }
        else
        {
            ++it;
        }
    }
    return out;
}

std::uint64_t ExitManager::remember_fired_protection(
    std::uint64_t opener_order_id, double qty, order_exit_reason reason,
    order_side close_side, std::string_view symbol, std::string_view strategy_name)
{
    if (opener_order_id == 0 || !(qty > 0.0)) return 0;
    const std::uint64_t ticket = next_protective_ticket_++;
    fired_protections_.emplace(ticket, fired_protection{
        ticket, 0, opener_order_id, qty, qty, reason, close_side,
        std::string(symbol), std::string(strategy_name)});
    return ticket;
}

bool ExitManager::bind_protective_exit(std::uint64_t ticket,
                                       std::uint64_t order_id)
{
    if (ticket == 0 || order_id == 0) return false;
    auto it = fired_protections_.find(ticket);
    if (it == fired_protections_.end() || it->second.order_id != 0)
        return false;
    it->second.order_id = order_id;
    protective_ticket_by_order_.emplace(order_id, ticket);
    return true;
}

std::optional<ExitManager::protective_exit_view>
ExitManager::protective_exit_for_order(std::uint64_t order_id) const
{
    const auto by_order = protective_ticket_by_order_.find(order_id);
    if (by_order == protective_ticket_by_order_.end()) return std::nullopt;
    const auto it = fired_protections_.find(by_order->second);
    if (it == fired_protections_.end()) return std::nullopt;
    const auto& p = it->second;
    return protective_exit_view{
        p.ticket, p.order_id, p.opener_order_id, p.requested_qty,
        std::max(0.0, p.requested_qty - p.remaining_qty),
        p.remaining_qty, p.reason, p.symbol, p.strategy_name};
}

bool ExitManager::protective_fill_is_admissible(std::uint64_t order_id,
                                                 double qty) const
{
    if (completed_protective_close_orders_.count(order_id) != 0)
        return false;
    const auto p = protective_exit_for_order(order_id);
    return !p || (qty > 0.0 && qty <= p->remaining_qty + 1e-12);
}

bool ExitManager::is_known_protective_close(std::uint64_t order_id) const
{
    return protective_ticket_by_order_.count(order_id) != 0
        || completed_protective_close_orders_.count(order_id) != 0;
}

double ExitManager::reserved_close_qty(const std::string& symbol,
                                       order_side side) const
{
    double total = 0.0;
    for (const auto& [_, p] : fired_protections_)
        if (p.symbol == symbol && p.close_side == side)
            total += p.remaining_qty;
    return total;
}

void ExitManager::record_orphan_opener_fill(std::uint64_t opener_order_id,
                                            const fill_event& f, double qty)
{
    if (!(qty > 0.0)) return;

    auto& o = orphan_opener_fills_[opener_order_id];
    const double prev = o.qty;
    if (prev + qty > 0.0)
        o.vwap = (o.vwap * prev + f.get_fill_price() * qty) / (prev + qty);
    else
        o.vwap = f.get_fill_price();
    o.qty  += qty;
    o.ts    = f.get_timestamp();
    o.epoch = eval_epoch_;
    ++counters_.orphan_fills_recorded;
    sweep_orphan_opener_fills();
}

void ExitManager::sweep_orphan_opener_fills()
{
    // Expire anything older than one full window. register_pending for a
    // synchronous fill runs in the same window as the fill, so a one-window
    // grace is generous; beyond it the fill belonged to an opener that never
    // declared a bracket and must not be able to arm a later intent.
    for (auto it = orphan_opener_fills_.begin();
         it != orphan_opener_fills_.end(); )
    {
        if (it->second.epoch + 1 < eval_epoch_)
        {
            it = orphan_opener_fills_.erase(it);
            ++counters_.orphan_fills_evicted;
        }
        else
        {
            ++it;
        }
    }

    // Hard bound for a pathological single window (many bracket-less
    // openers between two evaluations). Evict the oldest first.
    while (orphan_opener_fills_.size() > kMaxOrphanOpenerFills)
    {
        auto oldest = orphan_opener_fills_.begin();
        for (auto it = orphan_opener_fills_.begin();
             it != orphan_opener_fills_.end(); ++it)
        {
            if (it->second.epoch < oldest->second.epoch)
                oldest = it;
        }
        orphan_opener_fills_.erase(oldest);
        ++counters_.orphan_fills_evicted;
    }
}

void ExitManager::enforce_pending_bound()
{
    if (pending_.size() <= kMaxPendingIntents) return;

    // F-06: pending_ is erased only by an opener fill or an explicit
    // cancel(). An order that dies any other way (rejected after
    // registration - see F-02) leaves its intent here forever, and on
    // order-id reuse could phantom-arm on an unrelated opener. The bound
    // makes that leak terminal and loud instead of unbounded and silent.
    while (pending_.size() > kMaxPendingIntents)
    {
        auto it = pending_.begin();
        const std::uint64_t opener = it->first;
        const std::string sname = it->second.strategy_name;
        const std::string sym   = it->second.symbol;
        pending_.erase(it);
        ++counters_.pending_evicted;
        if (pending_.count(opener) == 0 && armed_.count(opener) == 0)
            untrack_opener(opener, sname, sym);
        std::fprintf(stderr,
            "[ExitManager] F-06: pending exit-intent capacity %zu exceeded; "
            "evicted intent for opener %llu (%s). An order died without a "
            "terminal notification - see F-02.\n",
            kMaxPendingIntents,
            static_cast<unsigned long long>(opener), sym.c_str());
    }
}


void ExitManager::on_fill(const fill_event& f)
{
    // Legacy path delegates; prefer callers to use the opener-aware overload
    // (pass true opener so closers correctly cancel the matching armed intent
    // instead of treating the closer id as a new opener).
    uint64_t opener = (f.get_opener_order_id() != 0) ? f.get_opener_order_id() : f.get_order_id();
    on_fill(f, opener);
}

void ExitManager::on_fill(const fill_event& f, std::uint64_t opener_order_id)
{
    on_fill(f, opener_order_id, f.get_filled_quantity());
}

void ExitManager::on_fill(const fill_event& f, std::uint64_t opener_order_id,
                          double accounted_qty)
{
    // A split is strictly internal bracket accounting for a single physical
    // fill. Refuse an empty/non-finite component rather than creating a
    // zero-sized bracket or mutating a reservation.
    if (!(accounted_qty > 0.0) || !std::isfinite(accounted_qty))
        return;

    // Closer fill: fills from our own partial-exit orders were already
    // reserved when triggered; unrelated manual closes still clear brackets.
    if (opener_order_id != 0 && opener_order_id != f.get_order_id())
    {
        double unaccounted_qty = accounted_qty;
        bool protective_close_still_pending = false;
        // A ticketed protective close is reconciled by its own order id.
        // The aggregate reservation remains for compatibility with older
        // attributed/manual close paths, but must not be the only source of
        // truth: two close orders for one opener otherwise become
        // indistinguishable and can cross through flat.
        if (const auto by_order = protective_ticket_by_order_.find(f.get_order_id());
            by_order != protective_ticket_by_order_.end())
        {
            auto p = fired_protections_.find(by_order->second);
            if (p != fired_protections_.end())
            {
                const double matched = std::min(unaccounted_qty, p->second.remaining_qty);
                p->second.remaining_qty -= matched;
                protective_close_still_pending = p->second.remaining_qty > 1e-12;
                if (p->second.remaining_qty <= 1e-12)
                {
                    completed_protective_close_orders_.insert(f.get_order_id());
                    fired_protections_.erase(p);
                    protective_ticket_by_order_.erase(by_order);
                }
            }
        }
        auto inflight = opener_close_in_flight_qty_.find(opener_order_id);
        if (inflight != opener_close_in_flight_qty_.end())
        {
            const double matched = std::min(unaccounted_qty, inflight->second);
            inflight->second -= matched;
            unaccounted_qty -= matched;
            if (inflight->second <= 1e-12)
                opener_close_in_flight_qty_.erase(inflight);
        }

        // The venue still owns an unfilled portion of this exact protective
        // order. Keep the opener/ticket state intact until it fills or
        // reaches a terminal status; cancelling here would make a later
        // terminal reject indistinguishable from a healthy flat lot.
        if (protective_close_still_pending)
            return;

        if (unaccounted_qty > 1e-12)
        {
            auto rem = opener_remaining_qty_.find(opener_order_id);
            if (rem != opener_remaining_qty_.end())
            {
                rem->second -= unaccounted_qty;
                if (rem->second <= 1e-12)
                {
                    cancel(opener_order_id);
                    return;
                }
                // Scale down remaining armed intent quantities proportionally
                auto arange = armed_.equal_range(opener_order_id);
                for (auto it = arange.first; it != arange.second; ++it)
                {
                    it->second.intent.qty = std::max(0.0, it->second.intent.qty - unaccounted_qty);
                }
                return;
            }
            cancel(opener_order_id);
            return;
        }

        auto rem = opener_remaining_qty_.find(opener_order_id);
        if (rem == opener_remaining_qty_.end() || rem->second <= 1e-12)
            cancel(opener_order_id);
        return;
    }

    // Opener fill: promote pending -> armed.
    const std::uint64_t opener = f.get_order_id();
    auto range = pending_.equal_range(opener);
    if (range.first == range.second)
    {
        auto arange = armed_.equal_range(opener);
        if (arange.first == arange.second)
        {
            // F-01(a): this opener's bracket was refused. A later partial
            // fill is more exposure on the same void premise, so extend the
            // flatten instead of leaving the residual silently naked.
            if (!disarmed_openers_.empty() && disarmed_openers_.count(opener) != 0)

            {
                exit_intent sample;
                sample.symbol     = f.get_symbol();
                sample.close_side = (f.get_side() == order_side::buy)
                    ? order_side::sell : order_side::buy;
                request_flatten(opener, sample, accounted_qty,
                                f.get_fill_price(), 0.0, 0.0,
                                f.get_timestamp());
                return;
            }
            // F-03: neither pending nor armed. Either this opener carries no
            // bracket at all, or its intent has not been registered yet
            // (synchronous fill inside route(); register_pending runs later
            // in finalize_route). Record it so register_pending can arm from
            // it rather than waiting for a promotion that already happened.
            record_orphan_opener_fill(opener, f, accounted_qty);
            return;
        }

        // Subsequent partial fill of an opener whose bracket is already armed
        // (the book can emit one fill per walked level). Grow the armed qty so
        // the exit covers the whole position — anchoring it to the first
        // partial only would leave the residual silently unprotected — and
        // roll the entry to the VWAP across opener fills. (A venue-side
        // bracket placed on the first fill keeps its original qty; engine-side
        // eval covers the rest.)
        bool breached = false;
        double breach_designed = 0.0;
        double breach_slip = 0.0;
        double breach_entry = 0.0;
        exit_intent breach_sample;
        for (auto it = arange.first; it != arange.second; ++it)
        {
            auto& ai = it->second;
            double frac = ai.intent.qty_fraction;
            if (frac <= 0.0) frac = 1.0;
            if (frac > 1.0)  frac = 1.0;
            const double add = accounted_qty * frac;
            if (add <= 0.0) continue;
            const double prev = ai.intent.qty;
            double new_entry = ai.entry_price;
            if (prev + add > 0.0)
                new_entry = (ai.entry_price * prev +
                             f.get_fill_price() * add) / (prev + add);

            // F-09a: the designed risk distance is measured from the entry.
            // Rolling entry_price to the VWAP while leaving SL/TP anchored to
            // the first partial silently rewrites the trade's risk — a four
            // level walk turned a designed 5.14 distance into 33.45. Re-shift
            // the levels by the same delta, under the same F-01(a) guard so a
            // wide walk cannot re-introduce the mis-arm.
            const double designed = ai.designed_stop_distance();
            if (!shift_entry_relative_levels(ai.intent, designed, new_entry))
            {
                breached        = true;
                breach_designed = designed;

                breach_slip     = std::abs(new_entry - ai.entry_price);
                breach_entry    = new_entry;
                breach_sample   = ai.intent;
                break;
            }
            ai.entry_price = new_entry;
            ai.intent.qty  = prev + add;
        }

        if (breached)
        {
            auto rem = opener_remaining_qty_.find(opener);
            const double outstanding =
                (rem != opener_remaining_qty_.end() ? rem->second : 0.0)
                + accounted_qty;
            opener_remaining_qty_.erase(opener);
            disarm_and_flatten(opener, breach_sample, outstanding,
                               breach_entry, breach_designed, breach_slip,
                               f.get_timestamp());
            return;
        }

        // Keep remaining-qty in lockstep with armed size so consume_opener_qty
        // can cover the full position across multi-level opener fills.
        opener_remaining_qty_[opener] += accounted_qty;
        return;
    }

    // Move the pending intents out before arming: arm_one may disarm the
    // whole opener (F-01(a)), and iterating pending_ while it is erased is
    // undefined. One intent is overwhelmingly the common case (a single
    // SL/TP bracket) and is handled without touching the heap.
    auto next = range.first;
    ++next;
    if (next == range.second)
    {
        exit_intent only = std::move(range.first->second);
        pending_.erase(range.first);
        opener_remaining_qty_[opener] += accounted_qty;
        if (!arm_one(opener, std::move(only), f.get_fill_price(),
                     accounted_qty, f.get_timestamp(),
                     /*deferred=*/false))
            opener_remaining_qty_.erase(opener);   // premise void; flatten queued
        return;
    }

    std::vector<exit_intent> to_arm;
    to_arm.reserve(static_cast<std::size_t>(std::distance(range.first, range.second)));
    for (auto it = range.first; it != range.second; ++it)
        to_arm.push_back(std::move(it->second));
    pending_.erase(range.first, range.second);

    opener_remaining_qty_[opener] += accounted_qty;
    for (auto& intent : to_arm)
    {
        if (!arm_one(opener, std::move(intent), f.get_fill_price(),
                     accounted_qty, f.get_timestamp(),
                     /*deferred=*/false))
        {
            // Premise void: the opener is disarmed and queued for flatten.
            opener_remaining_qty_.erase(opener);
            return;
        }
    }
}



std::vector<order_event> ExitManager::on_price(
    const std::string& symbol, double px,
    std::chrono::system_clock::time_point ts)
{
    if (!engine_drives_windows_) { ++eval_epoch_; armed_this_window_.clear(); }

    std::vector<order_event> closes;
    if (armed_.empty() || !(px > 0.0))

        return closes;

    for (auto it = armed_.begin(); it != armed_.end(); )
    {
        auto& ai = it->second;
        if (ai.intent.symbol != symbol) { ++it; continue; }

        const bool is_long  = (ai.intent.close_side == order_side::sell);
        const bool is_short = (ai.intent.close_side == order_side::buy);



        // Update MFE + trail before evaluating so a single tick can raise
        // the SL and fire on the same pass if it also crosses it.
        if (is_long && px > ai.best_price)  ai.best_price = px;
        if (is_short && px < ai.best_price) ai.best_price = px;

        if (ai.intent.trailing_pct)
        {
            double pct = *ai.intent.trailing_pct;
            if (is_long)
            {
                double trailed = ai.best_price * (1.0 - pct);
                double cur = ai.intent.stop_loss.value_or(0.0);
                if (trailed > cur) ai.intent.stop_loss = trailed;
            }
            else
            {
                double trailed = ai.best_price * (1.0 + pct);
                double cur = ai.intent.stop_loss.value_or(std::numeric_limits<double>::infinity());
                if (trailed < cur) ai.intent.stop_loss = trailed;
            }
        }

        const bool sl_hit =
            ai.intent.stop_loss &&
            ((is_long  && px <= *ai.intent.stop_loss) ||
             (is_short && px >= *ai.intent.stop_loss));

        const bool tp_hit =
            ai.intent.take_profit &&
            ((is_long  && px >= *ai.intent.take_profit) ||
             (is_short && px <= *ai.intent.take_profit));

        const bool time_hit =
            ai.intent.deadline && ts >= *ai.intent.deadline;

        if (!sl_hit && !tp_hit && !time_hit) { ++it; continue; }

        const double close_qty = consume_opener_qty(it->first, ai.intent.qty);
        if (!(close_qty > 0.0))
        {
            const std::string sname = ai.intent.strategy_name;
            const std::string sym   = ai.intent.symbol;
            const std::uint64_t opener = it->first;
            it = armed_.erase(it);
            if (armed_.count(opener) == 0)
            {
                untrack_opener(opener, sname, sym);
                release_venue_bracket(opener);
            }
            continue;
        }

        order_event close(ts, ai.intent.symbol,
                          order_type::market, ai.intent.close_side,
                          close_qty, px);
        close.set_opener_order_id(it->first);
        close.set_strategy_name(ai.intent.strategy_name);
        const order_exit_reason reason = sl_hit
            ? (ai.intent.trailing_pct ? order_exit_reason::trailing_stop
                                      : order_exit_reason::stop_loss)
            : (tp_hit ? order_exit_reason::take_profit
                      : order_exit_reason::time_stop);
        close.set_exit_reason(reason);
        close.set_protective_exit_ticket(remember_fired_protection(
            it->first, close_qty, reason, ai.intent.close_side,
            ai.intent.symbol, ai.intent.strategy_name));
        closes.push_back(std::move(close));
        opener_close_in_flight_qty_[it->first] += close_qty;

        // Capture key fields before erase invalidates `ai`/`it`.
        const std::string sname = ai.intent.strategy_name;
        const std::string sym   = ai.intent.symbol;
        const std::uint64_t opener = it->first;
        it = armed_.erase(it);
        release_venue_bracket(opener);
        // The legacy strategy/openers view counts only pending/armed
        // protection.  The ticket map retains the safety-critical state.
        untrack_opener(opener, sname, sym);
    }

    return closes;
}

std::vector<order_event> ExitManager::on_bar(
    const std::string& symbol, double open, double low, double high,
    double close,
    std::chrono::system_clock::time_point ts)
{
    if (!engine_drives_windows_) { ++eval_epoch_; armed_this_window_.clear(); }

    // Almost always empty: nothing armed inside this observation. Hoisted so
    // the per-intent fire branches below cost one compare, not a scan.
    const bool any_armed_this_window = !armed_this_window_.empty();

    std::vector<order_event> closes;
    if (armed_.empty()) return closes;

    if (!(low > 0.0) || !(high > 0.0) || !(close > 0.0)) return closes;
    if (low > high) std::swap(low, high);
    if (!(open > 0.0)) open = close;

    for (auto it = armed_.begin(); it != armed_.end(); )
    {
        auto& ai = it->second;
        if (ai.intent.symbol != symbol) { ++it; continue; }

        const bool is_long  = (ai.intent.close_side == order_side::sell);

        // F-01(b): a bar can only gap through a level that existed when it
        // opened. For an intent armed by a fill *inside* this observation
        // (a bar-delayed order released at this bar's open, a stop
        // conversion, an MM/paper fill) the open is a price printed before
        // the bracket existed, so anchoring the fill there is a causality
        // violation — it exits strictly earlier than the extreme that
        // triggered the exit. Anchor at the level instead, clamped into this
        // bar's own range so the fill is always a price the bar actually
        // traded at.
        //
        // Protection itself is NOT deferred. A bar that wicks through the
        // stop after the entry filled at its open must still stop out;
        // skipping the whole bar would leave the lot unprotected for exactly
        // the bar the position was opened in.
        //
        // Read lazily, inside the branches that actually anchor a fill: the
        // overwhelmingly common outcome is "nothing fired", and that path
        // must not touch a field it does not need.



        const double favorable = is_long ? high : low;
        const double adverse   = is_long ? low  : high;

        double fire_px = 0.0;
        bool fired = false;
        order_exit_reason fire_reason = order_exit_reason::none;

        // SL takes precedence when both extremes cross in one bar — we can't
        // know intra-bar order so the worst case wins. The trailing stop is
        // tested at its level from PREVIOUS bars: raising it with this bar's
        // favorable extreme and then testing this bar's adverse extreme
        // would assume the favorable extreme printed first.
        if (ai.intent.stop_loss &&
            ((is_long  && adverse <= *ai.intent.stop_loss) ||
             (!is_long && adverse >= *ai.intent.stop_loss)))
        {
            // Anchored fill price: the stop level, or the open when the bar
            // gapped through it. Never the bar extreme — that overstates
            // slippage for an ordinary intra-bar trigger.
            const double sl = *ai.intent.stop_loss;
            fire_px = (any_armed_this_window && armed_in_this_window(it->first))
                ? std::clamp(sl, low, high)
                : ((is_long ? (open <= sl) : (open >= sl)) ? open : sl);
            fired = true;
            fire_reason = ai.intent.trailing_pct
                ? order_exit_reason::trailing_stop : order_exit_reason::stop_loss;



        }
        else if (ai.intent.take_profit &&
                 ((is_long  && favorable >= *ai.intent.take_profit) ||
                  (!is_long && favorable <= *ai.intent.take_profit)))
        {
            // A TP is a resting limit in reality: it fills at the TP level,
            // or better at the open when the bar gapped through it.
            const double tp = *ai.intent.take_profit;
            fire_px = (any_armed_this_window && armed_in_this_window(it->first))
                ? std::clamp(tp, low, high)
                : ((is_long ? (open >= tp) : (open <= tp)) ? open : tp);
            fired = true;
            fire_reason = order_exit_reason::take_profit;



        }
        else if (ai.intent.deadline && ts >= *ai.intent.deadline)
        {
            fire_px = close;
            fired = true;
            fire_reason = order_exit_reason::time_stop;
        }

        if (!fired)
        {
            // Survived this bar: now roll MFE + trail forward for the next.
            if (is_long  && favorable > ai.best_price) ai.best_price = favorable;
            if (!is_long && favorable < ai.best_price) ai.best_price = favorable;

            if (ai.intent.trailing_pct)
            {
                double pct = *ai.intent.trailing_pct;
                if (is_long)
                {
                    double trailed = ai.best_price * (1.0 - pct);
                    double cur = ai.intent.stop_loss.value_or(0.0);
                    if (trailed > cur) ai.intent.stop_loss = trailed;
                }
                else
                {
                    double trailed = ai.best_price * (1.0 + pct);
                    double cur = ai.intent.stop_loss.value_or(std::numeric_limits<double>::infinity());
                    if (trailed < cur) ai.intent.stop_loss = trailed;
                }
            }
            ++it; continue;
        }

        const double close_qty = consume_opener_qty(it->first, ai.intent.qty);
        if (!(close_qty > 0.0))
        {
            const std::string sname = ai.intent.strategy_name;
            const std::string sym   = ai.intent.symbol;
            const std::uint64_t opener = it->first;
            it = armed_.erase(it);
            if (armed_.count(opener) == 0)
            {
                untrack_opener(opener, sname, sym);
                release_venue_bracket(opener);
            }
            continue;
        }

        order_event c(ts, ai.intent.symbol, order_type::market,
                      ai.intent.close_side, close_qty, fire_px);
        c.set_opener_order_id(it->first);
        c.set_strategy_name(ai.intent.strategy_name);
        const order_exit_reason reason = fire_reason;
        c.set_exit_reason(reason);
        c.set_protective_exit_ticket(remember_fired_protection(
            it->first, close_qty, reason, ai.intent.close_side,
            ai.intent.symbol, ai.intent.strategy_name));
        closes.push_back(std::move(c));
        opener_close_in_flight_qty_[it->first] += close_qty;

        const std::string sname = ai.intent.strategy_name;
        const std::string sym   = ai.intent.symbol;
        const std::uint64_t opener = it->first;
        it = armed_.erase(it);
        release_venue_bracket(opener);
        // Do not erase lot state at fire time; only remove the legacy armed
        // index. The ticket retains the terminal-protection state.
        untrack_opener(opener, sname, sym);
    }

    return closes;
}

std::size_t ExitManager::openers_for(const std::string& strategy_name,
                                     const std::string& symbol) const
{
    strategy_symbol_key ssk{strategy_name, symbol};
    return strategy_symbol_to_openers_.count(ssk);
}

void ExitManager::cancel(std::uint64_t opener_order_id)
{
    // Capture side-index entries before the primary containers drop the key.
    // The same two lookups answer "did this opener have an intent at all",
    // which the lifecycle counter needs — cancel() runs once per closed lot,
    // so it does not repeat them.
    std::string sname, sym;
    bool had_intent = false;
    auto ap = pending_.find(opener_order_id);
    if (ap != pending_.end())
    {
        sname = ap->second.strategy_name;
        sym = ap->second.symbol;
        had_intent = true;
    }
    else
    {
        auto aa = armed_.find(opener_order_id);
        if (aa != armed_.end())
        {
            sname = aa->second.intent.strategy_name;
            sym = aa->second.intent.symbol;
            had_intent = true;
        }
    }

    pending_.erase(opener_order_id);
    armed_.erase(opener_order_id);
    opener_remaining_qty_.erase(opener_order_id);
    opener_close_in_flight_qty_.erase(opener_order_id);
    for (auto it = fired_protections_.begin(); it != fired_protections_.end(); )
    {
        if (it->second.opener_order_id == opener_order_id)
        {
            if (it->second.order_id != 0)
                protective_ticket_by_order_.erase(it->second.order_id);
            it = fired_protections_.erase(it);
        }
        else
        {
            ++it;
        }
    }
    // Both are empty in a healthy run; skip the hash entirely.
    if (!orphan_opener_fills_.empty()) orphan_opener_fills_.erase(opener_order_id);
    if (!disarmed_openers_.empty())    disarmed_openers_.erase(opener_order_id);
    if (had_intent) ++counters_.cancelled;

    if (!sname.empty() || !sym.empty())
        untrack_opener(opener_order_id, sname, sym);
    release_venue_bracket(opener_order_id);
}


void ExitManager::release_close_reservation(std::uint64_t opener_order_id,
                                            double qty)
{
    if (opener_order_id == 0 || !(qty > 0.0)) return;

    auto inflight = opener_close_in_flight_qty_.find(opener_order_id);
    if (inflight == opener_close_in_flight_qty_.end()) return;

    const double released = std::min(qty, inflight->second);
    inflight->second -= released;
    if (inflight->second <= 1e-12)
        opener_close_in_flight_qty_.erase(inflight);

    // Only give quantity back to an opener still being tracked. A closer
    // dying after its opener already went flat is not a reason to resurrect
    // remaining size on a lot that no longer exists.
    auto rem = opener_remaining_qty_.find(opener_order_id);
    if (rem != opener_remaining_qty_.end())
        rem->second += released;
    else if (armed_.count(opener_order_id) != 0)
        opener_remaining_qty_[opener_order_id] = released;
}

void ExitManager::cancel(const std::string& strategy_name, const std::string& symbol)

{
    strategy_symbol_key ssk{strategy_name, symbol};
    auto range = strategy_symbol_to_openers_.equal_range(ssk);
    std::vector<std::uint64_t> openers;
    for (auto it = range.first; it != range.second; ++it)
        openers.push_back(it->second);
    strategy_symbol_to_openers_.erase(ssk);

    for (auto id : openers)
    {
        if (pending_.count(id) != 0 || armed_.count(id) != 0)
            ++counters_.cancelled;
        pending_.erase(id);
        armed_.erase(id);
        opener_remaining_qty_.erase(id);
        opener_close_in_flight_qty_.erase(id);
        if (!orphan_opener_fills_.empty()) orphan_opener_fills_.erase(id);
        if (!disarmed_openers_.empty())    disarmed_openers_.erase(id);
        release_venue_bracket(id);
    }

}


std::uint64_t ExitManager::opener_for_exchange_order(const std::string& exchange_order_id) const
{
    const auto attribution = venue_fill_attribution_for_exchange_order(exchange_order_id);
    return attribution ? attribution->opener_order_id : 0u;
}

std::string ExitManager::strategy_name_for_exchange_order(const std::string& exchange_order_id) const
{
    const auto attribution = venue_fill_attribution_for_exchange_order(exchange_order_id);
    return attribution ? attribution->strategy_name : std::string{};
}

std::optional<ExitManager::venue_fill_attribution>
ExitManager::venue_fill_attribution_for_exchange_order(
    const std::string& exchange_order_id) const
{
    std::lock_guard<std::mutex> lk(venue_mu_);
    auto it = exchange_to_leg_.find(exchange_order_id);
    if (it == exchange_to_leg_.end()) return std::nullopt;
    return venue_fill_attribution{
        it->second.opener_order_id,
        it->second.strategy_name,
        it->second.intended_price};
}

double ExitManager::intended_price_for_exchange_order(
    const std::string& exchange_order_id) const
{
    const auto attribution = venue_fill_attribution_for_exchange_order(exchange_order_id);
    return attribution ? attribution->intended_price : 0.0;
}

std::vector<ExitManager::armed_view> ExitManager::snapshot_armed() const
{
    std::vector<armed_view> out;
    out.reserve(armed_.size());

    // Snapshot venue handles separately so we can mark each row's
    // venue_managed flag without touching armed_ across the lock.
    std::unordered_map<std::uint64_t, std::string> oco_ids;
    {
        std::lock_guard<std::mutex> lk(venue_mu_);
        for (const auto& [opener, h] : handles_)
            oco_ids.emplace(opener,
                h.oco_list_id ? *h.oco_list_id : std::string{});
    }

    for (const auto& [opener, ai] : armed_)
    {
        armed_view v;
        v.opener_order_id = opener;
        v.strategy_name   = ai.intent.strategy_name;
        v.symbol          = ai.intent.symbol;
        v.close_side      = ai.intent.close_side;
        v.qty             = ai.intent.qty;
        v.entry_price     = ai.entry_price;
        v.stop_loss       = ai.intent.stop_loss;
        v.take_profit     = ai.intent.take_profit;
        v.ts_armed        = ai.ts_armed;
        auto hit = oco_ids.find(opener);
        v.venue_managed = (hit != oco_ids.end());
        if (v.venue_managed) v.venue_list_id = hit->second;
        out.push_back(std::move(v));
    }
    return out;
}

void ExitManager::rehydrate(const IBracketAdapter::recovered_bracket& rb)
{
    if (rb.opener_order_id == 0) return;
    if (rb.handles.empty())      return;
    if (!(rb.qty > 0.0) || !std::isfinite(rb.qty))
        throw std::runtime_error(
            "ExitManager: recovered venue bracket has no authoritative quantity");

    exit_intent ei;
    ei.symbol           = rb.symbol;
    ei.close_side       = rb.close_side;
    ei.qty              = rb.qty;
    ei.stop_loss        = rb.stop_loss;
    ei.take_profit      = rb.take_profit;
    ei.opener_order_id  = rb.opener_order_id;
    ei.strategy_name    = rb.strategy_name;

    armed_intent ai;
    ai.intent      = ei;
    ai.entry_price = rb.entry_price;
    ai.best_price  = rb.entry_price;
    ai.ts_armed    = std::chrono::system_clock::now();  // best-effort post-restart

    // A rehydrated venue bracket has no strategy-declared reference entry;

    // treat the recovered entry price as the anchor so designed_stop_distance()
    // reports the real risk distance.
    if (ei.stop_loss && rb.entry_price > 0.0)
        ai.intent.reference_entry = rb.entry_price;

    armed_.emplace(rb.opener_order_id, std::move(ai));

    ++counters_.armed;
    opener_remaining_qty_[rb.opener_order_id] = rb.qty;


    // Strategy-symbol side index: keep openers_for() consistent so the
    // engine's net-flat sweep doesn't accidentally bulk-cancel a
    // multi-lot strategy after restart (count >= 1 already protects it).
    if (!rb.strategy_name.empty() || !rb.symbol.empty())
        strategy_symbol_to_openers_.emplace(
            strategy_symbol_key{rb.strategy_name, rb.symbol},
            rb.opener_order_id);

    // Venue-side state: install handles + reverse map under venue_mu_
    // so the inbound user-data fill stream can stamp opener_order_id
    // immediately without waiting for the next on_fill to populate it.
    {
        std::lock_guard<std::mutex> lk(venue_mu_);
        if (rb.handles.sl_exchange_id)
            exchange_to_leg_.emplace(
                *rb.handles.sl_exchange_id,
                exchange_leg{rb.opener_order_id, rb.strategy_name,
                             rb.stop_loss.value_or(0.0)});
        if (rb.handles.tp_exchange_id)
            exchange_to_leg_.emplace(
                *rb.handles.tp_exchange_id,
                exchange_leg{rb.opener_order_id, rb.strategy_name,
                             rb.take_profit.value_or(0.0)});
        handles_.emplace(rb.opener_order_id, rb.handles);
    }
}

void ExitManager::release_venue_bracket(std::uint64_t opener_order_id)
{
    bracket_handles handles;
    exchange_leg retained_sl_leg{opener_order_id, ""};
    exchange_leg retained_tp_leg{opener_order_id, ""};
    {
        std::lock_guard<std::mutex> lk(venue_mu_);
        auto it = handles_.find(opener_order_id);
        if (it == handles_.end()) return;
        handles = std::move(it->second);
        handles_.erase(it);
        if (handles.sl_exchange_id)
        {
            const auto leg_it = exchange_to_leg_.find(*handles.sl_exchange_id);
            if (leg_it != exchange_to_leg_.end()) retained_sl_leg = leg_it->second;
        }
        if (handles.tp_exchange_id)
        {
            const auto leg_it = exchange_to_leg_.find(*handles.tp_exchange_id);
            if (leg_it != exchange_to_leg_.end()) retained_tp_leg = leg_it->second;
        }
        if (handles.sl_exchange_id) exchange_to_leg_.erase(*handles.sl_exchange_id);
        if (handles.tp_exchange_id) exchange_to_leg_.erase(*handles.tp_exchange_id);
    }
    // Adapter call OUTSIDE the mutex - adapters do REST I/O and we don't
    // want WS-thread lookups blocked behind a network round-trip.
    if (bracket_adapter_)
    {
        try
        {
            bracket_adapter_->cancel(opener_order_id, handles);
        }
        catch (...)
        {
            // Cancellation was not authoritative. Restore the handles so a
            // later shutdown/restart can retry/reconcile rather than silently
            // orphaning venue protection after a malformed success response.
            std::lock_guard<std::mutex> lk(venue_mu_);
            handles_.insert_or_assign(opener_order_id, handles);
            if (handles.sl_exchange_id)
                exchange_to_leg_.insert_or_assign(
                    *handles.sl_exchange_id, retained_sl_leg);
            if (handles.tp_exchange_id)
                exchange_to_leg_.insert_or_assign(
                    *handles.tp_exchange_id, retained_tp_leg);
            throw;
        }
    }
}

void ExitManager::untrack_opener(std::uint64_t opener_order_id,
                                 const std::string& strategy_name,
                                 const std::string& symbol)
{
    strategy_symbol_key ssk{strategy_name, symbol};
    auto range = strategy_symbol_to_openers_.equal_range(ssk);
    for (auto it = range.first; it != range.second; )
    {
        if (it->second == opener_order_id)
            it = strategy_symbol_to_openers_.erase(it);
        else
            ++it;
    }
}

double ExitManager::consume_opener_qty(std::uint64_t opener_order_id, double requested_qty)
{
    if (!(requested_qty > 0.0))
        return 0.0;

    auto it = opener_remaining_qty_.find(opener_order_id);
    if (it == opener_remaining_qty_.end())
        return requested_qty;

    const double qty = std::min(requested_qty, it->second);
    it->second -= qty;
    if (std::abs(it->second) < 1e-12)
        it->second = 0.0;
    return qty;
}

// Phase A (MC object reuse)
void ExitManager::reset()
{
    pending_.clear();
    armed_.clear();
    opener_remaining_qty_.clear();
    opener_close_in_flight_qty_.clear();
    fired_protections_.clear();
    protective_ticket_by_order_.clear();
    completed_protective_close_orders_.clear();
    next_protective_ticket_ = 1;
    strategy_symbol_to_openers_.clear();
    orphan_opener_fills_.clear();
    disarmed_openers_.clear();
    flatten_requests_.clear();
    armed_this_window_.clear();
    counters_ = lifecycle_counters{};

    eval_epoch_ = 0;
    engine_drives_windows_ = false;


    {
        std::lock_guard<std::mutex> lk(venue_mu_);
        handles_.clear();
        exchange_to_leg_.clear();
    }
    // Note: bracket_adapter_ is intentionally not cleared — it is set once at startup.
}

}
