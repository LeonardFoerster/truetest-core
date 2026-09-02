#include "live_safety_session.h"

#include "providers/provider.h"

#include <iostream>

LiveSafetySession::LiveSafetySession(
    std::shared_ptr<IProvider> provider,
    live_safety_requirements requirements,
    std::chrono::milliseconds kill_deadline)
    : provider_(std::move(provider)),
      requirements_(std::move(requirements)),
      live_(requirements_.private_exchange_execution_requested),
      kill_deadline_(kill_deadline)
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
    }

    const auto reject_startup = [this](std::string error) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            startup_error_ = std::move(error);
            open_state_ = state::complete;
        }
        cv_.notify_all();
        return false;
    };

    private_execution_capability capability =
        private_execution_capability::unknown;
    std::shared_ptr<IReconciler> reconciler;
    std::shared_ptr<IKillSwitch> kill_switch;
    startup_safety_validation validation;
    try
    {
        const bool provider_has_execution = provider_->has_execution();
        capability = provider_->private_execution_capability_level();
        auto scope_error = validate_private_execution_scope(
            requirements_.target_allows_private_exchange_writes,
            requirements_.private_exchange_execution_requested,
            provider_has_execution, capability);
        if (!scope_error.empty())
            return reject_startup(std::move(scope_error));

        if (capability == private_execution_capability::exchange_writes
            && !provider_->prepare_write_safety())
        {
            return reject_startup(
                "startup rejected: provider could not prepare operational safety components");
        }

        // An explicit object is authoritative, including an invalid one.
        // Falling back after a bad override would recreate R5 fail-open.
        reconciler = requirements_.reconciler
            ? requirements_.reconciler
            : provider_->get_reconciler();
        kill_switch = requirements_.kill_switch
            ? requirements_.kill_switch
            : provider_->get_kill_switch();
        validation = validate_startup_safety(
            requirements_.target_allows_private_exchange_writes,
            requirements_.private_exchange_execution_requested,
            provider_has_execution, capability,
            reconciler.get(), kill_switch.get());
        if (!validation.accepted)
            return reject_startup(std::move(validation.error));

        if (capability == private_execution_capability::exchange_writes
            && !provider_->install_write_safety_readiness(validation.readiness))
        {
            return reject_startup(
                "startup rejected: provider refused validated write-safety readiness");
        }
    }
    catch (...)
    {
        return reject_startup(
            "startup rejected: provider safety preflight threw an exception");
    }

    {
        std::lock_guard<std::mutex> lock(mu_);
        reconciler_ = std::move(reconciler);
        kill_switch_ = std::move(kill_switch);
        write_safety_readiness_ = validation.readiness;
        startup_safety_validated_ = true;
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
        provider_open_succeeded_ = ok;
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
        if (kill && kill->is_operational())
            ok = kill->cancel_all_and_flatten(
                deadline.count() > 0 ? deadline : kill_deadline_);
        else
            std::cerr << "live safety: operational kill switch unavailable\n";
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
    if (open_state_ != state::idle || kill_state_ != state::idle) return false;
    requirements_.kill_switch = std::move(kill_switch);
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
    provider_open_succeeded_ = false;
    shutdown_state_ = state::complete;
    auto report = report_;
    lock.unlock();
    cv_.notify_all();
    return report;
}

bool LiveSafetySession::is_open() const noexcept
{
    std::lock_guard<std::mutex> lock(mu_);
    return opened_ && provider_open_succeeded_
        && open_state_ == state::complete;
}

bool LiveSafetySession::owns_provider(
    const std::shared_ptr<IProvider>& provider) const noexcept
{
    return provider_ && provider_ == provider;
}

bool LiveSafetySession::startup_safety_validated() const noexcept
{
    std::lock_guard<std::mutex> lock(mu_);
    return startup_safety_validated_;
}

bool LiveSafetySession::permits_private_exchange_writes() const noexcept
{
    std::lock_guard<std::mutex> lock(mu_);
    return write_safety_readiness_.permits_private_exchange_writes();
}

bool LiveSafetySession::configured_safety_matches(
    const std::shared_ptr<IReconciler>& reconciler,
    const std::shared_ptr<IKillSwitch>& kill_switch) const noexcept
{
    std::lock_guard<std::mutex> lock(mu_);
    return (!reconciler || reconciler == reconciler_)
        && (!kill_switch || kill_switch == kill_switch_);
}

std::string LiveSafetySession::startup_error() const
{
    std::lock_guard<std::mutex> lock(mu_);
    return startup_error_;
}

std::string LiveSafetySession::reconcile(const portfolio& local_view,
                                         double tolerance_bps)
{
    std::shared_ptr<IReconciler> reconciler;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!startup_safety_validated_)
            return "startup rejected: write safety has not been validated";
        reconciler = reconciler_;
    }
    if (!reconciler || !reconciler->is_operational())
        return "startup rejected: operational reconciler is unavailable";
    return reconciler->reconcile(local_view, tolerance_bps);
}
