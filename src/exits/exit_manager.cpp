#include "exits/exit_manager.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace truetest::exits {

void ExitManager::register_pending(exit_intent intent)
{
    if (intent.opener_order_id == 0) return;  // cannot key - drop silently
    strategy_symbol_key ssk{intent.strategy_name, intent.symbol};
    strategy_symbol_to_openers_.emplace(ssk, intent.opener_order_id);
    pending_.emplace(intent.opener_order_id, std::move(intent));
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
    // Closer fill: fills from our own partial-exit orders were already
    // reserved when triggered; unrelated manual closes still clear brackets.
    if (opener_order_id != 0 && opener_order_id != f.get_order_id())
    {
        double unaccounted_qty = f.get_filled_quantity();
        auto inflight = opener_close_in_flight_qty_.find(opener_order_id);
        if (inflight != opener_close_in_flight_qty_.end())
        {
            const double matched = std::min(unaccounted_qty, inflight->second);
            inflight->second -= matched;
            unaccounted_qty -= matched;
            if (inflight->second <= 1e-12)
                opener_close_in_flight_qty_.erase(inflight);
        }

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
    auto range = pending_.equal_range(f.get_order_id());
    if (range.first == range.second)
    {
        // No pending intents left: this is a subsequent partial fill of an
        // opener whose bracket is already armed (the book can emit one fill
        // per walked level). Grow the armed qty so the exit covers the whole
        // position — anchoring it to the first partial only would leave the
        // residual silently unprotected — and roll the entry to the VWAP
        // across opener fills. (A venue-side bracket placed on the first
        // fill keeps its original qty; engine-side eval covers the rest.)
        auto arange = armed_.equal_range(f.get_order_id());
        for (auto it = arange.first; it != arange.second; ++it)
        {
            auto& ai = it->second;
            double frac = ai.intent.qty_fraction;
            if (frac <= 0.0) frac = 1.0;
            if (frac > 1.0)  frac = 1.0;
            const double add = f.get_filled_quantity() * frac;
            if (add <= 0.0) continue;
            const double prev = ai.intent.qty;
            if (prev + add > 0.0)
                ai.entry_price = (ai.entry_price * prev +
                                  f.get_fill_price() * add) / (prev + add);
            ai.intent.qty = prev + add;
        }
        // Keep remaining-qty in lockstep with armed size so consume_opener_qty
        // can cover the full position across multi-level opener fills.
        opener_remaining_qty_[f.get_order_id()] += f.get_filled_quantity();
        return;
    }

    // Snapshot intents before we move them so we can hand the adapter
    // a stable copy (the in-process armed copy stays as watchdog).
    std::vector<exit_intent> to_place;
    to_place.reserve(static_cast<std::size_t>(std::distance(range.first, range.second)));

    for (auto it = range.first; it != range.second; ++it)
    {
        armed_intent ai;
        ai.intent      = it->second;
        ai.entry_price = f.get_fill_price();
        ai.best_price  = f.get_fill_price();
        ai.ts_armed    = f.get_timestamp();

        double frac = ai.intent.qty_fraction;
        if (frac <= 0.0) frac = 1.0;
        if (frac > 1.0)  frac = 1.0;
        ai.intent.qty = f.get_filled_quantity() * frac;

        // Entry-relative brackets: preserve designed |entry - SL/TP|
        // distance when the opener fills away from the intended price.
        // Absolute structure levels leave reference_entry unset.
        if (ai.intent.reference_entry && *ai.intent.reference_entry > 0.0)
        {
            const double delta = f.get_fill_price() - *ai.intent.reference_entry;
            if (std::abs(delta) > 1e-15)
            {
                if (ai.intent.stop_loss)
                    *ai.intent.stop_loss += delta;
                if (ai.intent.take_profit)
                    *ai.intent.take_profit += delta;
            }
            ai.intent.reference_entry = f.get_fill_price();
        }

        to_place.push_back(ai.intent);
        armed_.emplace(f.get_order_id(), std::move(ai));
    }
    opener_remaining_qty_[f.get_order_id()] += f.get_filled_quantity();
    pending_.erase(range.first, range.second);

    // Defense-in-depth: hand the venue adapter a copy so it can place
    // resting orders. Empty handles -> engine-side eval is the only path
    // (already armed above; nothing else to do). For multi-intent openers
    // (TP1/TP2/SL scale-outs) we currently delegate the first one only -
    // adapters that don't model partial brackets natively must short-circuit.
    if (bracket_adapter_ && !to_place.empty())
    {
        const auto& first = to_place.front();
        auto handles = bracket_adapter_->place(
            f.get_order_id(), first, f.get_fill_price());
        if (!handles.empty())
        {
            std::lock_guard<std::mutex> lk(venue_mu_);
            const exchange_leg leg{f.get_order_id(), first.strategy_name};
            if (handles.sl_exchange_id)
                exchange_to_leg_.emplace(*handles.sl_exchange_id, leg);
            if (handles.tp_exchange_id)
                exchange_to_leg_.emplace(*handles.tp_exchange_id, leg);
            handles_.emplace(f.get_order_id(), std::move(handles));
        }
    }
}

std::vector<order_event> ExitManager::on_price(
    const std::string& symbol, double px,
    std::chrono::system_clock::time_point ts)
{
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
        closes.push_back(std::move(close));
        opener_close_in_flight_qty_[it->first] += close_qty;

        // Capture key fields before erase invalidates `ai`/`it`.
        const std::string sname = ai.intent.strategy_name;
        const std::string sym   = ai.intent.symbol;
        const std::uint64_t opener = it->first;
        it = armed_.erase(it);
        release_venue_bracket(opener);
        auto rem = opener_remaining_qty_.find(opener);
        const bool flat = (rem == opener_remaining_qty_.end() || rem->second <= 0.0);
        if (flat || armed_.count(opener) == 0)
        {
            opener_remaining_qty_.erase(opener);
            untrack_opener(opener, sname, sym);
            release_venue_bracket(opener);
        }
    }

    return closes;
}

std::vector<order_event> ExitManager::on_bar(
    const std::string& symbol, double open, double low, double high,
    double close,
    std::chrono::system_clock::time_point ts)
{
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
        const double favorable = is_long ? high : low;
        const double adverse   = is_long ? low  : high;

        double fire_px = 0.0;
        bool fired = false;

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
            fire_px = (is_long ? (open <= sl) : (open >= sl)) ? open : sl;
            fired = true;
        }
        else if (ai.intent.take_profit &&
                 ((is_long  && favorable >= *ai.intent.take_profit) ||
                  (!is_long && favorable <= *ai.intent.take_profit)))
        {
            // A TP is a resting limit in reality: it fills at the TP level,
            // or better at the open when the bar gapped through it.
            const double tp = *ai.intent.take_profit;
            fire_px = (is_long ? (open >= tp) : (open <= tp)) ? open : tp;
            fired = true;
        }
        else if (ai.intent.deadline && ts >= *ai.intent.deadline)
        {
            fire_px = close;
            fired = true;
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
        closes.push_back(std::move(c));
        opener_close_in_flight_qty_[it->first] += close_qty;

        const std::string sname = ai.intent.strategy_name;
        const std::string sym   = ai.intent.symbol;
        const std::uint64_t opener = it->first;
        it = armed_.erase(it);
        release_venue_bracket(opener);
        auto rem = opener_remaining_qty_.find(opener);
        const bool flat = (rem == opener_remaining_qty_.end() || rem->second <= 0.0);
        if (flat || armed_.count(opener) == 0)
        {
            opener_remaining_qty_.erase(opener);
            untrack_opener(opener, sname, sym);
            release_venue_bracket(opener);
        }
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
    std::string sname, sym;
    auto ap = pending_.find(opener_order_id);
    if (ap != pending_.end()) { sname = ap->second.strategy_name; sym = ap->second.symbol; }
    else
    {
        auto aa = armed_.find(opener_order_id);
        if (aa != armed_.end()) { sname = aa->second.intent.strategy_name; sym = aa->second.intent.symbol; }
    }

    pending_.erase(opener_order_id);
    armed_.erase(opener_order_id);
    opener_remaining_qty_.erase(opener_order_id);
    opener_close_in_flight_qty_.erase(opener_order_id);
    if (!sname.empty() || !sym.empty())
        untrack_opener(opener_order_id, sname, sym);
    release_venue_bracket(opener_order_id);
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
        pending_.erase(id);
        armed_.erase(id);
        opener_remaining_qty_.erase(id);
        opener_close_in_flight_qty_.erase(id);
        release_venue_bracket(id);
    }
}

std::uint64_t ExitManager::opener_for_exchange_order(const std::string& exchange_order_id) const
{
    std::lock_guard<std::mutex> lk(venue_mu_);
    auto it = exchange_to_leg_.find(exchange_order_id);
    return it == exchange_to_leg_.end() ? 0u : it->second.opener_order_id;
}

std::string ExitManager::strategy_name_for_exchange_order(const std::string& exchange_order_id) const
{
    std::lock_guard<std::mutex> lk(venue_mu_);
    auto it = exchange_to_leg_.find(exchange_order_id);
    return it == exchange_to_leg_.end() ? std::string{} : it->second.strategy_name;
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

    armed_.emplace(rb.opener_order_id, std::move(ai));
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
        const exchange_leg leg{rb.opener_order_id, rb.strategy_name};
        if (rb.handles.sl_exchange_id)
            exchange_to_leg_.emplace(*rb.handles.sl_exchange_id, leg);
        if (rb.handles.tp_exchange_id)
            exchange_to_leg_.emplace(*rb.handles.tp_exchange_id, leg);
        handles_.emplace(rb.opener_order_id, rb.handles);
    }
}

void ExitManager::release_venue_bracket(std::uint64_t opener_order_id)
{
    bracket_handles handles;
    exchange_leg retained_leg{opener_order_id, ""};
    {
        std::lock_guard<std::mutex> lk(venue_mu_);
        auto it = handles_.find(opener_order_id);
        if (it == handles_.end()) return;
        handles = std::move(it->second);
        handles_.erase(it);
        if (handles.sl_exchange_id)
        {
            const auto leg_it = exchange_to_leg_.find(*handles.sl_exchange_id);
            if (leg_it != exchange_to_leg_.end()) retained_leg = leg_it->second;
        }
        else if (handles.tp_exchange_id)
        {
            const auto leg_it = exchange_to_leg_.find(*handles.tp_exchange_id);
            if (leg_it != exchange_to_leg_.end()) retained_leg = leg_it->second;
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
                    *handles.sl_exchange_id, retained_leg);
            if (handles.tp_exchange_id)
                exchange_to_leg_.insert_or_assign(
                    *handles.tp_exchange_id, retained_leg);
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
    strategy_symbol_to_openers_.clear();

    {
        std::lock_guard<std::mutex> lk(venue_mu_);
        handles_.clear();
        exchange_to_leg_.clear();
    }
    // Note: bracket_adapter_ is intentionally not cleared — it is set once at startup.
}

}
