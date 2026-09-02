#pragma once

// ============================================================
// LIVE-SAFETY SURFACE — Phase 1 freeze (see prod.md)
// Any edit requires explicit two-person CCB review + 4 h
// mainnet shadow run on engine_shadow before merge.
// Authoritative path list: scripts/check-live-safety-freeze.sh
// ============================================================

// Live-mode safety capabilities. No-op implementations explicitly represent
// an unavailable capability and may only be composed with a path that cannot
// issue private exchange writes.

#include "execution/portfolio.h"

#include <chrono>
#include <memory>
#include <string>

class IReconciler
{
public:
    virtual ~IReconciler() = default;

    // Fail-closed default: a new implementation is unavailable until it
    // explicitly proves that its required transport dependencies exist.
    virtual bool is_operational() const noexcept { return false; }

    // Empty = pass; non-empty error = engine refuses to start.
    virtual std::string reconcile(const portfolio& local_view,
                                  double tolerance_bps) = 0;
};

class NoopReconciler : public IReconciler
{
public:
    bool is_operational() const noexcept override { return false; }
    std::string reconcile(const portfolio&, double) override
    {
        return "NoopReconciler: reconciliation capability is unavailable";
    }
};

class IKillSwitch
{
public:
    virtual ~IKillSwitch() = default;

    // Fail-closed default for the same reason as IReconciler.
    virtual bool is_operational() const noexcept { return false; }

    // Returns false if it bailed past `deadline` — operator must intervene.
    virtual bool cancel_all_and_flatten(std::chrono::milliseconds deadline) = 0;
};

class NoopKillSwitch : public IKillSwitch
{
public:
    bool is_operational() const noexcept override { return false; }
    bool cancel_all_and_flatten(std::chrono::milliseconds) override
    {
        return false;
    }
};

// Provider-declared ability of the configured execution path. Local paper and
// shadow simulation are `no_private_writes`; exchange demo/testnet/mainnet are
// all `exchange_writes`. Unknown is never interpreted as read-only when an
// execution adapter exists.
enum class private_execution_capability
{
    no_private_writes,
    exchange_writes,
    unknown
};

struct startup_safety_validation;

// Immutable evidence that the central cold-path validator observed both
// operational safety capabilities for an authorized private-write target.
// The public default is intentionally invalid; normal configuration cannot
// manufacture a valid value or toggle it after startup.
class WriteSafetyReadiness final
{
public:
    constexpr WriteSafetyReadiness() noexcept = default;

    [[nodiscard]] constexpr bool
    permits_private_exchange_writes() const noexcept
    {
        return validated_;
    }

private:
    explicit constexpr WriteSafetyReadiness(bool validated) noexcept
        : validated_(validated) {}

    bool validated_ = false;

    friend startup_safety_validation validate_startup_safety(
        bool, bool, bool, private_execution_capability,
        const IReconciler*, const IKillSwitch*);
};

struct startup_safety_validation
{
    bool accepted = false;
    WriteSafetyReadiness readiness;
    std::string error;
};

inline std::string validate_private_execution_scope(
    bool target_allows_private_exchange_writes,
    bool private_exchange_execution_requested,
    bool provider_has_execution,
    private_execution_capability capability)
{
    if (capability == private_execution_capability::unknown)
    {
        if (provider_has_execution || private_exchange_execution_requested)
            return "startup rejected: provider execution write capability is undeclared";
        return {};
    }

    if (capability == private_execution_capability::exchange_writes)
    {
        if (!target_allows_private_exchange_writes)
            return "startup rejected: target does not allow private exchange writes";
        if (!private_exchange_execution_requested)
            return "startup rejected: read-only execution selected a private write-capable provider";
        return {};
    }

    if (private_exchange_execution_requested)
        return "startup rejected: requested execution path cannot issue private exchange writes";
    return {};
}

// Central, allocation-tolerant cold-path validation. The target input must be
// supplied by the target-specific main TU from target_allows_live_orders(); it
// is deliberately not a CLI/config option.
inline startup_safety_validation validate_startup_safety(
    bool target_allows_private_exchange_writes,
    bool private_exchange_execution_requested,
    bool provider_has_execution,
    private_execution_capability capability,
    const IReconciler* reconciler,
    const IKillSwitch* kill_switch)
{
    startup_safety_validation result;
    result.error = validate_private_execution_scope(
        target_allows_private_exchange_writes,
        private_exchange_execution_requested,
        provider_has_execution,
        capability);
    if (!result.error.empty()) return result;

    if (capability == private_execution_capability::exchange_writes)
    {
        if (!reconciler || !reconciler->is_operational())
        {
            result.error =
                "startup rejected: write-capable execution requires an operational reconciler";
            return result;
        }
        if (!kill_switch || !kill_switch->is_operational())
        {
            result.error =
                "startup rejected: write-capable execution requires an operational kill switch";
            return result;
        }

        result.accepted = true;
        result.readiness = WriteSafetyReadiness{true};
        return result;
    }

    result.accepted = true;
    return result;
}
