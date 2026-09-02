#pragma once

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>

class IProvider;
class IKillSwitch;

enum class live_shutdown_reason
{
    normal_end,
    operator_kill,
    engine_halt,
    construction_failure
};

struct live_shutdown_report
{
    // Distinguishes a session that never owned an open provider (a valid
    // no-op) from an attempted shutdown whose individual steps failed.
    bool shutdown_required = false;
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
    LiveSafetySession(std::shared_ptr<IProvider> provider, bool live,
                      std::chrono::milliseconds kill_deadline);
    ~LiveSafetySession();

    bool open_provider();
    bool set_kill_switch(std::shared_ptr<IKillSwitch> kill_switch) noexcept;
    live_shutdown_report shutdown_once(
        live_shutdown_reason reason,
        std::chrono::milliseconds deadline = std::chrono::milliseconds{0}) noexcept;

    bool is_open() const noexcept;
    bool owns_provider(const std::shared_ptr<IProvider>& provider) const noexcept;

private:
    enum class state { idle, in_progress, complete };
    bool kill_once(std::chrono::milliseconds deadline) noexcept;

    std::shared_ptr<IProvider> provider_;
    std::shared_ptr<IKillSwitch> kill_switch_;
    bool live_ = false;
    std::chrono::milliseconds kill_deadline_;

    mutable std::mutex mu_;
    std::condition_variable cv_;
    state open_state_ = state::idle;
    bool opened_ = false;
    state kill_state_ = state::idle;
    bool kill_result_ = false;
    state shutdown_state_ = state::idle;
    live_shutdown_report report_;
};
