#pragma once

#include "simulation/monte_carlo_types.h"

#include <string_view>

namespace truetest::simulation {

inline constexpr std::string_view kMonteCarloFloatingPointReduction =
    "stable-trial-index-compensated-v1";

// Validate and reduce complete trial results. Invalid or internally
// contradictory economic results are rejected; they are never converted into
// plausible campaign statistics.
void summarize_monte_carlo_trials(McAggregate& aggregate);

} // namespace truetest::simulation
