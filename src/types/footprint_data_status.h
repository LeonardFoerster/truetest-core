#pragma once

#include <cstdint>

// footprint.md §2.2's data-status vocabulary - shared across the ingress
// (providers/footprint), aggregation/reconciliation (analytics/footprint),
// and desk (ui/desk) layers, none of which may depend on each other in the
// direction this enum would otherwise require. Lives in `types` (a leaf
// module every one of them may already depend on) rather than duplicated or
// housed in whichever layer defined it first.
//
// Camera state (FOLLOWING/DETACHED) is a desk (§2.3) concern, not this.
namespace truetest::footprint {

enum class data_status : std::uint8_t
{
    unavailable,
    backfilling,
    live,
    recovering,
    partial,
    stale,
    replay,
};

} // namespace truetest::footprint
