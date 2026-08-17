#pragma once

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <limits>
#include <mutex>
#include <poll.h>
#include <sys/socket.h>

namespace provider_io
{

enum class poll_wait_result
{
    ready,
    timeout,
    error,
};

// Publishes only the native descriptor across the transport's owner-thread /
// stop-thread boundary. request_stop() may call POSIX shutdown(2), which wakes
// synchronous and asynchronous reads without concurrently invoking methods on
// Boost.Asio's non-thread-safe socket object. The owner must clear the latch
// before closing or replacing the socket so a recycled descriptor is never
// interrupted accidentally.
class native_socket_interrupt
{
public:
    void publish(int fd) noexcept
    {
        std::lock_guard<std::mutex> lock(mu_);
        fd_ = fd;
    }

    void clear() noexcept
    {
        std::lock_guard<std::mutex> lock(mu_);
        fd_ = -1;
    }

    bool request_shutdown() noexcept
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (fd_ < 0) return false;
        for (;;)
        {
            if (::shutdown(fd_, SHUT_RDWR) == 0 || errno == ENOTCONN)
                return true;
            if (errno != EINTR) return false;
        }
    }

private:
    std::mutex mu_;
    int fd_ = -1;
};

// Cold blocking-I/O readiness seam used by live public transports. It bounds
// silence before a websocket read begins; the websocket implementation remains
// responsible for reporting a partial-frame/read error.
inline poll_wait_result poll_fd_readable(
    int fd, std::chrono::milliseconds timeout) noexcept
{
    if (fd < 0) return poll_wait_result::error;

    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;
    const auto deadline = std::chrono::steady_clock::now()
                        + std::max(timeout, std::chrono::milliseconds::zero());

    for (;;)
    {
        const auto now = std::chrono::steady_clock::now();
        const auto remaining = now >= deadline
            ? std::chrono::milliseconds::zero()
            : std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
        const auto bounded_ms = std::min<std::chrono::milliseconds::rep>(
            remaining.count(), std::numeric_limits<int>::max());
        const int ms = static_cast<int>(bounded_ms);
        const int rc = ::poll(&pfd, 1, ms);
        if (rc < 0)
        {
            if (errno == EINTR) continue;
            return poll_wait_result::error;
        }
        if (rc == 0) return poll_wait_result::timeout;
        if (pfd.revents & (POLLERR | POLLNVAL))
            return poll_wait_result::error;
        if (pfd.revents & (POLLIN | POLLHUP))
            return poll_wait_result::ready;
        return poll_wait_result::error;
    }
}

} // namespace provider_io
