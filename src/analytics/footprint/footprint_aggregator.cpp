#include "analytics/footprint/footprint_aggregator.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace truetest::footprint {

namespace {
constexpr double kDiagonalImbalanceRatio = 3.0; // 300%, footprint.md §2.2 - fixed by spec
constexpr std::int64_t kDayNs = 86'400LL * 1'000'000'000LL;

FootprintAggregatorConfig sanitize(FootprintAggregatorConfig cfg)
{
    if (cfg.group_size < 1) cfg.group_size = 1;
    if (cfg.max_bars < 1) cfg.max_bars = 1;
    // interval_ns is a divisor/step in ensure_bar_for and roll_time_bars_to.
    // <=0 is caller error (bad config, corrupt request) - clamp to the
    // struct's own documented default rather than SIGFPE (0) or hanging
    // forever synthesizing backwards-shrinking "intervals" (negative).
    if (cfg.bar_spec.kind == bar_kind::time && cfg.bar_spec.interval_ns <= 0)
        cfg.bar_spec.interval_ns = 60'000'000'000LL;
    return cfg;
}
} // namespace

FootprintAggregator::FootprintAggregator(FootprintAggregatorConfig config)
    : config_(sanitize(std::move(config)))
{
}

void FootprintAggregator::reset(FootprintAggregatorConfig new_config)
{
    config_ = sanitize(std::move(new_config));
    bars_.clear();
    cvd_ = 0;
    cvd_last_boundary_ns_ = 0;
    cvd_initialized_ = false;
    ++version_; // signal the change even though bars_ is now empty
}

void FootprintAggregator::apply_cvd(const PublicTrade& trade)
{
    // Assumes trade.event_ns - reset_ns_of_day is non-negative, true for any
    // real epoch-nanosecond timestamp since reset_ns_of_day is < one day.
    const std::int64_t recent_boundary =
        ((trade.event_ns - config_.cvd_reset_ns_of_day) / kDayNs) * kDayNs
        + config_.cvd_reset_ns_of_day;

    if (!cvd_initialized_)
    {
        cvd_last_boundary_ns_ = recent_boundary;
        cvd_initialized_ = true;
    }
    else if (recent_boundary > cvd_last_boundary_ns_)
    {
        cvd_ = 0;
        cvd_last_boundary_ns_ = recent_boundary;
    }

    if (trade.side == aggressor_side::buy)
        cvd_ += trade.base_qty_atoms;
    else if (trade.side == aggressor_side::sell)
        cvd_ -= trade.base_qty_atoms;
    // unknown aggression: no CVD contribution (§2.2)
}

void FootprintAggregator::ensure_bar_for(std::int64_t event_ns)
{
    if (bars_.empty())
    {
        FootprintBar first;
        if (config_.bar_spec.kind == bar_kind::time)
        {
            const std::int64_t interval = config_.bar_spec.interval_ns;
            first.start_ns = (event_ns / interval) * interval;
            first.end_ns = first.start_ns + interval;
        }
        else
        {
            first.start_ns = event_ns;
            first.end_ns = 0; // set when the volume bar closes
        }
        bars_.push_back(std::move(first));
        return;
    }

    if (config_.bar_spec.kind == bar_kind::time)
        roll_time_bars_to(event_ns);
    // volume-kind: always append into the current forming bar - closing (if
    // any) happens explicitly in on_trade() after the trade lands, since it
    // depends on the trade's own contribution to quote_notional.
}

void FootprintAggregator::roll_time_bars_to(std::int64_t event_ns)
{
    const std::int64_t interval = config_.bar_spec.interval_ns;

    // Safety bound: an anomalously large event_ns gap (corrupt timestamp,
    // multi-year clock jump) must not hang or exhaust memory synthesizing
    // one EMPTY bar per interval. Anything past max_bars would be evicted
    // by trim_to_max_bars() before it could ever be read anyway, so once
    // the synthesized run reaches that cap, fast-forward straight to the
    // interval containing event_ns instead of stepping through the rest.
    const std::size_t synth_cap = config_.max_bars + 1;
    std::size_t synthesized = 0;

    while (true)
    {
        FootprintBar& cur = bars_.back();
        if (event_ns < cur.end_ns)
            break; // belongs to (or precedes - caller's reorder job) cur

        close_current_bar();

        FootprintBar next;
        next.start_ns = (synthesized < synth_cap)
            ? cur.end_ns
            : (event_ns / interval) * interval; // fast-forward past the doomed-to-be-evicted stretch
        next.end_ns = next.start_ns + interval;
        bars_.push_back(std::move(next));
        ++synthesized;
    }
}

void FootprintAggregator::close_current_bar()
{
    if (bars_.empty())
        return;
    FootprintBar& cur = bars_.back();
    if (cur.state != bar_state::forming)
        return; // already closed - never mutate a closed bar again (§2.2)

    cur.state = cur.has_trades ? bar_state::complete : bar_state::empty;
    if (cur.has_trades)
        recompute_derived(cur);
}

void FootprintAggregator::recompute_derived(FootprintBar& bar) const
{
    if (!bar.has_trades)
    {
        bar.poc_valid = false;
        return;
    }
    recompute_poc(bar);
    recompute_imbalance(bar);
}

void FootprintAggregator::recompute_poc(FootprintBar& bar) const
{
    if (bar.cells.empty())
    {
        bar.poc_valid = false;
        return;
    }

    const price_level close_level = bar.close_price_ticks / config_.group_size;
    bool have = false;
    std::int64_t best_total = 0;
    price_level best_level = 0;

    for (const auto& [level, cell] : bar.cells)
    {
        const std::int64_t total = cell.total();
        if (!have)
        {
            best_total = total;
            best_level = level;
            have = true;
            continue;
        }
        if (total > best_total)
        {
            best_total = total;
            best_level = level;
            continue;
        }
        if (total == best_total)
        {
            // Tie-break: nearest the close, then toward the lower price (§2.2).
            const std::int64_t cur_dist =
                level >= close_level ? level - close_level : close_level - level;
            const std::int64_t best_dist =
                best_level >= close_level ? best_level - close_level : close_level - best_level;
            if (cur_dist < best_dist || (cur_dist == best_dist && level < best_level))
                best_level = level;
        }
    }

    bar.poc_level = best_level;
    bar.poc_valid = true;
}

void FootprintAggregator::recompute_imbalance(FootprintBar& bar) const
{
    // Recompute is idempotent/full - always derived fresh from current
    // cell volumes, never incrementally patched, so a forming bar's flags
    // never lag behind its latest trade.
    for (auto& [level, cell] : bar.cells)
    {
        cell.diagonal = FootprintCell::imbalance::none;
        cell.stacked = false;
    }

    for (auto& [level, cell] : bar.cells)
    {
        // Buy diagonal: this level's buy volume vs the sell volume one
        // grouped level below (§2.2 "diagonal imbalance against the
        // adjacent grouped price level").
        if (auto it = bar.cells.find(level - 1); it != bar.cells.end())
        {
            const std::int64_t below_sell = it->second.sell_base_qty;
            if (cell.buy_base_qty >= config_.imbalance_min_volume && below_sell > 0 &&
                static_cast<double>(cell.buy_base_qty) >=
                    kDiagonalImbalanceRatio * static_cast<double>(below_sell))
            {
                cell.diagonal = FootprintCell::imbalance::buy;
            }
        }
        // Sell diagonal: this level's sell volume vs the buy volume one
        // grouped level above. A level that would qualify for both
        // directions keeps its buy flag (computed first) - documented,
        // deterministic tie-break rather than an arbitrary last-write.
        if (cell.diagonal == FootprintCell::imbalance::none)
        {
            if (auto it = bar.cells.find(level + 1); it != bar.cells.end())
            {
                const std::int64_t above_buy = it->second.buy_base_qty;
                if (cell.sell_base_qty >= config_.imbalance_min_volume && above_buy > 0 &&
                    static_cast<double>(cell.sell_base_qty) >=
                        kDiagonalImbalanceRatio * static_cast<double>(above_buy))
                {
                    cell.diagonal = FootprintCell::imbalance::sell;
                }
            }
        }
    }

    // Stacked: runs of >=3 physically consecutive grouped levels sharing
    // the same diagonal direction (§2.2).
    std::vector<price_level> levels;
    levels.reserve(bar.cells.size());
    for (const auto& [level, cell] : bar.cells)
        levels.push_back(level);
    std::sort(levels.begin(), levels.end());

    std::size_t run_start = 0;
    for (std::size_t i = 1; i <= levels.size(); ++i)
    {
        bool contiguous_same_dir = false;
        if (i < levels.size())
        {
            const auto dir_prev = bar.cells[levels[i - 1]].diagonal;
            const auto dir_cur = bar.cells[levels[i]].diagonal;
            contiguous_same_dir = (levels[i] == levels[i - 1] + 1) &&
                                   dir_prev != FootprintCell::imbalance::none &&
                                   dir_prev == dir_cur;
        }
        if (!contiguous_same_dir)
        {
            if (i - run_start >= 3)
            {
                for (std::size_t j = run_start; j < i; ++j)
                    bar.cells[levels[j]].stacked = true;
            }
            run_start = i;
        }
    }
}

void FootprintAggregator::trim_to_max_bars()
{
    while (bars_.size() > config_.max_bars)
        bars_.pop_front();
}

void FootprintAggregator::on_trade(const PublicTrade& trade)
{
    apply_cvd(trade);
    ensure_bar_for(trade.event_ns);

    FootprintBar& bar = bars_.back();
    if (!bar.has_trades)
    {
        bar.open_price_ticks = trade.price_ticks;
        bar.high_price_ticks = trade.price_ticks;
        bar.low_price_ticks = trade.price_ticks;
    }
    else
    {
        bar.high_price_ticks = std::max(bar.high_price_ticks, trade.price_ticks);
        bar.low_price_ticks = std::min(bar.low_price_ticks, trade.price_ticks);
    }
    bar.close_price_ticks = trade.price_ticks;
    bar.has_trades = true;
    bar.cvd = cvd_; // running total as of this trade - frozen once the bar closes

    const price_level level = trade.price_ticks / config_.group_size;
    FootprintCell& cell = bar.cells[level];
    switch (trade.side)
    {
        case aggressor_side::buy:
            cell.buy_base_qty += trade.base_qty_atoms;
            bar.buy_volume += trade.base_qty_atoms;
            break;
        case aggressor_side::sell:
            cell.sell_base_qty += trade.base_qty_atoms;
            bar.sell_volume += trade.base_qty_atoms;
            break;
        case aggressor_side::unknown:
        default:
            cell.unknown_base_qty += trade.base_qty_atoms;
            bar.unknown_volume += trade.base_qty_atoms;
            break;
    }

    const double price = static_cast<double>(trade.price_ticks) * config_.tick_size;
    const double qty = static_cast<double>(trade.base_qty_atoms) / config_.qty_atom_scale;
    bar.quote_notional += price * qty;

    recompute_derived(bar);
    ++version_;

    if (config_.bar_spec.kind == bar_kind::volume &&
        config_.bar_spec.volume_threshold > 0.0 &&
        bar.quote_notional >= config_.bar_spec.volume_threshold)
    {
        bar.end_ns = trade.event_ns; // the closing trade's own timestamp
        close_current_bar();

        FootprintBar next;
        next.start_ns = trade.event_ns;
        bars_.push_back(std::move(next));
    }

    trim_to_max_bars();
}

void FootprintAggregator::flush()
{
    if (bars_.empty())
        return;
    close_current_bar();
    ++version_;
}

} // namespace truetest::footprint
