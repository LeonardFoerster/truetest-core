#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

// Pure, stateless suggestion utilities for footprint.md §2.2's two "suggest
// once, then save until manually changed" defaults. Neither function saves
// anything - persistence is a §2.3 desk-settings concern (out of scope
// here); these are just the math.
namespace truetest::footprint {

// "Suggest the first quote-volume threshold from median recent one-minute
// quote volume divided by six" (§2.2). Returns 0.0 for an empty input.
inline double suggest_quote_volume_threshold(
    std::vector<double> recent_one_minute_quote_volumes)
{
    if (recent_one_minute_quote_volumes.empty())
        return 0.0;

    std::sort(recent_one_minute_quote_volumes.begin(),
              recent_one_minute_quote_volumes.end());
    const std::size_t n = recent_one_minute_quote_volumes.size();
    const double median = (n % 2 == 1)
        ? recent_one_minute_quote_volumes[n / 2]
        : 0.5 * (recent_one_minute_quote_volumes[n / 2 - 1] +
                 recent_one_minute_quote_volumes[n / 2]);
    return median / 6.0;
}

// "Suggest the imbalance minimum from recent nonzero footprint-cell base
// volumes, rounded to valid quantity precision" (§2.2). The caller is
// responsible for filtering to nonzero volumes first - the function name
// says "nonzero_..." and trusts its input rather than re-filtering.
// qty_step_atoms <= 0 disables rounding (median returned as-is); otherwise
// the median is rounded DOWN to the nearest valid multiple, matching a
// venue's minimum-quantity increment.
inline std::int64_t suggest_imbalance_min_volume(
    std::vector<std::int64_t> nonzero_cell_base_volumes,
    std::int64_t qty_step_atoms = 0)
{
    if (nonzero_cell_base_volumes.empty())
        return 0;

    std::sort(nonzero_cell_base_volumes.begin(), nonzero_cell_base_volumes.end());
    const std::size_t n = nonzero_cell_base_volumes.size();
    const std::int64_t median = (n % 2 == 1)
        ? nonzero_cell_base_volumes[n / 2]
        : (nonzero_cell_base_volumes[n / 2 - 1] + nonzero_cell_base_volumes[n / 2]) / 2;

    if (qty_step_atoms <= 0)
        return median;
    return (median / qty_step_atoms) * qty_step_atoms;
}

} // namespace truetest::footprint
