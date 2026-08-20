#pragma once

#include "mm_fixed_vector.h"
#include "mm_strategy.h"

#include <cstdint>
#include <string_view>

namespace truetest::mm
{

// One decision's worth of structured telemetry. Everything a run record,
// QuestDB row, or dashboard needs, with no owned storage: `strategy_id`
// points into the strategy's configuration, which outlives the call.
struct mm_decision_record
{
    std::string_view strategy_id{};
    std::uint32_t strategy_version = 0;
    std::uint64_t config_hash = 0;

    timestamp_ns decision_time_ns = 0;
    std::uint64_t market_snapshot_id = 0;
    std::uint32_t symbol_id = 0;

    Price fair_value{};
    Price reservation_price{};

    qty_atoms inventory = 0;
    double inventory_utilization = 0.0;
    std::int64_t market_age_ns = 0;
    basis_points half_spread_bps = 0.0;

    qty_atoms bid_size = 0;
    qty_atoms ask_size = 0;
    std::uint8_t number_of_quote_intents = 0;

    mm_state state = mm_state::paused;
    fixed_vector<quote_reason, max_quote_reasons> reasons{};

    std::uint64_t decision_hash = 0;
};

// Cold-path consumer of decision telemetry.
//
// Called from evaluate() after the decision is complete. Implementations must
// be non-blocking — an SPSC ring push that drops on overflow, never a mutex,
// an ILP socket write, or a formatted string build. The strategy neither
// waits on the sink nor changes its decision if the sink is absent or
// saturated: a telemetry outage must not move quotes.
class IMMDecisionSink
{
public:
    virtual ~IMMDecisionSink() = default;
    virtual void on_decision(const mm_decision_record& record) noexcept = 0;
};

} // namespace truetest::mm
