#pragma once

#include "engine_config.h"
#include "orderbook/orderbook_registry.h"
#include "providers/provider.h"
#include "execution/execution_adapter.h"
#include "core/event.h"

#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <string>
#include <vector>

// Skeleton for PR-02. Full logic moved in later PRs.
// All hot-path entry points are final/noexcept where possible, no alloc on cached path.

class ExecutionRouter final {
public:
    // Ctor collaborators passed by ref; no ownership of engine state except adapters map.
    explicit ExecutionRouter(
        OrderbookRegistry& ob_reg,
        const engine_config& cfg,
        std::unordered_set<std::string>& l2_seeded,
        IProvider* provider   // may be null
        // pending refs for later full ownership move (PR-05)
    );

    // Hot-path entry points: final + cheap. No heap, no string construction on repeated calls.
    // For hot symbols, adapters are pre-cached at construction / prewarm time.
    std::shared_ptr<IExecutionAdapter> resolve_adapter(const std::string& symbol) noexcept;
    bool is_async_submit(IExecutionAdapter* a) const noexcept;
    void submit(const order_event& o, IExecutionAdapter* a) noexcept;
    void drain_submit_results(IExecutionAdapter* a) noexcept;  // full previous drain_async logic
    bool poll_fills(IExecutionAdapter* a, std::vector<fill_event>& out) noexcept;
    void submit_to_exchange_shadow(const order_event& o) noexcept;

private:
    OrderbookRegistry* ob_reg_ = nullptr;
    const engine_config* cfg_ = nullptr;
    std::unordered_set<std::string>* l2_seeded_ = nullptr;
    IProvider* provider_ = nullptr;

    std::unordered_map<std::string, std::shared_ptr<IExecutionAdapter>> adapters_;
};