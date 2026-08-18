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
            cancel(opener_order_id);
            return;
        }

        auto rem = opener_remaining_qty_.find(opener_order_id);
        if (rem == opener_remaining_qty_.end() || rem->second <= 0.0)
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
            bool installed = false;
            try
            {
                installed = install_native_venue_state_locked(
                    f.get_order_id(), first.strategy_name, first.symbol,
                    first.close_side, first.qty, handles);
            }
            catch (...)
            {
                native_reconciliation_required_ = true;
                throw;
            }
            if (!installed)
            {
                native_reconciliation_required_ = true;
                throw std::runtime_error(
                    "ExitManager: native bracket handles failed immutable identity registration");
            }
            try
            {
                handles_.emplace(f.get_order_id(), std::move(handles));
            }
            catch (...)
            {
                native_reconciliation_required_ = true;
                throw;
            }
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
    return it == exchange_to_leg_.end()
        ? std::string{}
        : std::string{it->second.strategy_name.view()};
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

bool ExitManager::native_update_fingerprint::assign(
    const native_bracket_update& update) noexcept
{
    lifecycle = update.lifecycle;
    side = update.side;
    event_time_ms = update.event_time_ms;
    last_fill_qty = update.last_fill_qty;
    last_fill_price = update.last_fill_price;
    cumulative_qty = update.cumulative_qty;
    commission = update.commission;
    cumulative_reported = update.cumulative_reported;
    lifecycle_only = update.lifecycle_only;
    return exchange_order_id.assign(update.exchange_order_id)
        && client_order_id.assign(update.client_order_id)
        && execution_id.assign(update.execution_id)
        && symbol.assign(update.symbol)
        && group_id.assign(update.group_id)
        && commission_asset.assign(update.commission_asset)
        && error.assign(update.error);
}

bool ExitManager::native_update_fingerprint::equals(
    const native_bracket_update& update) const noexcept
{
    return lifecycle == update.lifecycle
        && exchange_order_id.equals(update.exchange_order_id)
        && client_order_id.equals(update.client_order_id)
        && execution_id.equals(update.execution_id)
        && symbol.equals(update.symbol)
        && group_id.equals(update.group_id)
        && commission_asset.equals(update.commission_asset)
        && error.equals(update.error)
        && side == update.side
        && event_time_ms == update.event_time_ms
        && same_number(last_fill_qty, update.last_fill_qty)
        && same_number(last_fill_price, update.last_fill_price)
        && same_number(cumulative_qty, update.cumulative_qty)
        && same_number(commission, update.commission)
        && cumulative_reported == update.cumulative_reported
        && lifecycle_only == update.lifecycle_only;
}

bool ExitManager::native_group_fingerprint::assign(
    const native_bracket_group_update& update) noexcept
{
    status = update.status;
    event_time_ms = update.event_time_ms;
    return group_id.assign(update.group_id) && symbol.assign(update.symbol);
}

bool ExitManager::native_group_fingerprint::equals(
    const native_bracket_group_update& update) const noexcept
{
    return status == update.status
        && group_id.equals(update.group_id)
        && symbol.equals(update.symbol)
        && event_time_ms == update.event_time_ms;
}

bool ExitManager::same_number(double left, double right) noexcept
{
    // Parser output is canonical decimal-to-double conversion.  Native
    // execution replay must be byte-for-byte economic truth, not epsilon-close
    // approximation that could hide a changed fee or quantity.
    return left == right;
}

bool ExitManager::ascii_case_equal(std::string_view left,
                                   std::string_view right) noexcept
{
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i)
    {
        const auto fold = [](unsigned char c) noexcept {
            return (c >= 'A' && c <= 'Z')
                ? static_cast<unsigned char>(c - 'A' + 'a') : c;
        };
        if (fold(static_cast<unsigned char>(left[i]))
            != fold(static_cast<unsigned char>(right[i])))
            return false;
    }
    return true;
}

bool ExitManager::is_valid_native_side(order_side side) noexcept
{
    return side == order_side::buy || side == order_side::sell;
}

bool ExitManager::is_native_leg_terminal(native_leg_lifecycle lifecycle) noexcept
{
    return lifecycle == native_leg_lifecycle::terminal_filled
        || lifecycle == native_leg_lifecycle::terminal_nonfill;
}

bool ExitManager::is_native_leg_unresolved(native_leg_lifecycle lifecycle) noexcept
{
    return lifecycle == native_leg_lifecycle::active
        || lifecycle == native_leg_lifecycle::cancel_requested
        || lifecycle == native_leg_lifecycle::expected_sibling_terminal
        || lifecycle == native_leg_lifecycle::reconciliation_required;
}

bool ExitManager::valid_native_update_shape(
    const native_bracket_update& update) noexcept
{
    if (update.exchange_order_id.empty()
        || update.exchange_order_id.size() > native_id_capacity
        || update.client_order_id.size() > native_id_capacity
        || update.execution_id.size() > native_id_capacity
        || update.symbol.empty() || update.symbol.size() > native_symbol_capacity
        || update.group_id.size() > native_id_capacity
        || update.commission_asset.size() > 15
        || update.error.size() > native_error_capacity
        || update.event_time_ms <= 0 || !is_valid_native_side(update.side)
        || !std::isfinite(update.cumulative_qty)
        || !std::isfinite(update.commission))
        return false;

    switch (update.lifecycle)
    {
    case native_bracket_lifecycle::ack:
    case native_bracket_lifecycle::partial_fill:
    case native_bracket_lifecycle::full_fill:
    case native_bracket_lifecycle::canceled:
    case native_bracket_lifecycle::rejected:
    case native_bracket_lifecycle::expired:
        break;
    default:
        return false;
    }

    if (update.is_economic_fill())
    {
        return update.source_sequence != 0
            && !update.execution_id.empty()
            && update.cumulative_reported
            && update.last_fill_qty > 0.0 && std::isfinite(update.last_fill_qty)
            && update.last_fill_price > 0.0 && std::isfinite(update.last_fill_price)
            && update.cumulative_qty >= update.last_fill_qty;
    }

    // A lifecycle-only full is a terminal corroboration of already-accounted
    // economics.  It must not smuggle an execution increment through a status
    // channel, and it must explicitly report the cumulative total it proves.
    if (update.lifecycle == native_bracket_lifecycle::full_fill)
    {
        return update.lifecycle_only && update.execution_id.empty()
            && update.cumulative_reported
            && update.last_fill_qty == 0.0 && update.last_fill_price == 0.0
            && update.commission == 0.0;
    }

    if (update.lifecycle_only || update.last_fill_qty != 0.0
        || update.last_fill_price != 0.0 || update.commission != 0.0)
        return false;
    return update.cumulative_reported
        ? update.cumulative_qty >= 0.0
        : update.cumulative_qty == 0.0;
}

bool ExitManager::install_native_leg_locked(
    std::uint64_t opener_order_id,
    std::string_view strategy_name,
    std::string_view symbol,
    order_side close_side,
    double placed_qty,
    std::string_view exchange_order_id,
    std::string_view group_id,
    native_bracket_leg_role role)
{
    if (opener_order_id == 0 || exchange_order_id.empty()
        || !std::isfinite(placed_qty) || !(placed_qty > 0.0)
        || symbol.empty() || !is_valid_native_side(close_side)
        || strategy_name.size() > native_strategy_capacity
        || symbol.size() > native_symbol_capacity
        || exchange_order_id.size() > native_id_capacity
        || group_id.size() > native_id_capacity
        || exchange_to_leg_.contains(std::string{exchange_order_id})
        || exchange_to_leg_.size() >= native_retained_leg_capacity
        || native_next_leg_token_ == 0)
        return false;

    native_venue_leg leg;
    leg.opener_order_id = opener_order_id;
    leg.reservation_leg_token = native_next_leg_token_;
    leg.role = role;
    leg.close_side = close_side;
    leg.placed_qty = placed_qty;
    if (!leg.strategy_name.assign(strategy_name)
        || !leg.symbol.assign(symbol)
        || !leg.exchange_order_id.assign(exchange_order_id)
        || !leg.group_id.assign(group_id))
        return false;

    if (!exchange_to_leg_.emplace(std::string{exchange_order_id}, std::move(leg)).second)
        return false;
    ++native_next_leg_token_;
    return true;
}

bool ExitManager::install_native_group_locked(
    std::uint64_t opener_order_id,
    std::string_view symbol,
    std::string_view group_id,
    std::string_view stop_exchange_id,
    std::string_view take_profit_exchange_id)
{
    if (opener_order_id == 0 || group_id.empty() || symbol.empty()
        || stop_exchange_id.empty() || take_profit_exchange_id.empty()
        || group_id.size() > native_id_capacity
        || symbol.size() > native_symbol_capacity
        || stop_exchange_id.size() > native_id_capacity
        || take_profit_exchange_id.size() > native_id_capacity
        || stop_exchange_id == take_profit_exchange_id
        || native_groups_.contains(std::string{group_id})
        || native_groups_.size() >= native_retained_group_capacity)
        return false;

    native_oco_group group;
    group.opener_order_id = opener_order_id;
    if (!group.group_id.assign(group_id)
        || !group.symbol.assign(symbol)
        || !group.stop_exchange_id.assign(stop_exchange_id)
        || !group.take_profit_exchange_id.assign(take_profit_exchange_id))
        return false;

    native_groups_.emplace(std::string{group_id}, std::move(group));
    return true;
}

bool ExitManager::install_native_venue_state_locked(
    std::uint64_t opener_order_id,
    std::string_view strategy_name,
    std::string_view symbol,
    order_side close_side,
    double placed_qty,
    const bracket_handles& handles)
{
    const auto stop_id = handles.sl_exchange_id
        ? std::string_view{*handles.sl_exchange_id} : std::string_view{};
    const auto take_profit_id = handles.tp_exchange_id
        ? std::string_view{*handles.tp_exchange_id} : std::string_view{};
    const auto group_id = handles.oco_list_id
        ? std::string_view{*handles.oco_list_id} : std::string_view{};

    // A group-only handle remains a legacy adapter handle, not a native
    // lifecycle registration.  It cannot safely resolve an order report
    // without an immutable per-leg exchange identity.
    if (stop_id.empty() && take_profit_id.empty()) return true;

    if ((!handles.symbol.empty() && handles.symbol != symbol)
        || (stop_id == take_profit_id && !stop_id.empty())
        || opener_order_id == 0 || strategy_name.size() > native_strategy_capacity
        || symbol.empty() || symbol.size() > native_symbol_capacity
        || !is_valid_native_side(close_side)
        || !std::isfinite(placed_qty) || !(placed_qty > 0.0)
        || (!stop_id.empty() && (stop_id.size() > native_id_capacity
                                 || exchange_to_leg_.contains(std::string{stop_id})))
        || (!take_profit_id.empty()
            && (take_profit_id.size() > native_id_capacity
                || exchange_to_leg_.contains(std::string{take_profit_id})))
        || group_id.size() > native_id_capacity
        || ((!stop_id.empty() ? 1U : 0U) + (!take_profit_id.empty() ? 1U : 0U)
                > native_retained_leg_capacity - exchange_to_leg_.size())
        || (!group_id.empty() && !stop_id.empty() && !take_profit_id.empty()
            && (native_groups_.contains(std::string{group_id})
                || native_groups_.size() >= native_retained_group_capacity)))
        return false;

    // Keep registration all-or-nothing even on a cold-path allocation
    // failure. The caller still latches reconciliation, but it must not be
    // left with a misleading half-leg/group topology in retained state.
    const auto token_before = native_next_leg_token_;
    bool stop_installed = false;
    bool take_profit_installed = false;
    bool group_installed = false;
    const auto rollback_install = [&]() noexcept {
        if (group_installed)
        {
            const auto it = native_groups_.find(group_id);
            if (it != native_groups_.end()
                && it->second.opener_order_id == opener_order_id)
                native_groups_.erase(it);
        }
        if (take_profit_installed)
        {
            const auto it = exchange_to_leg_.find(take_profit_id);
            if (it != exchange_to_leg_.end()
                && it->second.opener_order_id == opener_order_id
                && it->second.role == native_bracket_leg_role::take_profit)
                exchange_to_leg_.erase(it);
        }
        if (stop_installed)
        {
            const auto it = exchange_to_leg_.find(stop_id);
            if (it != exchange_to_leg_.end()
                && it->second.opener_order_id == opener_order_id
                && it->second.role == native_bracket_leg_role::stop_loss)
                exchange_to_leg_.erase(it);
        }
        native_next_leg_token_ = token_before;
    };

    try
    {
        if (!stop_id.empty())
        {
            if (!install_native_leg_locked(opener_order_id, strategy_name, symbol,
                                           close_side, placed_qty, stop_id, group_id,
                                           native_bracket_leg_role::stop_loss))
                return false;
            stop_installed = true;
        }
        if (!take_profit_id.empty())
        {
            if (!install_native_leg_locked(opener_order_id, strategy_name, symbol,
                                           close_side, placed_qty, take_profit_id, group_id,
                                           native_bracket_leg_role::take_profit))
            {
                rollback_install();
                return false;
            }
            take_profit_installed = true;
        }
        if (!group_id.empty() && !stop_id.empty() && !take_profit_id.empty())
        {
            if (!install_native_group_locked(opener_order_id, symbol, group_id,
                                             stop_id, take_profit_id))
            {
                rollback_install();
                return false;
            }
            group_installed = true;
        }
        return true;
    }
    catch (...)
    {
        rollback_install();
        throw;
    }
}

void ExitManager::mark_native_cancel_requested_locked(
    std::uint64_t opener_order_id, std::chrono::steady_clock::time_point now)
{
    venue_cancel_requested_openers_.insert(opener_order_id);
    for (auto& [_, leg] : exchange_to_leg_)
    {
        if (leg.opener_order_id != opener_order_id) continue;
        if (leg.lifecycle == native_leg_lifecycle::active)
        {
            leg.lifecycle = native_leg_lifecycle::cancel_requested;
            leg.terminal_deadline = now + native_sibling_terminal_deadline;
        }
    }
}

void ExitManager::mark_native_group_reconciliation_required_locked(
    native_oco_group& group)
{
    group.lifecycle = native_group_lifecycle::reconciliation_required;
    group.sibling_terminal_deadline = {};
    native_reconciliation_required_ = true;
}

void ExitManager::mark_native_reconciliation_required_locked(native_venue_leg& leg)
{
    leg.lifecycle = native_leg_lifecycle::reconciliation_required;
    leg.terminal_deadline = {};
    native_reconciliation_required_ = true;
    if (!leg.group_id.view().empty())
    {
        auto group_it = native_groups_.find(leg.group_id.view());
        if (group_it != native_groups_.end())
            mark_native_group_reconciliation_required_locked(group_it->second);
    }
}

bool ExitManager::all_native_legs_terminal_for_opener_locked(
    std::uint64_t opener_order_id) const
{
    bool found = false;
    for (const auto& [_, leg] : exchange_to_leg_)
    {
        if (leg.opener_order_id != opener_order_id) continue;
        found = true;
        if (!is_native_leg_terminal(leg.lifecycle)) return false;
    }
    return found;
}

bool ExitManager::has_native_legs_for_opener_locked(
    std::uint64_t opener_order_id) const
{
    for (const auto& [_, leg] : exchange_to_leg_)
        if (leg.opener_order_id == opener_order_id) return true;
    return false;
}

ExitManager::native_venue_leg* ExitManager::find_native_sibling_locked(
    const native_venue_leg& leg) noexcept
{
    // A non-empty group id is a venue-native OCO/list relationship and is
    // resolved through its explicit group state below. This helper exists for
    // paired independent legs (notably Binance Futures) only.
    if (!leg.group_id.view().empty()) return nullptr;
    for (auto& [_, candidate] : exchange_to_leg_)
    {
        if (&candidate != &leg
            && candidate.opener_order_id == leg.opener_order_id
            && candidate.group_id.view().empty()
            && candidate.role != leg.role)
            return &candidate;
    }
    return nullptr;
}

ExitManager::native_venue_leg* ExitManager::find_native_leg_for_reservation_locked(
    const native_bracket_economic_reservation& reservation) noexcept
{
    if (!reservation.valid()) return nullptr;
    for (auto& [_, leg] : exchange_to_leg_)
    {
        if (leg.reservation_leg_token == reservation.leg_token)
            return &leg;
    }
    return nullptr;
}

void ExitManager::maybe_complete_native_group_locked(native_oco_group& group)
{
    if (group.lifecycle == native_group_lifecycle::reconciliation_required)
        return;

    const auto stop_it = exchange_to_leg_.find(group.stop_exchange_id.view());
    const auto take_profit_it = exchange_to_leg_.find(
        group.take_profit_exchange_id.view());
    if (stop_it == exchange_to_leg_.end() || take_profit_it == exchange_to_leg_.end())
    {
        mark_native_group_reconciliation_required_locked(group);
        return;
    }
    if (!is_native_leg_terminal(stop_it->second.lifecycle)
        || !is_native_leg_terminal(take_profit_it->second.lifecycle))
        return;

    // A venue OCO/list is not clean solely because its two children happen to
    // be terminal. ALL_DONE is independent venue proof that the list itself
    // retired; without it, retain the group as unresolved and block admission.
    // Once both children are terminal, absence of that proof must also become
    // loud after the same conservative deadline as sibling confirmation; an
    // indefinitely quiet admission block is not a completed venue lifecycle.
    if (!group.completed_status_seen)
    {
        if (group.sibling_terminal_deadline
            == std::chrono::steady_clock::time_point{})
        {
            group.lifecycle = native_group_lifecycle::awaiting_sibling_terminal;
            group.sibling_terminal_deadline = std::chrono::steady_clock::now()
                + native_sibling_terminal_deadline;
        }
        return;
    }

    const auto& stop = stop_it->second;
    const auto& take_profit = take_profit_it->second;
    const bool stop_filled = stop.lifecycle == native_leg_lifecycle::terminal_filled;
    const bool take_profit_filled =
        take_profit.lifecycle == native_leg_lifecycle::terminal_filled;
    if (stop_filled == take_profit_filled)
    {
        // Both canceled/rejected/expired is a valid manual release outcome;
        // both filled is a double-execution contradiction.
        if (stop_filled)
        {
            mark_native_group_reconciliation_required_locked(group);
            return;
        }
        if (!group.winning_exchange_id.view().empty()
            || !group.expected_sibling_exchange_id.view().empty())
        {
            mark_native_group_reconciliation_required_locked(group);
            return;
        }
    }
    else
    {
        const auto& winner = stop_filled ? stop : take_profit;
        const auto& sibling = stop_filled ? take_profit : stop;
        if (!group.winning_exchange_id.equals(winner.exchange_order_id.view())
            || !group.expected_sibling_exchange_id.equals(
                sibling.exchange_order_id.view()))
        {
            mark_native_group_reconciliation_required_locked(group);
            return;
        }
    }

    group.lifecycle = native_group_lifecycle::completed;
    group.sibling_terminal_deadline = {};
    venue_cancel_requested_openers_.erase(group.opener_order_id);
    handles_.erase(group.opener_order_id);
}

void ExitManager::mark_native_leg_filled_locked(
    native_venue_leg& leg, std::chrono::steady_clock::time_point now,
    bool& sibling_economic_race, bool& sibling_cancel_required)
{
    sibling_economic_race = false;
    sibling_cancel_required = false;
    if (leg.lifecycle == native_leg_lifecycle::reconciliation_required)
    {
        sibling_economic_race = true;
        return;
    }
    if (leg.group_id.view().empty())
    {
        leg.lifecycle = native_leg_lifecycle::terminal_filled;
        leg.terminal_deadline = {};

        auto* sibling = find_native_sibling_locked(leg);
        if (!sibling) return;
        if (sibling->pending_economic.active
            || sibling->lifecycle == native_leg_lifecycle::terminal_filled
            || sibling->lifecycle == native_leg_lifecycle::reconciliation_required)
        {
            mark_native_reconciliation_required_locked(leg);
            sibling_economic_race = true;
            return;
        }
        // Unlike an atomic native list, independently placed legs have no
        // venue proof that a prior sibling cancellation belongs to this fill.
        // Attribute the fill but treat that ordering as a reconciliation race.
        if (sibling->lifecycle == native_leg_lifecycle::terminal_nonfill)
        {
            mark_native_reconciliation_required_locked(leg);
            sibling_economic_race = true;
            return;
        }
        sibling->lifecycle = native_leg_lifecycle::expected_sibling_terminal;
        sibling->terminal_deadline = now + native_sibling_terminal_deadline;
        sibling_cancel_required = true;
        return;
    }

    auto group_it = native_groups_.find(leg.group_id.view());
    if (group_it == native_groups_.end())
    {
        // A per-leg group id without a two-leg registration is not enough to
        // infer OCO semantics.  Keep the leg attributable, but do not claim a
        // sibling cancellation proof that was never registered.
        leg.lifecycle = native_leg_lifecycle::terminal_filled;
        leg.terminal_deadline = {};
        return;
    }

    auto& group = group_it->second;
    if (group.lifecycle == native_group_lifecycle::reconciliation_required)
    {
        mark_native_reconciliation_required_locked(leg);
        sibling_economic_race = true;
        return;
    }
    if (!group.winning_exchange_id.view().empty()
        && !group.winning_exchange_id.equals(leg.exchange_order_id.view()))
    {
        // The second OCO leg produced economics after its sibling was already
        // terminal.  Preserve the fill for caller accounting, then force
        // reconciliation instead of pretending the OCO guarantee held.
        mark_native_reconciliation_required_locked(leg);
        sibling_economic_race = true;
        return;
    }

    leg.lifecycle = native_leg_lifecycle::terminal_filled;
    leg.terminal_deadline = {};
    if (!group.winning_exchange_id.assign(leg.exchange_order_id.view()))
    {
        mark_native_reconciliation_required_locked(leg);
        sibling_economic_race = true;
        return;
    }

    const auto sibling_id = leg.role == native_bracket_leg_role::stop_loss
        ? group.take_profit_exchange_id.view()
        : group.stop_exchange_id.view();
    if (!group.expected_sibling_exchange_id.assign(sibling_id))
    {
        mark_native_reconciliation_required_locked(leg);
        sibling_economic_race = true;
        return;
    }
    auto sibling_it = exchange_to_leg_.find(sibling_id);
    if (sibling_it == exchange_to_leg_.end())
    {
        mark_native_group_reconciliation_required_locked(group);
        sibling_economic_race = true;
        return;
    }

    group.lifecycle = native_group_lifecycle::awaiting_sibling_terminal;
    group.sibling_terminal_deadline = now + native_sibling_terminal_deadline;
    if (!is_native_leg_terminal(sibling_it->second.lifecycle))
    {
        sibling_it->second.lifecycle = native_leg_lifecycle::expected_sibling_terminal;
        sibling_it->second.terminal_deadline = group.sibling_terminal_deadline;
    }
    maybe_complete_native_group_locked(group);
}

native_bracket_resolution ExitManager::resolve_native_bracket_update(
    const native_bracket_update& update)
{
    native_bracket_resolution result;
    // Keep an overlong or empty external id on the generic fail-closed path;
    // it cannot be an exact registered native identity.
    if (update.exchange_order_id.empty()
        || update.exchange_order_id.size() > native_id_capacity)
        return result;

    std::lock_guard<std::mutex> lk(venue_mu_);
    auto leg_it = exchange_to_leg_.find(update.exchange_order_id);
    if (leg_it == exchange_to_leg_.end()) return result;
    auto& leg = leg_it->second;

    const auto set_result = [&](native_bracket_resolution_kind kind,
                                bool terminal = false) {
        result.kind = kind;
        result.opener_order_id = leg.opener_order_id;
        result.role = leg.role;
        result.close_side = leg.close_side;
        result.remaining_qty = std::max(0.0, leg.placed_qty - leg.cumulative_qty);
        result.terminal = terminal;
        result.strategy_name = leg.strategy_name.view();
        return result;
    };
    const auto fail = [&]() {
        mark_native_reconciliation_required_locked(leg);
        return set_result(native_bracket_resolution_kind::fatal);
    };

    if (!valid_native_update_shape(update)
        || !leg.exchange_order_id.equals(update.exchange_order_id)
        || !leg.symbol.equals(update.symbol)
        || leg.close_side != update.side
        || !leg.group_id.equals(update.group_id))
        return fail();

    if (update.is_economic_fill())
    {
        for (std::size_t i = 0; i < leg.execution_history_size; ++i)
        {
            const auto& prior = leg.execution_history[i];
            if (!prior.execution_id.equals(update.execution_id)) continue;
            return prior.equals(update)
                ? set_result(native_bracket_resolution_kind::duplicate)
                : fail();
        }

        if (leg.pending_economic.active)
        {
            if (leg.pending_economic.source_sequence == update.source_sequence
                && leg.pending_economic.fingerprint.equals(update))
            {
                auto pending_result = set_result(
                    leg.pending_economic.requires_reconciliation
                        ? native_bracket_resolution_kind::economic_fill_requires_reconciliation
                        : native_bracket_resolution_kind::economic_fill,
                    leg.pending_economic.terminal);
                pending_result.remaining_qty = std::max(
                    0.0, leg.placed_qty
                        - leg.pending_economic.next_cumulative_qty);
                pending_result.reservation = {
                    update.source_sequence, leg.reservation_leg_token};
                return pending_result;
            }
            // Source order must not advance around an unaccounted economic
            // reservation.  The engine either commits it after accounting or
            // rolls it back before considering another report for this leg.
            return fail();
        }

        // Accounting is serialized on the engine thread. Do not admit a
        // second child report for the same opener while an earlier economic
        // fact is reserved but not committed/rolled back: otherwise a list
        // status or sibling terminal could be evaluated against stale
        // cumulative state.
        for (const auto& [_, other] : exchange_to_leg_)
        {
            if (&other == &leg || !other.pending_economic.active) continue;
            // `source_sequence` names a single strictly ordered private FIFO
            // record. A second outstanding leg may never claim the same
            // record, even though the opaque reservation also carries a
            // per-leg token for exact commit/rollback lookup.
            if (other.pending_economic.source_sequence == update.source_sequence
                || other.opener_order_id == leg.opener_order_id)
                return fail();
        }

        if (leg.lifecycle == native_leg_lifecycle::terminal_filled
            || leg.execution_history_size == leg.execution_history.size())
            return fail();

        const double next_cumulative = leg.cumulative_qty + update.last_fill_qty;
        if (!update.cumulative_reported
            || !same_number(update.cumulative_qty, next_cumulative)
            || update.cumulative_qty > leg.placed_qty
            || (update.lifecycle == native_bracket_lifecycle::partial_fill
                && !(update.cumulative_qty < leg.placed_qty))
            || (update.lifecycle == native_bracket_lifecycle::full_fill
                && !same_number(update.cumulative_qty, leg.placed_qty)))
            return fail();

        native_update_fingerprint fingerprint;
        if (!fingerprint.assign(update)) return fail();

        // A fill that arrives after a previously accepted non-fill terminal
        // is still authoritative venue truth.  Do not drop it just because
        // the local OCO/cancel story appeared complete: attribute/account it,
        // then require reconciliation for the contradiction.
        bool economic_race = leg.lifecycle
            == native_leg_lifecycle::terminal_nonfill
            || leg.lifecycle == native_leg_lifecycle::expected_sibling_terminal
            || leg.lifecycle == native_leg_lifecycle::reconciliation_required
            // A local cancel was already dispatched. A later economic fact is
            // still accounted, but it cannot be a clean native bracket
            // outcome: the venue may have filled while cancellation raced.
            || venue_cancel_requested_openers_.contains(leg.opener_order_id);
        for (const auto& [id, other] : exchange_to_leg_)
        {
            if (id != update.exchange_order_id
                && other.opener_order_id == leg.opener_order_id
                && (other.lifecycle == native_leg_lifecycle::terminal_filled
                    || (leg.group_id.view().empty()
                        && other.lifecycle
                            == native_leg_lifecycle::terminal_nonfill)))
            {
                economic_race = true;
                break;
            }
        }
        if (!leg.group_id.view().empty())
        {
            const auto group_it = native_groups_.find(leg.group_id.view());
            if (group_it != native_groups_.end()
                && group_it->second.lifecycle
                    == native_group_lifecycle::reconciliation_required)
                economic_race = true;
        }

        // Do not mutate cumulative/history/terminal state before engine-side
        // accounting succeeds.  A later commit performs those mutations; a
        // rollback leaves this exact source fact retriable for final drain or
        // reconciliation rather than turning it into a false duplicate.
        leg.pending_economic.active = true;
        leg.pending_economic.source_sequence = update.source_sequence;
        leg.pending_economic.fingerprint = std::move(fingerprint);
        leg.pending_economic.next_cumulative_qty = update.cumulative_qty;
        leg.pending_economic.terminal =
            update.lifecycle == native_bracket_lifecycle::full_fill;
        leg.pending_economic.requires_reconciliation = economic_race;

        auto pending_result = set_result(economic_race
            ? native_bracket_resolution_kind::economic_fill_requires_reconciliation
            : native_bracket_resolution_kind::economic_fill,
            update.lifecycle == native_bracket_lifecycle::full_fill);
        pending_result.remaining_qty = std::max(
            0.0, leg.placed_qty - update.cumulative_qty);
        pending_result.reservation = {
            update.source_sequence, leg.reservation_leg_token};
        return pending_result;
    }

    if (leg.pending_economic.active) return fail();
    for (const auto& [_, other] : exchange_to_leg_)
    {
        if (&other != &leg && other.opener_order_id == leg.opener_order_id
            && other.pending_economic.active)
            return fail();
    }

    for (std::size_t i = 0; i < leg.lifecycle_history_size; ++i)
    {
        if (leg.lifecycle_history[i].equals(update))
            return set_result(native_bracket_resolution_kind::duplicate);
    }
    if (leg.lifecycle_history_size == leg.lifecycle_history.size())
        return fail();
    if (update.cumulative_reported
        && !same_number(update.cumulative_qty, leg.cumulative_qty))
        return fail();

    const auto append_lifecycle = [&]() -> bool {
        native_update_fingerprint fingerprint;
        if (!fingerprint.assign(update)) return false;
        leg.lifecycle_history[leg.lifecycle_history_size++] = std::move(fingerprint);
        return true;
    };

    if (update.lifecycle == native_bracket_lifecycle::ack)
    {
        if (is_native_leg_terminal(leg.lifecycle)) return fail();
        if (!append_lifecycle()) return fail();
        return set_result(native_bracket_resolution_kind::lifecycle);
    }

    if (update.lifecycle == native_bracket_lifecycle::full_fill)
    {
        // valid_native_update_shape already established lifecycle_only and a
        // zero economic increment.  It can only corroborate an already
        // accounted full cumulative total.
        if (!same_number(leg.cumulative_qty, leg.placed_qty)
            || leg.lifecycle == native_leg_lifecycle::terminal_nonfill)
            return fail();
        if (!append_lifecycle()) return fail();
        bool sibling_economic_race = false;
        bool sibling_cancel_required = false;
        mark_native_leg_filled_locked(leg, std::chrono::steady_clock::now(),
                                      sibling_economic_race,
                                      sibling_cancel_required);
        if (leg.lifecycle == native_leg_lifecycle::reconciliation_required
            || sibling_economic_race)
            return set_result(native_bracket_resolution_kind::lifecycle, true);
        if (all_native_legs_terminal_for_opener_locked(leg.opener_order_id))
        {
            handles_.erase(leg.opener_order_id);
            venue_cancel_requested_openers_.erase(leg.opener_order_id);
        }
        return set_result(native_bracket_resolution_kind::lifecycle, true);
    }

    // Canceled/rejected/expired are authoritative only after their cumulative
    // total corroborates all economic increments already accounted for this
    // leg.  A spontaneous protective-leg loss is a reconciliation condition;
    // only a local cancel request or recorded OCO sibling winner makes it an
    // expected terminal.
    if (!update.cumulative_reported
        || !same_number(update.cumulative_qty, leg.cumulative_qty)
        || is_native_leg_terminal(leg.lifecycle)
        || leg.lifecycle == native_leg_lifecycle::active)
        return fail();
    if (leg.lifecycle == native_leg_lifecycle::expected_sibling_terminal)
    {
        if (!leg.group_id.view().empty())
        {
            auto group_it = native_groups_.find(leg.group_id.view());
            if (group_it == native_groups_.end()
                || !group_it->second.expected_sibling_exchange_id.equals(
                    leg.exchange_order_id.view()))
                return fail();
        }
        else
        {
            const auto* winner = find_native_sibling_locked(leg);
            if (!winner
                || winner->lifecycle != native_leg_lifecycle::terminal_filled)
                return fail();
        }
    }
    if (!append_lifecycle()) return fail();
    if (leg.lifecycle != native_leg_lifecycle::reconciliation_required)
    {
        leg.lifecycle = native_leg_lifecycle::terminal_nonfill;
        leg.terminal_deadline = {};
    }
    if (!leg.group_id.view().empty())
    {
        auto group_it = native_groups_.find(leg.group_id.view());
        if (group_it != native_groups_.end())
            maybe_complete_native_group_locked(group_it->second);
    }
    if (all_native_legs_terminal_for_opener_locked(leg.opener_order_id))
    {
        handles_.erase(leg.opener_order_id);
        venue_cancel_requested_openers_.erase(leg.opener_order_id);
    }
    return set_result(native_bracket_resolution_kind::lifecycle, true);
}

bool ExitManager::commit_native_bracket_economic(
    const native_bracket_economic_reservation& reservation) noexcept
{
    if (!reservation.valid())
    {
        try
        {
            std::lock_guard<std::mutex> lk(venue_mu_);
            native_reconciliation_required_ = true;
        }
        catch (...) {}
        return false;
    }
    try
    {
        std::lock_guard<std::mutex> lk(venue_mu_);
        auto* leg = find_native_leg_for_reservation_locked(reservation);
        if (!leg)
        {
            native_reconciliation_required_ = true;
            return false;
        }
        auto& pending = leg->pending_economic;
        if (!pending.active || pending.source_sequence != reservation.source_sequence
            || leg->execution_history_size == leg->execution_history.size()
            || leg->lifecycle == native_leg_lifecycle::terminal_filled)
        {
            mark_native_reconciliation_required_locked(*leg);
            return false;
        }

        // Commit is intentionally after canonical accounting. All fields
        // below are fixed-size; no external I/O, allocation, or adapter
        // callback may run between economic accounting and this transition.
        leg->execution_history[leg->execution_history_size++] = pending.fingerprint;
        leg->cumulative_qty = pending.next_cumulative_qty;
        if (pending.requires_reconciliation
            && leg->lifecycle != native_leg_lifecycle::reconciliation_required)
            mark_native_reconciliation_required_locked(*leg);

        if (pending.terminal)
        {
            bool sibling_economic_race = false;
            bool sibling_cancel_required = false;
            mark_native_leg_filled_locked(*leg, std::chrono::steady_clock::now(),
                                          sibling_economic_race,
                                          sibling_cancel_required);
            leg->committed_terminal_source_sequence = reservation.source_sequence;
            leg->sibling_cancel_required = sibling_cancel_required;
            leg->sibling_cancel_dispatched = false;
            if (sibling_economic_race
                && leg->lifecycle != native_leg_lifecycle::reconciliation_required)
                mark_native_reconciliation_required_locked(*leg);
        }
        pending = {};

        if (leg->lifecycle != native_leg_lifecycle::reconciliation_required
            && all_native_legs_terminal_for_opener_locked(leg->opener_order_id))
        {
            handles_.erase(leg->opener_order_id);
            venue_cancel_requested_openers_.erase(leg->opener_order_id);
        }
        return true;
    }
    catch (...)
    {
        // Caller has already accounted venue truth at this point. A failed
        // commit is never a fabricated duplicate acknowledgement; the engine
        // must latch/reconcile before any further admission.
        try
        {
            std::lock_guard<std::mutex> lk(venue_mu_);
            native_reconciliation_required_ = true;
        }
        catch (...) {}
        return false;
    }
}

bool ExitManager::rollback_native_bracket_economic(
    const native_bracket_economic_reservation& reservation) noexcept
{
    if (!reservation.valid())
    {
        try
        {
            std::lock_guard<std::mutex> lk(venue_mu_);
            native_reconciliation_required_ = true;
        }
        catch (...) {}
        return false;
    }
    try
    {
        std::lock_guard<std::mutex> lk(venue_mu_);
        auto* leg = find_native_leg_for_reservation_locked(reservation);
        if (!leg)
        {
            native_reconciliation_required_ = true;
            return false;
        }
        if (!leg->pending_economic.active
            || leg->pending_economic.source_sequence != reservation.source_sequence)
        {
            mark_native_reconciliation_required_locked(*leg);
            return false;
        }
        // Nothing economic has reached committed history/cumulative state yet,
        // so this is a true rollback rather than compensating accounting. The
        // same private record can be preflighted again for final drain.
        leg->pending_economic = {};
        return true;
    }
    catch (...)
    {
        try
        {
            std::lock_guard<std::mutex> lk(venue_mu_);
            native_reconciliation_required_ = true;
        }
        catch (...) {}
        return false;
    }
}

native_bracket_sibling_cancel_result
ExitManager::request_native_bracket_sibling_cancel(
    const native_bracket_economic_reservation& reservation) noexcept
{
    if (!reservation.valid())
    {
        try
        {
            std::lock_guard<std::mutex> lk(venue_mu_);
            native_reconciliation_required_ = true;
        }
        catch (...) {}
        return native_bracket_sibling_cancel_result::fatal;
    }

    bracket_handles handles;
    std::shared_ptr<IBracketAdapter> adapter;
    std::uint64_t opener_order_id = 0;
    try
    {
        {
            std::lock_guard<std::mutex> lk(venue_mu_);
            auto* leg = find_native_leg_for_reservation_locked(reservation);
            if (!leg
                || leg->committed_terminal_source_sequence
                    != reservation.source_sequence)
            {
                native_reconciliation_required_ = true;
                return native_bracket_sibling_cancel_result::fatal;
            }

            // Native OCO/list venues own sibling cancellation atomically. We
            // wait for their child lifecycle proof plus ALL_DONE rather than
            // sending a second, semantically weaker REST request.
            if (!leg->sibling_cancel_required || leg->sibling_cancel_dispatched
                || !leg->group_id.view().empty())
                return native_bracket_sibling_cancel_result::not_required;

            auto* sibling = find_native_sibling_locked(*leg);
            if (!sibling || sibling->lifecycle
                    != native_leg_lifecycle::expected_sibling_terminal)
            {
                mark_native_reconciliation_required_locked(*leg);
                return native_bracket_sibling_cancel_result::fatal;
            }
            if (venue_cancel_requested_openers_.contains(leg->opener_order_id))
            {
                // A normal closer/manual path already dispatched the adapter
                // request before this post-accounting step. Its private proof
                // remains the expected-sibling lifecycle above.
                leg->sibling_cancel_dispatched = true;
                return native_bracket_sibling_cancel_result::not_required;
            }

            const auto handles_it = handles_.find(leg->opener_order_id);
            if (handles_it == handles_.end() || !bracket_adapter_)
            {
                mark_native_reconciliation_required_locked(*leg);
                return native_bracket_sibling_cancel_result::fatal;
            }
            handles = handles_it->second;
            adapter = bracket_adapter_;
            opener_order_id = leg->opener_order_id;
            // No automatic retry after an ambiguous external result.
            leg->sibling_cancel_dispatched = true;
        }

        // Adapter I/O must never run under venue_mu_. For the independent
        // two-leg adapter this intentionally passes the bracket handles; the
        // winner's already-gone leg is idempotent, while the sibling receives
        // the best-effort cancellation request.
        adapter->cancel(opener_order_id, handles);

        std::lock_guard<std::mutex> lk(venue_mu_);
        auto* leg = find_native_leg_for_reservation_locked(reservation);
        if (!leg || leg->committed_terminal_source_sequence
                        != reservation.source_sequence)
        {
            native_reconciliation_required_ = true;
            return native_bracket_sibling_cancel_result::fatal;
        }
        venue_cancel_requested_openers_.insert(opener_order_id);
        return native_bracket_sibling_cancel_result::requested;
    }
    catch (...)
    {
        // The venue may have acted even though its response was ambiguous.
        // Preserve the terminal winner and expected sibling/deadline exactly
        // as they stood; only latch reconciliation. Resetting to active would
        // erase proof of the race and invite an unsafe retry.
        try
        {
            std::lock_guard<std::mutex> lk(venue_mu_);
            if (opener_order_id != 0)
                venue_cancel_requested_openers_.insert(opener_order_id);
            native_reconciliation_required_ = true;
        }
        catch (...) {}
        return native_bracket_sibling_cancel_result::fatal;
    }
}

native_bracket_group_resolution ExitManager::resolve_native_bracket_group_update(
    const native_bracket_group_update& update)
{
    if (update.group_id.empty() || update.group_id.size() > native_id_capacity)
        return native_bracket_group_resolution::not_native;

    std::lock_guard<std::mutex> lk(venue_mu_);
    auto group_it = native_groups_.find(update.group_id);
    if (group_it == native_groups_.end())
        return native_bracket_group_resolution::not_native;
    auto& group = group_it->second;
    const auto fail = [&]() {
        mark_native_group_reconciliation_required_locked(group);
        return native_bracket_group_resolution::fatal;
    };

    if (update.symbol.empty() || update.symbol.size() > native_symbol_capacity
        || update.event_time_ms <= 0 || !group.group_id.equals(update.group_id)
        || !group.symbol.equals(update.symbol))
        return fail();
    switch (update.status)
    {
    case native_bracket_group_status::active:
    case native_bracket_group_status::completed:
        break;
    default:
        return fail();
    }

    for (std::size_t i = 0; i < group.history_size; ++i)
    {
        if (group.history[i].equals(update))
            return native_bracket_group_resolution::duplicate;
    }
    if (group.history_size == group.history.size()) return fail();
    if (group.lifecycle == native_group_lifecycle::reconciliation_required)
        return fail();
    // A list cannot become ACTIVE again once ALL_DONE was observed, even if
    // child lifecycle proof is still arriving and the aggregate group has not
    // yet crossed its own completed predicate.
    if (group.completed_status_seen
        && update.status == native_bracket_group_status::active)
        return fail();
    if (update.status == native_bracket_group_status::active)
    {
        const auto stop_it = exchange_to_leg_.find(group.stop_exchange_id.view());
        const auto take_profit_it = exchange_to_leg_.find(
            group.take_profit_exchange_id.view());
        if (stop_it == exchange_to_leg_.end()
            || take_profit_it == exchange_to_leg_.end()
            || (is_native_leg_terminal(stop_it->second.lifecycle)
                && is_native_leg_terminal(take_profit_it->second.lifecycle)))
            return fail();
    }
    for (const auto& [_, leg] : exchange_to_leg_)
    {
        if (leg.group_id.equals(update.group_id)
            && leg.pending_economic.active)
            return fail();
    }

    native_group_fingerprint fingerprint;
    if (!fingerprint.assign(update)) return fail();
    group.history[group.history_size++] = std::move(fingerprint);
    if (update.status == native_bracket_group_status::completed)
    {
        // List completion is a useful corroborating fact, but it does not
        // identify which leg filled or prove its sibling terminal.  Preserve
        // it and continue blocking same-symbol admission until leg proof
        // reaches maybe_complete_native_group_locked().
        group.completed_status_seen = true;
        maybe_complete_native_group_locked(group);
    }
    return native_bracket_group_resolution::lifecycle;
}

bool ExitManager::check_native_bracket_lifecycle_deadline(
    std::chrono::steady_clock::time_point now)
{
    std::lock_guard<std::mutex> lk(venue_mu_);
    for (auto& [_, leg] : exchange_to_leg_)
    {
        if ((leg.lifecycle == native_leg_lifecycle::cancel_requested
             || leg.lifecycle == native_leg_lifecycle::expected_sibling_terminal)
            && leg.terminal_deadline != std::chrono::steady_clock::time_point{}
            && now >= leg.terminal_deadline)
            mark_native_reconciliation_required_locked(leg);
    }
    for (auto& [_, group] : native_groups_)
    {
        if (group.lifecycle == native_group_lifecycle::awaiting_sibling_terminal
            && group.sibling_terminal_deadline
                != std::chrono::steady_clock::time_point{}
            && now >= group.sibling_terminal_deadline)
            mark_native_group_reconciliation_required_locked(group);
    }
    return !native_reconciliation_required_;
}

bool ExitManager::native_group_blocks_symbol_admission_locked(
    std::string_view symbol) const
{
    for (const auto& [_, leg] : exchange_to_leg_)
    {
        if (ascii_case_equal(leg.symbol.view(), symbol)
            && (leg.pending_economic.active
                || is_native_leg_unresolved(leg.lifecycle)))
            return true;
    }
    for (const auto& [_, group] : native_groups_)
    {
        if (ascii_case_equal(group.symbol.view(), symbol)
            && group.lifecycle != native_group_lifecycle::completed)
            return true;
    }
    return false;
}

bool ExitManager::native_bracket_blocks_symbol_admission(
    std::string_view symbol) const
{
    if (symbol.empty() || symbol.size() > native_symbol_capacity) return true;
    std::lock_guard<std::mutex> lk(venue_mu_);
    return native_reconciliation_required_
        || native_group_blocks_symbol_admission_locked(symbol);
}

bool ExitManager::has_unresolved_native_bracket_lifecycle() const
{
    std::lock_guard<std::mutex> lk(venue_mu_);
    if (native_reconciliation_required_) return true;
    for (const auto& [_, leg] : exchange_to_leg_)
        if (leg.pending_economic.active
            || is_native_leg_unresolved(leg.lifecycle))
            return true;
    for (const auto& [_, group] : native_groups_)
        if (group.lifecycle != native_group_lifecycle::completed) return true;
    return false;
}

bool ExitManager::native_bracket_requires_reconciliation() const
{
    std::lock_guard<std::mutex> lk(venue_mu_);
    return native_reconciliation_required_;
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
    // so a later engine-thread native-lifecycle resolver has immutable
    // attribution from the first private report onward.
    {
        std::lock_guard<std::mutex> lk(venue_mu_);
        bool installed = false;
        try
        {
            installed = install_native_venue_state_locked(
                rb.opener_order_id, rb.strategy_name, rb.symbol,
                rb.close_side, rb.qty, rb.handles);
        }
        catch (...)
        {
            native_reconciliation_required_ = true;
            throw;
        }
        if (!installed)
        {
            native_reconciliation_required_ = true;
            throw std::runtime_error(
                "ExitManager: recovered native bracket failed immutable identity registration");
        }
        try
        {
            handles_.emplace(rb.opener_order_id, rb.handles);
        }
        catch (...)
        {
            native_reconciliation_required_ = true;
            throw;
        }
    }
}

void ExitManager::release_venue_bracket(std::uint64_t opener_order_id)
{
    bracket_handles handles;
    bool opaque_legacy_handles = false;
    {
        std::lock_guard<std::mutex> lk(venue_mu_);
        auto it = handles_.find(opener_order_id);
        if (it == handles_.end()) return;

        // A handle that lacks immutable leg ids is intentionally not native
        // lifecycle state. Keep legacy teardown semantics for it instead of
        // retaining an opaque handle forever with no private proof path.
        opaque_legacy_handles = !has_native_legs_for_opener_locked(opener_order_id);
        if (opaque_legacy_handles)
        {
            handles = std::move(it->second);
            handles_.erase(it);
        }
        else
        {
            if (venue_cancel_requested_openers_.contains(opener_order_id)) return;

            // Native terminal economics are reserved before canonical engine
            // accounting. If handle_engine_fill() reaches its ordinary closer
            // cleanup during that interval, defer adapter I/O until the exact
            // reservation commits. Otherwise an accounting throw could leave
            // a speculative REST cancellation ahead of the venue fact.
            for (const auto& [_, leg] : exchange_to_leg_)
            {
                if (leg.opener_order_id == opener_order_id
                    && leg.pending_economic.active
                    && leg.pending_economic.terminal)
                    return;
            }

            // A terminal winner may already have been observed before the normal
            // closer-fill path reaches this helper.  Do not manufacture a second
            // cancel request in that fully settled single-leg case; grouped legs
            // still need their sibling terminal proof and are handled below.
            if (all_native_legs_terminal_for_opener_locked(opener_order_id))
            {
                handles_.erase(it);
                return;
            }

            handles = it->second;
            mark_native_cancel_requested_locked(opener_order_id,
                                                std::chrono::steady_clock::now());
        }
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
            // Cancellation was not authoritative. Restore active lifecycle
            // state rather than leaving a failed request looking privately
            // confirmed. Reverse identities were never removed, so a race
            // with an inbound fill remains attributable either way.
            std::lock_guard<std::mutex> lk(venue_mu_);
            if (opaque_legacy_handles)
            {
                handles_.insert_or_assign(opener_order_id, std::move(handles));
                throw;
            }
            bool sibling_proof_outstanding = false;
            for (auto& [_, leg] : exchange_to_leg_)
            {
                if (leg.opener_order_id != opener_order_id) continue;
                sibling_proof_outstanding = sibling_proof_outstanding
                    || leg.lifecycle == native_leg_lifecycle::terminal_filled
                    || leg.lifecycle
                        == native_leg_lifecycle::expected_sibling_terminal;
                if (leg.lifecycle == native_leg_lifecycle::cancel_requested)
                {
                    leg.lifecycle = native_leg_lifecycle::active;
                    leg.terminal_deadline = {};
                }
            }
            if (sibling_proof_outstanding)
            {
                // An independent/normal cancel may have reached the venue
                // despite its ambiguous response. Preserve winner/sibling
                // state (especially its deadline), remember that a request
                // was dispatched so no automatic retry can occur, and latch
                // reconciliation; only unproven cancel_requested legs were
                // restored above.
                venue_cancel_requested_openers_.insert(opener_order_id);
                native_reconciliation_required_ = true;
                for (auto& [_, group] : native_groups_)
                {
                    if (group.opener_order_id == opener_order_id)
                        mark_native_group_reconciliation_required_locked(group);
                }
            }
            else
            {
                venue_cancel_requested_openers_.erase(opener_order_id);
            }
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
        native_groups_.clear();
        venue_cancel_requested_openers_.clear();
        native_reconciliation_required_ = false;
    }
    // Note: bracket_adapter_ is intentionally not cleared — it is set once at startup.
}

}
