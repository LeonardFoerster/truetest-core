#include "exits/exit_manager.h"

#include <algorithm>
#include <limits>

namespace truetest::exits {

void ExitManager::register_pending(exit_intent intent)
{
    key_t k{intent.strategy_name, intent.symbol};
    if (intent.opener_order_id != 0)
        opener_id_to_key_[intent.opener_order_id] = k;
    pending_.emplace(k, std::move(intent));
}

void ExitManager::on_fill(const fill_event& f)
{
    auto it = opener_id_to_key_.find(f.get_order_id());
    if (it == opener_id_to_key_.end())
        return;

    key_t k = it->second;
    opener_id_to_key_.erase(it);

    // Promote every pending intent at this key to armed using the fill
    // price and fraction-of-fill qty (TP1/TP2/SL plans flip in one go).
    auto range = pending_.equal_range(k);
    if (range.first == range.second) return;

    for (auto pit = range.first; pit != range.second; ++pit)
    {
        armed_intent ai;
        ai.intent      = std::move(pit->second);
        ai.entry_price = f.get_fill_price();
        ai.best_price  = f.get_fill_price();
        // Clamp to [0,1] so a mis-authored fraction can't overflow position.
        double frac = ai.intent.qty_fraction;
        if (frac <= 0.0) frac = 1.0;
        if (frac > 1.0)  frac = 1.0;
        ai.intent.qty = f.get_filled_quantity() * frac;
        armed_.emplace(k, std::move(ai));
    }
    pending_.erase(range.first, range.second);
}

std::optional<order_event> ExitManager::on_price(
    const std::string& symbol, double px,
    std::chrono::system_clock::time_point ts)
{
    if (armed_.empty() || !(px > 0.0))
        return std::nullopt;

    for (auto it = armed_.begin(); it != armed_.end(); ++it)
    {
        if (it->first.second != symbol) continue;
        auto& ai = it->second;

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

        if (!sl_hit && !tp_hit && !time_hit)
            continue;

        order_event close(ts, ai.intent.symbol,
                          order_type::market, ai.intent.close_side,
                          ai.intent.qty, px);
        armed_.erase(it);
        return close;
    }
    return std::nullopt;
}

void ExitManager::cancel(const std::string& strategy_name, const std::string& symbol)
{
    key_t k{strategy_name, symbol};
    pending_.erase(k);
    armed_.erase(k);
    for (auto it = opener_id_to_key_.begin(); it != opener_id_to_key_.end(); )
    {
        if (it->second == k) it = opener_id_to_key_.erase(it);
        else ++it;
    }
}

} // namespace truetest::exits
