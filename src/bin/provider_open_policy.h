#pragma once

#include "engine/live_safety_session.h"
#include "providers/provider.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <exception>
#include <functional>
#include <memory>
#include <ostream>
#include <thread>
#include <utility>

namespace truetest::bin {

// The process signal handler may only publish this sig_atomic_t notification.
// A normal C++ thread owns the bridge lifetime and performs the virtual stop.
inline volatile std::sig_atomic_t shutdown_signal_requested = 0;

inline void mark_shutdown_signal(int) noexcept
{
    shutdown_signal_requested = 1;
}

inline void reset_shutdown_signal() noexcept
{
    shutdown_signal_requested = 0;
}

template <typename Bridge>
class bridge_shutdown_monitor
{
public:
    explicit bridge_shutdown_monitor(std::shared_ptr<Bridge> bridge)
        : bridge_(std::move(bridge))
        , monitor_([bridge = bridge_](std::stop_token stop) {
            while (!stop.stop_requested())
            {
                if (shutdown_signal_requested != 0)
                {
                    try { bridge->stop(); }
                    catch (...) {}
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
        })
    {}

    ~bridge_shutdown_monitor() { monitor_.request_stop(); }

    bridge_shutdown_monitor(const bridge_shutdown_monitor&) = delete;
    bridge_shutdown_monitor& operator=(const bridge_shutdown_monitor&) = delete;

private:
    std::shared_ptr<Bridge> bridge_;
    std::jthread monitor_;
};

inline bool provider_open_required(IProvider& provider)
{
    return provider.has_data_feed() || provider.has_execution();
}

inline bool open_provider_if_required(
    IProvider& provider, LiveSafetySession& live_safety_session)
{
    if (shutdown_signal_requested != 0) return false;
    if (!provider_open_required(provider)) return true;
    const bool opened = live_safety_session.open_provider();
    if (shutdown_signal_requested == 0) return opened;
    if (opened)
        (void)live_safety_session.shutdown_once(
            live_shutdown_reason::operator_kill);
    return false;
}

template <typename Fn>
int run_provider_session_guarded(
    LiveSafetySession& live_safety_session,
    Fn&& fn,
    std::ostream& errors) noexcept
{
    try
    {
        return std::invoke(std::forward<Fn>(fn));
    }
    catch (const std::exception& e)
    {
        errors << "  ! Provider mode failed after open: " << e.what() << '\n';
    }
    catch (...)
    {
        errors << "  ! Provider mode failed after open with an unknown exception\n";
    }

    const auto report = live_safety_session.shutdown_once(
        live_shutdown_reason::construction_failure);
    errors << "  ! Fail-closed shutdown: kill_attempted="
           << (report.kill_attempted ? "yes" : "no")
           << " kill_succeeded=" << (report.kill_succeeded ? "yes" : "no")
           << " provider_closed=" << (report.provider_closed ? "yes" : "no")
           << '\n';
    return 1;
}

} // namespace truetest::bin
