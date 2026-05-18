#pragma once

#include <vector>
#include <string>

namespace truetest::risk {

/**
 * One tier from Binance's /fapi/v1/leverageBracket response.
 * See https://binance-docs.github.io/apidocs/futures/en/#notional-and-leverage-brackets
 */
struct MarginTier {
    double notional_cap;            // upper bound of this tier (inclusive)
    double maintenance_margin_rate; // e.g. 0.005 for 0.5%
    double maint_amount;            // maintenance amount (cumulative)
};

/**
 * Holds the notional-tiered maintenance margin schedule for one symbol.
 * Loaded once at provider open() from the exchange and then used read-only
 * in the hot path by FuturesRiskCheck.
 */
class MaintenanceMarginTable {
public:
    MaintenanceMarginTable() = default;

    /**
     * Parse the JSON body returned by GET /fapi/v1/leverageBracket?symbol=XXX
     * and populate the internal tiers (sorted by notional_cap ascending).
     */
    void load_from_leverage_bracket_json(const std::string& body);

    /**
     * Return the maintenance margin rate that applies for the given notional.
     * If the notional exceeds all defined tiers, returns the rate of the last tier.
     */
    double maintenance_margin_rate_for_notional(double notional) const;

    /**
     * Return the full maint amount (including cumulative) for the given notional.
     * Useful for more precise liquidation price calculations.
     */
    double maint_amount_for_notional(double notional) const;

    bool empty() const { return tiers_.empty(); }

private:
    std::vector<MarginTier> tiers_;   // kept sorted by notional_cap
};

} // namespace truetest::risk
