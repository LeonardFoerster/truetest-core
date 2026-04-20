#pragma once

#include <chrono>
#include <exception>
#include <functional>
#include <thread>

struct retry_config
{
    unsigned max_attempts = 5;
    std::chrono::milliseconds initial_delay{1000};
    std::chrono::milliseconds max_delay{30000};

    std::function<void(unsigned, std::exception_ptr)> on_retry;
};

template <typename Callable>
bool retry_with_backoff(Callable&& fn, const retry_config& cfg)
{
    auto delay = cfg.initial_delay;
    std::exception_ptr last_ex;

    for (unsigned attempt = 1; attempt <= cfg.max_attempts; ++attempt)
    {
        try
        {
            if (fn())
                return true;
            last_ex = nullptr;
        }
        catch (...)
        {
            last_ex = std::current_exception();
        }

        if (attempt == cfg.max_attempts)
            break;

        if (cfg.on_retry)
            cfg.on_retry(attempt, last_ex);

        std::this_thread::sleep_for(delay);

        delay = std::min(delay * 2, cfg.max_delay);
    }

    if (last_ex)
        std::rethrow_exception(last_ex);

    return false;
}

template <typename Callable>
bool retry_with_backoff(Callable&& fn,
                        unsigned max_attempts,
                        std::chrono::milliseconds initial_delay,
                        std::chrono::milliseconds max_delay)
{
    retry_config cfg;
    cfg.max_attempts = max_attempts;
    cfg.initial_delay = initial_delay;
    cfg.max_delay = max_delay;
    return retry_with_backoff(std::forward<Callable>(fn), cfg);
}
