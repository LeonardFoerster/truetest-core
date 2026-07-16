#include "execution_router.h"

#include <iostream>

ExecutionRouter::ExecutionRouter(
    OrderbookRegistry& ob_reg,
    const engine_config& cfg,
    std::unordered_set<std::string>& l2_seeded,
    IProvider* provider
)
    : ob_reg_(&ob_reg)
    , cfg_(&cfg)
    , l2_seeded_(&l2_seeded)
    , provider_(provider)
{
    // Skeleton: adapters populated in later PRs when logic is moved.
    // Pre-warm or ctor can create initial adapters for known symbols.
}

std::shared_ptr<IExecutionAdapter> ExecutionRouter::resolve_adapter(const std::string& symbol) noexcept {
    auto it = adapters_.find(symbol);
    if (it != adapters_.end()) {
        return it->second;
    }
    // Creation logic will be moved here in later PR.
    // For skeleton, return nullptr or basic (real impl in PR-05).
    return nullptr;
}

bool ExecutionRouter::is_async_submit(IExecutionAdapter* a) const noexcept {
    // Will detect ExecutionBridge etc in later PR.
    return false; // skeleton
}

void ExecutionRouter::submit(const order_event& o, IExecutionAdapter* a) noexcept {
    if (a) {
        // Full submit moved later.
    }
}

void ExecutionRouter::drain_submit_results(IExecutionAdapter* a) noexcept {
    if (a) {
        // Full drain logic moved from engine later.
    }
}

bool ExecutionRouter::poll_fills(IExecutionAdapter* a, std::vector<fill_event>& out) noexcept {
    if (a) {
        // poll moved later.
        return false;
    }
    return false;
}

void ExecutionRouter::submit_to_exchange_shadow(const order_event& o) noexcept {
    // Shadow dual moved later.
}