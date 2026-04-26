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
    on_fill(f, f.get_order_id());
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

    for (auto it = range.first; it != range.second; ++it)
    {
        armed_intent ai;
        ai.intent      = std::move(it->second);
        ai.entry_price = f.get_fill_price();
        ai.best_price  = f.get_fill_price();

        double frac = ai.intent.qty_fraction;
        if (frac <= 0.0) frac = 1.0;
        if (frac > 1.0)  frac = 1.0;
        ai.intent.qty = f.get_filled_quantity() * frac;

        armed_.emplace(f.get_order_id(), std::move(ai));
    }
    pending_.erase(range.first, range.second);
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

} // namespace truetest::exits
