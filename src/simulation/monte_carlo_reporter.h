#pragma once

#include "monte_carlo_types.h"

#include <string>

namespace truetest::simulation {

/**
 * Produces human and machine readable summaries from an McAggregate.
 * Phase 2 version is deliberately simple (text + basic JSON).
 */
class MonteCarloReporter {
public:
    static std::string render_text_summary(const McAggregate& agg,
                                           const McRunConfig& cfg);

    static std::string render_json(const McAggregate& agg,
                                   const McRunConfig& cfg);
};

} // namespace truetest::simulation
