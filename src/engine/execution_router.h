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

// Skeleton-local redeclarations of meta types to match the exact design
// signatures using bare names. These mirror the nested structs currently
// private in engine (hoisting / dedup happens in later integration PRs).
//
// See core/docs/internal/engine-decomposition.md + engine-decomposition skill for router extraction history
// and Phase 2 history. This remains a partial seam: async submit-result
// draining and exchange-shadow dual submission still live in engine.
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

    // These methods cross the provider adapter port.  The port deliberately
    // does not promise noexcept (venue transports may report a synchronous
    // local failure), so do not turn a recoverable engine-side safety halt
    // into std::terminate here.  The engine owns the catch/latch boundary.
    std::shared_ptr<IExecutionAdapter> resolve_adapter(const std::string& symbol);
    bool is_async_submit(IExecutionAdapter* a) const;
    void submit(const order_event& o, IExecutionAdapter* a);
    void drain_submit_results(IExecutionAdapter* a) noexcept;
    bool poll_fills(IExecutionAdapter* a, std::vector<fill_event>& out);
    void submit_to_exchange_shadow(const order_event& o) noexcept;

    // Adapter map iteration moved fully into router (final cleanup).
    // Also forwards to provider's execution adapter when present (for live/shadow).
    void advance_all(std::chrono::system_clock::time_point ts);
    void on_l2_snapshot(const std::string& symbol,
                        const std::vector<std::pair<double, double>>& bids,
                        const std::vector<std::pair<double, double>>& asks);
    void on_l2_update(const std::string& symbol, order_side os,
                      double price, double new_qty);

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
