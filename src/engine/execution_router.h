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
#include <cstdint>

// Skeleton-local redeclarations of meta types to match the exact design
// signatures using bare names. These mirror the nested structs currently
// private in engine (hoisting / dedup happens in later integration PRs).
struct pending_cancel_meta
{
    std::string symbol;
    std::string reason;
};

struct order_meta
{
    uint64_t opener_order_id = 0;
    std::string strategy_name;
};

class ExecutionRouter final
{
public:
    explicit ExecutionRouter(
        OrderbookRegistry& ob_reg,
        const engine_config& cfg,
        std::unordered_set<std::string>& l2_seeded,
        IProvider* provider, // may be null
        std::unordered_map<uint64_t, pending_cancel_meta>& pending_cancels_ref,
        std::unordered_map<uint64_t, order_meta>& order_meta_ref,
        std::unordered_map<std::string, std::shared_ptr<IExecutionAdapter>>& adapters_ref
    );

    std::shared_ptr<IExecutionAdapter> resolve_adapter(const std::string& symbol) noexcept;
    bool is_async_submit(IExecutionAdapter* a) const noexcept;
    void submit(const order_event& o, IExecutionAdapter* a) noexcept;
    void drain_submit_results(IExecutionAdapter* a) noexcept;
    bool poll_fills(IExecutionAdapter* a, std::vector<fill_event>& out) noexcept;
    void submit_to_exchange_shadow(const order_event& o) noexcept;

private:
    // Backing map is the engine's execution_adapters_ (passed by ref). Router owns creation/lookup logic.
    std::unordered_map<std::string, std::shared_ptr<IExecutionAdapter>>& adapters_;

    // Injected engine-owned state (refs/pointers; no ownership, no copies on hot path)
    OrderbookRegistry& ob_reg_;
    const engine_config& cfg_;
    std::unordered_set<std::string>& l2_seeded_;
    IProvider* provider_;
    std::unordered_map<uint64_t, pending_cancel_meta>& pending_cancels_;
    std::unordered_map<uint64_t, order_meta>& order_meta_;
};
