#pragma once

#include "core/event.h"
#include "execution/execution_adapter.h"
#include "providers/provider.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

struct engine_config;
class OrderbookRegistry;

// Skeleton-local redeclaration of the cancel-metadata type to match the
// exact design signature using a bare name (hoisting / dedup happens in
// later integration PRs).
//
// See core/docs/internal/engine-decomposition.md + engine-decomposition skill for router extraction history
// and Phase 2 history. This remains a partial seam: async submit-result
// draining and exchange-shadow dual submission still live in engine.
//
// NOTE: order attribution metadata (former `struct order_meta` here) moved
// to order_attribution_store.h as of the OrderIntentProcessor preparatory
// extraction. pending_cancel_meta below is now owned outright by
// OrderIntentProcessor (Phase 3) — ExecutionRouter never read its own
// former reference to the map (stored but unused in every method body,
// same dead-reference pattern order_meta_ had; see
// tests/test_execution_router_characterization.cpp history), so that
// parameter was removed here rather than repointed. The struct definition
// stays in this header (OrderIntentProcessor already includes it for the
// ExecutionRouter& type) rather than being relocated for a one-line saving.
struct pending_cancel_meta
{
    std::string symbol;
    std::string reason;
};

class ExecutionRouter final
{
public:
    explicit ExecutionRouter(
        OrderbookRegistry& ob_reg,
        const engine_config& cfg,
        std::unordered_set<std::string>& l2_seeded,
        IProvider* provider, // may be null
        std::unordered_map<std::string, std::shared_ptr<IExecutionAdapter>>& adapters_ref
    );

    std::shared_ptr<IExecutionAdapter> resolve_adapter(const std::string& symbol) noexcept;
    bool is_async_submit(IExecutionAdapter* a) const noexcept;
    void submit(const order_event& o, IExecutionAdapter* a) noexcept;
    void drain_submit_results(IExecutionAdapter* a) noexcept;
    bool poll_fills(IExecutionAdapter* a, std::vector<fill_event>& out) noexcept;
    void submit_to_exchange_shadow(const order_event& o) noexcept;

    // Adapter map iteration moved fully into router (final cleanup).
    // Also forwards to provider's execution adapter when present (for live/shadow).
    void advance_all(std::chrono::system_clock::time_point ts) noexcept;
    void on_l2_snapshot(const std::string& symbol,
                        const std::vector<std::pair<double, double>>& bids,
                        const std::vector<std::pair<double, double>>& asks,
                        std::chrono::system_clock::time_point event_ts) noexcept;
    void on_l2_update(const std::string& symbol, order_side os,
                      double price, double new_qty,
                      std::chrono::system_clock::time_point event_ts) noexcept;

private:
    // Backing map is the engine's execution_adapters_ (passed by ref). Router owns creation/lookup logic.
    std::unordered_map<std::string, std::shared_ptr<IExecutionAdapter>>& adapters_;

    // Injected engine-owned state (refs/pointers; no ownership, no copies on hot path)
    OrderbookRegistry& ob_reg_;
    const engine_config& cfg_;
    std::unordered_set<std::string>& l2_seeded_;
    IProvider* provider_;
};
