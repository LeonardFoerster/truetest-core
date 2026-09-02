#include "ui/desk/footprint_panel_state.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace truetest::ui::desk {

std::int64_t footprint_bar_type_interval_ns(FootprintPanelSettings::BarType type) noexcept
{
    using BT = FootprintPanelSettings::BarType;
    switch (type)
    {
    case BT::time_1s:  return 1'000'000'000LL;
    case BT::time_5s:  return 5'000'000'000LL;
    case BT::time_15s: return 15'000'000'000LL;
    case BT::time_1m:  return 60'000'000'000LL;
    case BT::time_5m:  return 300'000'000'000LL;
    case BT::volume:   return 60'000'000'000LL; // unused for volume bars
    }
    return 60'000'000'000LL;
}

const char* footprint_bar_type_label(FootprintPanelSettings::BarType type) noexcept
{
    using BT = FootprintPanelSettings::BarType;
    switch (type)
    {
    case BT::time_1s:  return "1s";
    case BT::time_5s:  return "5s";
    case BT::time_15s: return "15s";
    case BT::time_1m:  return "1m";
    case BT::time_5m:  return "5m";
    case BT::volume:   return "Volume";
    }
    return "?";
}

namespace {

truetest::footprint::FootprintAggregatorConfig config_from_settings(
    const FootprintPanelSettings& s, double tick_size, double qty_atom_scale)
{
    truetest::footprint::FootprintAggregatorConfig cfg;
    if (s.bar_type == FootprintPanelSettings::BarType::volume)
    {
        cfg.bar_spec.kind = truetest::footprint::bar_kind::volume;
        cfg.bar_spec.volume_threshold = s.volume_threshold;
    }
    else
    {
        cfg.bar_spec.kind = truetest::footprint::bar_kind::time;
        cfg.bar_spec.interval_ns = footprint_bar_type_interval_ns(s.bar_type);
    }
    cfg.group_size = std::max(1, s.tick_group);
    cfg.tick_size = tick_size;
    cfg.qty_atom_scale = qty_atom_scale;
    cfg.imbalance_min_volume = s.imbalance_min_volume;
    cfg.cvd_reset_ns_of_day = static_cast<std::int64_t>(s.cvd_reset_hour_utc) * 3600LL * 1'000'000'000LL;
    cfg.max_bars = 512; // footprint.md §2.3 "materialize up to 512 bars"
    return cfg;
}

// Fixed-seed xorshift64 - deterministic, no <random> device, matches the
// rest of the desk's demo-data determinism convention (demo_research.cpp
// uses sin/cos/fmod for the same reason).
std::uint64_t xorshift64(std::uint64_t& state) noexcept
{
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

// Deterministic synthetic public-trade stream for the footprint demo
// fixture - always labeled DEMO DATA by the caller. Feeds the real §2.2
// aggregator rather than hand-rolling bars, so toolbar settings changes
// genuinely affect what's rendered.
std::vector<truetest::footprint::PublicTrade> make_demo_public_trades(
    std::int64_t start_ns, int trade_count,
    double base_price, double tick_size, double qty_atom_scale)
{
    using truetest::footprint::PublicTrade;
    using truetest::footprint::aggressor_side;

    std::vector<PublicTrade> out;
    out.reserve(static_cast<std::size_t>(std::max(trade_count, 0)));

    double price = base_price;
    std::uint64_t rng = 0x9E3779B97F4A7C15ULL;
    std::int64_t ns = start_ns;

    for (int i = 0; i < trade_count; ++i)
    {
        const double drift = std::sin(static_cast<double>(i) * 0.013) * 0.6;
        const double jitter = static_cast<double>(xorshift64(rng) % 2001) / 1000.0 - 1.0; // [-1,1]
        price += drift * tick_size + jitter * tick_size * 3.0;
        price = std::clamp(price, base_price * 0.9, base_price * 1.1);

        PublicTrade t;
        t.event_ns = ns;
        t.recv_ns = ns;
        t.price_ticks = static_cast<std::int64_t>(std::llround(price / tick_size));
        const std::uint64_t qty_roll = xorshift64(rng) % 400;
        t.base_qty_atoms = static_cast<std::int64_t>(
            (static_cast<double>(qty_roll) + 5.0) * qty_atom_scale / 10.0);
        const std::uint64_t side_roll = xorshift64(rng) % 100;
        t.side = side_roll < 4 ? aggressor_side::unknown
               : (side_roll % 2 == 0 ? aggressor_side::buy : aggressor_side::sell);
        t.obs_seq = static_cast<std::uint64_t>(i);
        t.session_id = 1;
        // Local DEMO DATA never enters cache/reconciliation. Give it an
        // explicit non-venue identity instead of leaving the production
        // unresolved sentinel, which the aggregator correctly rejects.
        t.venue_id = std::numeric_limits<std::uint16_t>::max() - 1;
        t.symbol_id = 0; // first dense SymbolTable id is valid
        out.push_back(t);

        // 0.2 - 1.1s between prints.
        ns += 200'000'000LL + static_cast<std::int64_t>(xorshift64(rng) % 900'000'000ULL);
    }
    return out;
}

constexpr double kDemoTickSize = 0.5;
constexpr double kDemoQtyAtomScale = 100.0;
constexpr double kDemoBasePrice = 68120.0;

} // namespace

FootprintDemoState::FootprintDemoState()
    : aggregator(truetest::footprint::FootprintAggregatorConfig{})
{
    trades = make_demo_public_trades(1'754'200'000'000'000'000LL, 2'400,
                                     kDemoBasePrice, kDemoTickSize, kDemoQtyAtomScale);
    reaggregate();
}

void FootprintDemoState::reaggregate()
{
    aggregator.reset(config_from_settings(settings, kDemoTickSize, kDemoQtyAtomScale));
    for (const auto& t : trades)
        aggregator.on_trade(t);
    aggregator.flush();
}

} // namespace truetest::ui::desk
