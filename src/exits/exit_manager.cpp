#include "exits/exit_manager.h"

#include <limits>
#include <utility>

namespace truetest::exits {

void ExitManager::register_pending(exit_intent intent)
{
    if (intent.opener_order_id == 0) return;  // cannot key — drop silently
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
    // Closer fill: bracket for the original opener should no longer fire.
    if (opener_order_id != 0 && opener_order_id != f.get_order_id())
    {
        cancel(opener_order_id);
        return;
    }

    // Opener fill: promote pending → armed.
    auto range = pending_.equal_range(f.get_order_id());
    if (range.first == range.second) return;

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

        to_place.push_back(ai.intent);
        armed_.emplace(f.get_order_id(), std::move(ai));
    }
    pending_.erase(range.first, range.second);

    // Defense-in-depth: hand the venue adapter a copy so it can place
    // resting orders. Empty handles → engine-side eval is the only path
    // (already armed above; nothing else to do). For multi-intent openers
    // (TP1/TP2/SL scale-outs) we currently delegate the first one only —
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

    std::vector<std::uint64_t> erased_openers;

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

        order_event close(ts, ai.intent.symbol,
                          order_type::market, ai.intent.close_side,
                          ai.intent.qty, px);
        close.set_opener_order_id(it->first);
        close.set_strategy_name(ai.intent.strategy_name);
        closes.push_back(std::move(close));

        erased_openers.push_back(it->first);
        // Capture key fields before erase invalidates `ai`/`it`.
        const std::string sname = ai.intent.strategy_name;
        const std::string sym   = ai.intent.symbol;
        const std::uint64_t opener = it->first;
        it = armed_.erase(it);
        untrack_opener(opener, sname, sym);
        release_venue_bracket(opener);
    }

    return closes;
}

std::vector<order_event> ExitManager::on_bar(
    const std::string& symbol, double low, double high, double close,
    std::chrono::system_clock::time_point ts)
{
    std::vector<order_event> closes;
    if (armed_.empty()) return closes;
    if (!(low > 0.0) || !(high > 0.0) || !(close > 0.0)) return closes;
    if (low > high) std::swap(low, high);

    for (auto it = armed_.begin(); it != armed_.end(); )
    {
        auto& ai = it->second;
        if (ai.intent.symbol != symbol) { ++it; continue; }

        const bool is_long  = (ai.intent.close_side == order_side::sell);
        const double favorable = is_long ? high : low;
        const double adverse   = is_long ? low  : high;

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

        double fire_px = 0.0;
        bool fired = false;

        // SL takes precedence when both extremes cross in one bar — we can't
        // know intra-bar order so the worst case wins.
        if (ai.intent.stop_loss &&
            ((is_long  && adverse <= *ai.intent.stop_loss) ||
             (!is_long && adverse >= *ai.intent.stop_loss)))
        {
            fire_px = adverse;
            fired = true;
        }
        else if (ai.intent.take_profit &&
                 ((is_long  && favorable >= *ai.intent.take_profit) ||
                  (!is_long && favorable <= *ai.intent.take_profit)))
        {
            fire_px = favorable;
            fired = true;
        }
        else if (ai.intent.deadline && ts >= *ai.intent.deadline)
        {
            fire_px = close;
            fired = true;
        }

        if (!fired) { ++it; continue; }

        order_event c(ts, ai.intent.symbol, order_type::market,
                      ai.intent.close_side, ai.intent.qty, fire_px);
        c.set_opener_order_id(it->first);
        c.set_strategy_name(ai.intent.strategy_name);
        closes.push_back(std::move(c));

        const std::string sname = ai.intent.strategy_name;
        const std::string sym   = ai.intent.symbol;
        const std::uint64_t opener = it->first;
        it = armed_.erase(it);
        untrack_opener(opener, sname, sym);
        release_venue_bracket(opener);
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
    {
        std::lock_guard<std::mutex> lk(venue_mu_);
        auto it = handles_.find(opener_order_id);
        if (it == handles_.end()) return;
        handles = std::move(it->second);
        handles_.erase(it);
        if (handles.sl_exchange_id) exchange_to_leg_.erase(*handles.sl_exchange_id);
        if (handles.tp_exchange_id) exchange_to_leg_.erase(*handles.tp_exchange_id);
    }
    // Adapter call OUTSIDE the mutex — adapters do REST I/O and we don't
    // want WS-thread lookups blocked behind a network round-trip.
    if (bracket_adapter_)
        bracket_adapter_->cancel(opener_order_id, handles);
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

// Phase A (MC object reuse)
void ExitManager::reset()
{
    pending_.clear();
    armed_.clear();
    strategy_symbol_to_openers_.clear();

    {
        std::lock_guard<std::mutex> lk(venue_mu_);
        handles_.clear();
        exchange_to_leg_.clear();
    }
    // Note: bracket_adapter_ is intentionally not cleared — it is set once at startup.
}

}
