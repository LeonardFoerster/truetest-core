#include "live_safety_session.h"

#include "providers/provider.h"

#include <iostream>

LiveSafetySession::LiveSafetySession(std::shared_ptr<IProvider> provider,
                                     bool live,
                                     std::chrono::milliseconds kill_deadline)
    : provider_(std::move(provider)), live_(live), kill_deadline_(kill_deadline)
{
}

LiveSafetySession::~LiveSafetySession()
{
    (void)shutdown_once(live_shutdown_reason::normal_end);
}

bool LiveSafetySession::open_provider()
{
    if (!provider_) return false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (open_state_ != state::idle || shutdown_state_ != state::idle)
            return false;
        open_state_ = state::in_progress;
        // Ownership starts before open(): a throwing provider may already
        // have armed venue safety or opened one of several transports.
        opened_ = true;
    }
    bool ok = false;
    try
    {
        ok = provider_->open();
    }
    catch (...)
    {
        std::cerr << "live safety: provider open threw; entering fail-closed shutdown\n";
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        open_state_ = state::complete;
    }
    cv_.notify_all();
    if (!ok)
        (void)shutdown_once(live_shutdown_reason::construction_failure);
    return ok;
}

bool LiveSafetySession::kill_once(std::chrono::milliseconds deadline) noexcept
{
    std::unique_lock<std::mutex> lock(mu_);
    while (kill_state_ == state::in_progress) cv_.wait(lock);
    if (kill_state_ == state::complete) return kill_result_;
    kill_state_ = state::in_progress;
    report_.kill_attempted = true;
    lock.unlock();

    bool ok = false;
    try
    {
        auto kill = kill_switch_;
        if (!kill && provider_) kill = provider_->get_kill_switch();
        if (kill)
            ok = kill->cancel_all_and_flatten(
                deadline.count() > 0 ? deadline : kill_deadline_);
        else
            std::cerr << "live safety: provider has no kill switch\n";
    }
    catch (...)
    {
        ok = false;
    }

    lock.lock();
    kill_result_ = ok;
    report_.kill_succeeded = ok;
    kill_state_ = state::complete;
    lock.unlock();
    cv_.notify_all();
    return ok;
}

bool LiveSafetySession::set_kill_switch(
    std::shared_ptr<IKillSwitch> kill_switch) noexcept
{
    std::lock_guard<std::mutex> lock(mu_);
    if (kill_state_ != state::idle) return false;
    kill_switch_ = std::move(kill_switch);
    return true;
}

live_shutdown_report LiveSafetySession::shutdown_once(
    live_shutdown_reason,
    std::chrono::milliseconds deadline) noexcept
{
    std::unique_lock<std::mutex> lock(mu_);
    while (open_state_ == state::in_progress) cv_.wait(lock);
    while (shutdown_state_ == state::in_progress) cv_.wait(lock);
    if (shutdown_state_ == state::complete) return report_;
    report_.shutdown_required = opened_;
    if (!opened_)
    {
        shutdown_state_ = state::complete;
        return report_;
    }
    shutdown_state_ = state::in_progress;
    lock.unlock();

    bool quiesce_ok = true;
    try { provider_->quiesce_for_live_shutdown(); }
    catch (...) { quiesce_ok = false; }
    report_.quiesce_succeeded = quiesce_ok;
    const bool kill_ok = !live_ || kill_once(deadline);
    try
    {
        provider_->finish_live_shutdown(
            (quiesce_ok && kill_ok)
                ? live_shutdown_disposition::disarm_after_kill
                : live_shutdown_disposition::preserve_dead_man_switch);
        report_.provider_closed = true;
    }
    catch (...)
    {
        report_.provider_closed = false;
    }

    lock.lock();
    opened_ = false;
    shutdown_state_ = state::complete;
    auto report = report_;
    lock.unlock();
    cv_.notify_all();
    return report;
}

bool LiveSafetySession::is_open() const noexcept
{
    std::lock_guard<std::mutex> lock(mu_);
    return opened_;
}

bool LiveSafetySession::owns_provider(
    const std::shared_ptr<IProvider>& provider) const noexcept
{
    return provider_ && provider_ == provider;
}
