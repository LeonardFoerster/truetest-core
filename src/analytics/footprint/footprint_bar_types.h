#pragma once

#include <cstdint>
#include <unordered_map>

// Pure bar/cell value types for the footprint aggregation engine.
// footprint.md §2.2. No I/O, no allocation policy beyond a plain
// unordered_map per bar (cold-path research math only - never touched by
// the engine event loop; see cyrex/00-architecture.md §2/§7 for the
// dirty-flag + version-counter convention this module follows).
namespace truetest::footprint {

// Grouped price level = price_ticks / group_size (group_size >= 1; "Auto"
// tick grouping, footprint.md §1, is resolved to a concrete multiplier by
// the caller before construction - the aggregator only ever sees a fixed
// integer group size). Integer division only - never floating point price
// bucketing (§2.2).
using price_level = std::int64_t;

enum class bar_kind : std::uint8_t
{
    time,   // UTC-aligned half-open interval (1s/5s/15s/1m/5m)
    volume, // closes at/above a saved quote-notional threshold
};

struct BarSpec
{
    bar_kind kind = bar_kind::time;
    std::int64_t interval_ns = 60'000'000'000LL; // time bars only
    double volume_threshold = 0.0;               // volume bars only, quote-notional units
};

// One grouped price level's split volume within a bar. Base-quantity atoms
// - same unit as PublicTrade::base_qty_atoms.
struct FootprintCell
{
    std::int64_t sell_base_qty = 0;    // aggressor sell (hit the bid) - left side
    std::int64_t buy_base_qty = 0;     // aggressor buy (lifted the offer) - right side
    std::int64_t unknown_base_qty = 0; // unknown aggression - counts to total/OHLC only

    enum class imbalance : std::uint8_t { none, buy, sell };
    imbalance diagonal = imbalance::none; // vs the adjacent grouped level, computed on bar close
    bool stacked = false;                 // part of a run of >=3 consecutive same-direction levels

    std::int64_t total() const noexcept
    {
        return sell_base_qty + buy_base_qty + unknown_base_qty;
    }

    // Unknown aggression contributes to total/OHLC but not delta/CVD/imbalance (§2.2).
    std::int64_t delta() const noexcept
    {
        return buy_base_qty - sell_base_qty;
    }
};

enum class bar_state : std::uint8_t
{
    forming,  // still accumulating trades - fields may still change
    complete, // closed; the aggregator never mutates it again (§2.2 "never
              // mutate already-published historical bars silently")
    empty,    // time interval elapsed with zero observed trades - distinct
              // from a reconciliation GAP (see `gap` below).
};

struct FootprintBar
{
    std::int64_t start_ns = 0;
    std::int64_t end_ns = 0; // half-open [start_ns, end_ns); volume bars only
                              // know end_ns once closed.

    bar_state state = bar_state::forming;

    // Reconciliation hook only - the pure aggregator in footprint_aggregator.h
    // never sets this. A future §2.2 cache/reconciliation layer, which knows
    // about missing source data the aggregator itself cannot infer, marks
    // bars it knows are incomplete so the desk (§2.3) has one bar-status
    // surface instead of two parallel ones.
    bool gap = false;

    bool has_trades = false;
    std::int64_t open_price_ticks = 0;
    std::int64_t high_price_ticks = 0;
    std::int64_t low_price_ticks = 0;
    std::int64_t close_price_ticks = 0;

    std::int64_t buy_volume = 0;     // bar-level totals, base qty atoms
    std::int64_t sell_volume = 0;
    std::int64_t unknown_volume = 0;

    // Sum of price*qty over every trade in the bar, in real (double) units -
    // display/volume-bar-threshold only. Never used for price bucketing
    // (§2.2 "integer tick arithmetic throughout" governs cells/POC/levels,
    // not this notional total).
    double quote_notional = 0.0;

    price_level poc_level = 0; // valid only when has_trades
    bool poc_valid = false;

    // Running CVD (cumulative buy-aggressor minus sell-aggressor, §2.2) as
    // of this bar's last trade - the desk's CVD subplot is this series
    // across bars. Frozen once the bar closes, like every other field here;
    // live-updates on a forming bar just like poc_level does.
    std::int64_t cvd = 0;

    std::unordered_map<price_level, FootprintCell> cells;

    std::int64_t total_volume() const noexcept
    {
        return buy_volume + sell_volume + unknown_volume;
    }
};

} // namespace truetest::footprint
