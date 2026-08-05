#include "ui/desk/footprint_presentation_bridge.h"

#include <algorithm>

namespace truetest::ui::desk {

namespace {

using truetest::footprint::bar_state;
using truetest::footprint::FootprintCell;

FootprintBarState to_view_state(bar_state s) noexcept
{
    switch (s)
    {
    case bar_state::forming:  return FootprintBarState::forming;
    case bar_state::complete: return FootprintBarState::complete;
    case bar_state::empty:    return FootprintBarState::empty;
    }
    return FootprintBarState::empty;
}

FootprintImbalance to_view_imbalance(FootprintCell::imbalance i) noexcept
{
    switch (i)
    {
    case FootprintCell::imbalance::none: return FootprintImbalance::none;
    case FootprintCell::imbalance::buy:  return FootprintImbalance::buy;
    case FootprintCell::imbalance::sell: return FootprintImbalance::sell;
    }
    return FootprintImbalance::none;
}

} // namespace

std::vector<FootprintBarView> to_footprint_bar_views(
    const truetest::footprint::FootprintAggregator& aggregator,
    const FootprintPresentationOptions& options)
{
    const auto& cfg = aggregator.config();
    const double tick_size = cfg.tick_size;
    const double qty_scale = cfg.qty_atom_scale > 0.0 ? cfg.qty_atom_scale : 1.0;
    const std::int64_t group_size = cfg.group_size;

    auto to_price = [tick_size](std::int64_t price_ticks) {
        return static_cast<double>(price_ticks) * tick_size;
    };
    // The level key is price_ticks / group_size (floor); render at the
    // group's lower-edge price so adjacent groups tile without gaps.
    auto level_price = [&](truetest::footprint::price_level level) {
        return to_price(level * group_size);
    };
    auto to_base_qty = [qty_scale](std::int64_t atoms) {
        return static_cast<double>(atoms) / qty_scale;
    };

    std::vector<FootprintBarView> out;
    out.reserve(aggregator.bars().size());
    for (const auto& bar : aggregator.bars())
    {
        FootprintBarView view;
        view.start_ms = bar.start_ns / 1'000'000;
        view.end_ms = bar.end_ns / 1'000'000;
        view.state = to_view_state(bar.state);
        view.gap = bar.gap;
        view.open = to_price(bar.open_price_ticks);
        view.high = to_price(bar.high_price_ticks);
        view.low = to_price(bar.low_price_ticks);
        view.close = to_price(bar.close_price_ticks);
        // bar.cvd is raw base_qty_atoms (same unit as cell buy/sell_base_qty)
        // - convert to display base units exactly like to_base_qty() below,
        // so CVD and per-level quantities stay on the same visible scale.
        view.cvd = to_base_qty(bar.cvd);

        view.levels.reserve(bar.cells.size());
        for (const auto& [level, cell] : bar.cells)
        {
            FootprintLevelView lv;
            lv.price = level_price(level);
            const double notional_factor = options.quote_units ? lv.price : 1.0;
            lv.sell_qty = to_base_qty(cell.sell_base_qty) * notional_factor;
            lv.buy_qty = to_base_qty(cell.buy_base_qty) * notional_factor;
            lv.unknown_qty = to_base_qty(cell.unknown_base_qty) * notional_factor;
            lv.diagonal = to_view_imbalance(cell.diagonal);
            lv.stacked = cell.stacked;
            lv.is_poc = bar.poc_valid && level == bar.poc_level;
            view.levels.push_back(lv);
        }
        std::sort(view.levels.begin(), view.levels.end(),
                  [](const FootprintLevelView& a, const FootprintLevelView& b) {
                      return a.price < b.price;
                  });

        out.push_back(std::move(view));
    }
    return out;
}

} // namespace truetest::ui::desk
