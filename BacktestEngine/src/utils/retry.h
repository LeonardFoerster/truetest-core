#pragma once

// Unified retry-with-exponential-backoff utility.
//
// Usage:
//   auto result = retry_with_backoff(
//       []() -> bool { return try_connect(); },
//       5,                          // max_attempts
//       std::chrono::seconds(1),    // initial_delay
//       std::chrono::seconds(30)    // max_delay
//   );
//
// The callable should return true on success, false on failure.
// Between failed attempts the thread sleeps with exponential backoff.
// Returns true if any attempt succeeded, false if all attempts exhausted.
//
// An optional on_retry callback receives the attempt number and exception (if
// any) for logging purposes.

#include <chrono>
#include <exception>
#include <functional>
#include <thread>

struct retry_config
{
    unsigned max_attempts = 5;
    std::chrono::milliseconds initial_delay{1000};
    std::chrono::milliseconds max_delay{30000};

    // Optional: called before each retry sleep with (attempt, exception_ptr).
    // exception_ptr is non-null only if the callable threw.
    std::function<void(unsigned, std::exception_ptr)> on_retry;
};

// Retries a callable that returns bool (true = success).
// If the callable throws, the attempt counts as a failure and the exception
// is forwarded to on_retry (if set). After all attempts are exhausted the
// last exception is rethrown.
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

        // Exponential backoff with cap
        delay = std::min(delay * 2, cfg.max_delay);
    }

    if (last_ex)
        std::rethrow_exception(last_ex);

    return false;
}

// Convenience overload matching the pattern in the task description.
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
