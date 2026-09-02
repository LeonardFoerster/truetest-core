#pragma once

#include "execution/live_safety.h"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>

class IProvider;

struct live_safety_requirements
{
    // The first value must come from target_allows_live_orders() in the
    // target-specific composition root, never from runtime configuration.
    bool target_allows_private_exchange_writes = false;
    bool private_exchange_execution_requested = false;
    std::shared_ptr<IReconciler> reconciler;
    std::shared_ptr<IKillSwitch> kill_switch;
};

enum class live_shutdown_reason
{
    normal_end,
    operator_kill,
    engine_halt,
    construction_failure
};

struct live_shutdown_report
{
    bool quiesce_succeeded = false;
    bool kill_attempted = false;
    bool kill_succeeded = false;
    bool provider_closed = false;
};

// Cold-path owner for a provider's live lifetime. All callers share one
// completed remote kill and one completed provider shutdown result.
class LiveSafetySession final
{
public:
    LiveSafetySession(std::shared_ptr<IProvider> provider,
                      live_safety_requirements requirements,
                      std::chrono::milliseconds kill_deadline);
    ~LiveSafetySession();

    bool open_provider();
    bool set_kill_switch(std::shared_ptr<IKillSwitch> kill_switch) noexcept;
    live_shutdown_report shutdown_once(
        live_shutdown_reason reason,
        std::chrono::milliseconds deadline = std::chrono::milliseconds{0}) noexcept;

    bool is_open() const noexcept;
    bool owns_provider(const std::shared_ptr<IProvider>& provider) const noexcept;
    bool startup_safety_validated() const noexcept;
    bool permits_private_exchange_writes() const noexcept;
    bool configured_safety_matches(
        const std::shared_ptr<IReconciler>& reconciler,
        const std::shared_ptr<IKillSwitch>& kill_switch) const noexcept;
    std::string startup_error() const;
    std::string reconcile(const portfolio& local_view, double tolerance_bps);

private:
    enum class state { idle, in_progress, complete };
    bool kill_once(std::chrono::milliseconds deadline) noexcept;

    std::shared_ptr<IProvider> provider_;
    live_safety_requirements requirements_;
    std::shared_ptr<IReconciler> reconciler_;
    std::shared_ptr<IKillSwitch> kill_switch_;
    WriteSafetyReadiness write_safety_readiness_;
    bool live_ = false;
    std::chrono::milliseconds kill_deadline_;

    mutable std::mutex mu_;
    std::condition_variable cv_;
    state open_state_ = state::idle;
    bool opened_ = false;
    bool provider_open_succeeded_ = false;
    bool startup_safety_validated_ = false;
    std::string startup_error_;
    state kill_state_ = state::idle;
    bool kill_result_ = false;
    state shutdown_state_ = state::idle;
    live_shutdown_report report_;
};
