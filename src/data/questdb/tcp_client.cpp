#ifdef HAS_QUESTDB

#include "tcp_client.h"

#include <algorithm>
#include <arpa/inet.h>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <iostream>
#include <string>
#include <utility>

namespace truetest::questdb {

TcpClient::~TcpClient()
{
    close();
}

void TcpClient::close()
{
    if (fd_ >= 0)
    {
        ::close(fd_);
        fd_ = -1;
    }
}

namespace {

int wait_until(int fd, bool writable, TcpClient::Deadline deadline)
{
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) return 0;
    const auto left = std::chrono::duration_cast<std::chrono::microseconds>(
        deadline - now);
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    timeval tv{};
    tv.tv_sec = static_cast<time_t>(left.count() / 1'000'000);
    tv.tv_usec = static_cast<suseconds_t>(left.count() % 1'000'000);
    return writable
        ? ::select(fd + 1, nullptr, &fds, nullptr, &tv)
        : ::select(fd + 1, &fds, nullptr, nullptr, &tv);
}

}

bool TcpClient::connect(const std::string& host, std::uint16_t port,
                        int connect_timeout_ms)
{
    if (connect_timeout_ms <= 0) return false;
    return connect_until(host, port, std::chrono::steady_clock::now()
                         + std::chrono::milliseconds(connect_timeout_ms));
}

bool TcpClient::connect_until(const std::string& host, std::uint16_t port,
                              Deadline deadline)
{
    close();
    if (std::chrono::steady_clock::now() >= deadline) return false;

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* result = nullptr;
    const std::string port_str = std::to_string(port);
    const int gai_rc = ::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result);
    if (gai_rc != 0 || !result)
    {
        std::cerr << "[questdb] getaddrinfo(" << host << ":" << port
                  << ") failed: " << gai_strerror(gai_rc) << "\n";
        return false;
    }

    if (std::chrono::steady_clock::now() >= deadline)
    {
        ::freeaddrinfo(result);
        return false;
    }

    int last_err = 0;
    for (addrinfo* ai = result; ai != nullptr; ai = ai->ai_next)
    {
        if (std::chrono::steady_clock::now() >= deadline)
        {
            last_err = ETIMEDOUT;
            break;
        }
        const int sock = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock < 0)
        {
            last_err = errno;
            continue;
        }

        // Keep the socket non-blocking after connect: every later path uses
        // select() plus MSG_DONTWAIT under the same absolute deadline.
        int flags = -1;
        do
        {
            flags = ::fcntl(sock, F_GETFL, 0);
        } while (flags < 0 && errno == EINTR
                 && std::chrono::steady_clock::now() < deadline);
        if (flags < 0)
        {
            last_err = std::chrono::steady_clock::now() >= deadline ? ETIMEDOUT : errno;
            ::close(sock);
            continue;
        }
        int set_flags = -1;
        do
        {
            set_flags = ::fcntl(sock, F_SETFL, flags | O_NONBLOCK);
        } while (set_flags < 0 && errno == EINTR
                 && std::chrono::steady_clock::now() < deadline);
        if (set_flags < 0)
        {
            last_err = std::chrono::steady_clock::now() >= deadline ? ETIMEDOUT : errno;
            ::close(sock);
            continue;
        }

        const int rc = ::connect(sock, ai->ai_addr, ai->ai_addrlen);
        bool candidate_failed = false;
        if (rc < 0 && errno != EINPROGRESS && errno != EALREADY && errno != EINTR)
        {
            last_err = errno;
            candidate_failed = true;
        }

        if (!candidate_failed && rc < 0)
        {
            for (;;)
            {
                const int sel = wait_until(sock, true, deadline);
                if (sel == 0)
                {
                    last_err = ETIMEDOUT;
                    candidate_failed = true;
                    break;
                }
                if (sel < 0)
                {
                    if (errno == EINTR) continue;
                    last_err = errno;
                    candidate_failed = true;
                    break;
                }

                int so_err = 0;
                socklen_t so_len = sizeof(so_err);
                const int so_rc = ::getsockopt(
                    sock, SOL_SOCKET, SO_ERROR, &so_err, &so_len);
                if (so_rc < 0)
                {
                    if (errno == EINTR) continue;
                    last_err = errno;
                    candidate_failed = true;
                    break;
                }
                if (so_err == 0) break;
                if (so_err == EINPROGRESS || so_err == EALREADY || so_err == EINTR)
                    continue;
                last_err = so_err;
                candidate_failed = true;
                break;
            }
        }

        if (candidate_failed)
        {
            ::close(sock);
            continue;
        }

        if (std::chrono::steady_clock::now() >= deadline)
        {
            last_err = ETIMEDOUT;
            ::close(sock);
            break;
        }

        // TCP_NODELAY avoids batching delay for cold ILP/HTTP requests.
        const int one = 1;
        ::setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        fd_ = sock;
        ::freeaddrinfo(result);
        return true;
    }

    ::freeaddrinfo(result);
    std::cerr << "[questdb] connect(" << host << ":" << port
              << ") failed: " << std::strerror(last_err) << "\n";
    return false;
}

TcpClient::WriteAttempt TcpClient::write_attempt(
    std::string_view data, int write_timeout_ms)
{
    if (write_timeout_ms <= 0)
        return {WriteState::no_bytes_sent, 0};
    return write_attempt_until(data, std::chrono::steady_clock::now()
                               + std::chrono::milliseconds(write_timeout_ms));
}

TcpClient::WriteAttempt TcpClient::write_attempt_until(
    std::string_view data, Deadline deadline)
{
    if (fd_ < 0)
        return {WriteState::no_bytes_sent, 0};
    const char* p = data.data();
    std::size_t remaining = data.size();
    std::size_t bytes_sent = 0;
    const auto failed = [&] {
        return WriteAttempt{
            bytes_sent == 0 ? WriteState::no_bytes_sent
                            : WriteState::delivery_ambiguous,
            bytes_sent};
    };
    while (remaining > 0)
    {
        const int ready = wait_until(fd_, true, deadline);
        if (ready == 0) return failed();
        if (ready < 0)
        {
            if (errno == EINTR) continue;
            return failed();
        }

        const ssize_t n = ::send(
            fd_, p, remaining, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n < 0)
        {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            return failed();
        }
        if (n == 0) return failed();
        p += n;
        const auto sent = static_cast<std::size_t>(n);
        remaining -= sent;
        bytes_sent += sent;
    }
    return {WriteState::complete, bytes_sent};
}

bool TcpClient::write_all(std::string_view data, int write_timeout_ms)
{
    return write_attempt(data, write_timeout_ms).state == WriteState::complete;
}

bool TcpClient::write_all_until(std::string_view data, Deadline deadline)
{
    return write_attempt_until(data, deadline).state == WriteState::complete;
}

std::string TcpClient::read_until_header_end(std::size_t max_bytes,
                                             int read_timeout_ms)
{
    if (read_timeout_ms <= 0) return {};
    auto result = read_until_header_end_until(
        max_bytes, std::chrono::steady_clock::now()
        + std::chrono::milliseconds(read_timeout_ms));
    return std::move(result.data);
}

TcpClient::ReadAttempt TcpClient::read_until_header_end_until(
    std::size_t max_bytes, Deadline deadline)
{
    if (fd_ < 0) return {{}, ReadState::error};
    if (max_bytes == 0) return {{}, ReadState::limit};
    std::string out;
    out.reserve(std::min(max_bytes, std::size_t{4096}));
    char buf[4096];
    while (out.size() < max_bytes)
    {
        const int ready = wait_until(fd_, false, deadline);
        if (ready == 0) return {std::move(out), ReadState::deadline};
        if (ready < 0)
        {
            if (errno == EINTR) continue;
            return {std::move(out), ReadState::error};
        }
        const std::size_t want = std::min(max_bytes - out.size(), sizeof(buf));
        const ssize_t n = ::recv(fd_, buf, want, MSG_DONTWAIT);
        if (n < 0)
        {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return {std::move(out), ReadState::error};
        }
        if (n == 0) return {std::move(out), ReadState::eof};
        out.append(buf, static_cast<std::size_t>(n));
        if (out.find("\r\n\r\n") != std::string::npos)
            return {std::move(out), ReadState::complete};
    }
    return {std::move(out), ReadState::limit};
}

std::string TcpClient::read_n(std::size_t n, int read_timeout_ms)
{
    if (read_timeout_ms <= 0) return {};
    auto result = read_n_until(
        n, std::chrono::steady_clock::now()
        + std::chrono::milliseconds(read_timeout_ms));
    return std::move(result.data);
}

TcpClient::ReadAttempt TcpClient::read_n_until(std::size_t n, Deadline deadline)
{
    if (n == 0) return {{}, ReadState::complete};
    if (fd_ < 0) return {{}, ReadState::error};
    std::string out;
    out.reserve(std::min(n, std::size_t{4096}));
    char buf[4096];
    while (out.size() < n)
    {
        const int ready = wait_until(fd_, false, deadline);
        if (ready == 0) return {std::move(out), ReadState::deadline};
        if (ready < 0)
        {
            if (errno == EINTR) continue;
            return {std::move(out), ReadState::error};
        }
        const std::size_t want = std::min(n - out.size(), sizeof(buf));
        const ssize_t r = ::recv(fd_, buf, want, MSG_DONTWAIT);
        if (r < 0)
        {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return {std::move(out), ReadState::error};
        }
        if (r == 0) return {std::move(out), ReadState::eof};
        out.append(buf, static_cast<std::size_t>(r));
    }
    return {std::move(out), ReadState::complete};
}

}

#endif // HAS_QUESTDB
