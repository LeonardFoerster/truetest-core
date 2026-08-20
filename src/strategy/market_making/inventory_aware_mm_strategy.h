#pragma once

#include "mm_config.h"
#include "mm_strategy.h"
#include "mm_telemetry.h"

#include <cstdint>
#include <string_view>

namespace truetest::mm
{

// Inventory-aware market-making strategy (risk register R1).
//
// Pipeline position:
//   canonical market state + authoritative inventory
//     -> InventoryAwareMarketMakingStrategy::evaluate()
//     -> quote_decision / quote_intent
//     -> pre-trade risk -> matcher | shadow -> fills -> portfolio -> back here
//
// It owns none of that pipeline. It does not generate counterparty liquidity
// (that is src/market_maker/, a simulation component), does not decide queue
// position or fills, and does not submit anything anywhere.
//
// Hot-path contract: evaluate() is noexcept, performs no allocation, takes no
// lock, touches no global mutable state, and reads no clock — all time comes
// from the snapshots and the context.
class InventoryAwareMarketMakingStrategy final : public IMarketMakingStrategy
{
public:
    static constexpr std::uint32_t version = 1;

    InventoryAwareMarketMakingStrategy() = default;

    // Startup only. Returns the precise validation failure; the caller is
    // expected to fail fast. Until this succeeds, evaluate() returns
    // not_configured with a PAUSED, empty decision.
    [[nodiscard]] mm_config_status configure(const mm_config& cfg) noexcept;

    [[nodiscard]] bool configured() const noexcept { return configured_; }
    [[nodiscard]] const mm_config& config() const noexcept { return cfg_; }

    // Optional, injected — never a global. Safe to leave null.
    void set_decision_sink(IMMDecisionSink* sink) noexcept { sink_ = sink; }

    [[nodiscard]] strategy_result evaluate(
        const market_snapshot& market,
        const inventory_snapshot& inventory,
        const strategy_context& context) noexcept override;

    [[nodiscard]] std::string_view strategy_id() const noexcept override
    {
        return cfg_.strategy_id;
    }
    [[nodiscard]] std::uint32_t strategy_version() const noexcept override
    {
        return version;
    }
    [[nodiscard]] std::uint64_t strategy_config_hash() const noexcept override
    {
        return config_hash_;
    }

private:
    void publish(const quote_decision& decision,
                 const inventory_snapshot& inventory,
                 const strategy_context& context) const noexcept;

    mm_config cfg_{};
    std::uint64_t config_hash_ = 0;
    bool configured_ = false;
    IMMDecisionSink* sink_ = nullptr;
};

} // namespace truetest::mm
