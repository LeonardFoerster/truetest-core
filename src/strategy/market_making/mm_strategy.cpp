#include "mm_strategy.h"

#include <bit>
#include <cstddef>

namespace truetest::mm
{

namespace
{

constexpr std::uint64_t fnv_offset = 1469598103934665603ULL;
constexpr std::uint64_t fnv_prime = 1099511628211ULL;

void mix(std::uint64_t& h, std::uint64_t v) noexcept
{
    for (int i = 0; i < 8; ++i)
    {
        h ^= static_cast<std::uint64_t>((v >> (i * 8)) & 0xFFULL);
        h *= fnv_prime;
    }
}

void mix_f64(std::uint64_t& h, double v) noexcept
{
    if (v == 0.0)
        v = 0.0; // normalise -0.0
    mix(h, std::bit_cast<std::uint64_t>(v));
}

} // namespace

const char* to_string(strategy_status status) noexcept
{
    switch (status)
    {
    case strategy_status::ok:                 return "OK";
    case strategy_status::not_configured:     return "NOT_CONFIGURED";
    case strategy_status::invalid_instrument: return "INVALID_INSTRUMENT";
    }
    return "UNKNOWN";
}

std::uint64_t decision_hash(const quote_decision& decision) noexcept
{
    std::uint64_t h = fnv_offset;

    mix(h, static_cast<std::uint64_t>(decision.state));
    mix(h, static_cast<std::uint64_t>(decision.fair_value.raw()));
    mix(h, static_cast<std::uint64_t>(decision.reservation_price.raw()));
    mix_f64(h, decision.target_half_spread_bps);
    mix_f64(h, decision.inventory_utilization);
    mix(h, static_cast<std::uint64_t>(decision.decision_time_ns));
    mix(h, decision.market_snapshot_id);
    mix(h, static_cast<std::uint64_t>(decision.market_age_ns));
    mix(h, static_cast<std::uint64_t>(decision.bid_size));
    mix(h, static_cast<std::uint64_t>(decision.ask_size));
    mix(h, decision.requote ? 1ULL : 0ULL);
    mix(h, decision.cancel_resting_quotes ? 1ULL : 0ULL);

    mix(h, decision.intents.size());
    for (std::size_t i = 0; i < decision.intents.size(); ++i)
    {
        const auto& q = decision.intents[i];
        mix(h, static_cast<std::uint64_t>(q.side));
        mix(h, static_cast<std::uint64_t>(q.price.raw()));
        mix(h, static_cast<std::uint64_t>(q.quantity));
        mix(h, static_cast<std::uint64_t>(q.level));
        mix(h, q.post_only ? 1ULL : 0ULL);
    }

    mix(h, decision.reasons.size());
    for (std::size_t i = 0; i < decision.reasons.size(); ++i)
        mix(h, static_cast<std::uint64_t>(decision.reasons[i]));

    return h;
}

} // namespace truetest::mm
