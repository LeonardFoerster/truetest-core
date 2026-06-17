#include "maintenance_margin_table.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <limits>

namespace truetest::risk {

void MaintenanceMarginTable::load_from_leverage_bracket_json(const std::string& body)
{
    tiers_.clear();

    try {
        auto j = nlohmann::json::parse(body);

        // Binance returns an array when no symbol is given, or an object when symbol= is used.
        // We handle both the common "symbol-specific" object and the array case.
        nlohmann::json brackets;

        if (j.is_array() && !j.empty()) {
            // Take the first entry (we only asked for one symbol anyway)
            brackets = j[0]["brackets"];
        } else if (j.contains("brackets")) {
            brackets = j["brackets"];
        } else {
            // Unexpected format - leave empty
            return;
        }

        for (const auto& b : brackets) {
            MarginTier tier;
            tier.notional_cap            = b.value("notionalCap", std::numeric_limits<double>::max());
            tier.maintenance_margin_rate = b.value("maintMarginRatio", 0.0);
            tier.maint_amount            = b.value("cum", 0.0);   // cumulative maintenance amount

            tiers_.push_back(tier);
        }

        // Ensure sorted by notional_cap ascending (Binance usually sends them in order)
        std::sort(tiers_.begin(), tiers_.end(),
                  [](const MarginTier& a, const MarginTier& b) {
                      return a.notional_cap < b.notional_cap;
                  });
    }
    catch (...) {
        // Parsing failed - leave table empty. Caller can decide what to do.
        tiers_.clear();
    }
}

double MaintenanceMarginTable::maintenance_margin_rate_for_notional(double notional) const
{
    if (tiers_.empty()) {
        return 0.005; // safe fallback (original default)
    }

    for (const auto& t : tiers_) {
        if (notional <= t.notional_cap) {
            return t.maintenance_margin_rate;
        }
    }
    // Beyond last tier - use the highest tier's rate
    return tiers_.back().maintenance_margin_rate;
}

double MaintenanceMarginTable::maint_amount_for_notional(double notional) const
{
    if (tiers_.empty()) {
        return 0.0;
    }

    for (const auto& t : tiers_) {
        if (notional <= t.notional_cap) {
            return t.maint_amount;
        }
    }
    return tiers_.back().maint_amount;
}

} // namespace truetest::risk
