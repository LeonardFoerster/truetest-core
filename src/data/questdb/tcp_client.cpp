#ifdef HAS_QUESTDB

#include "tcp_client.h"

#include <arpa/inet.h>
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

bool TcpClient::connect(const std::string& host, std::uint16_t port,
                        int connect_timeout_ms)
{
    close();

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

    int last_err = 0;
    for (addrinfo* ai = result; ai != nullptr; ai = ai->ai_next)
    {
        const int sock = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock < 0)
        {
            last_err = errno;
            continue;
        }

        // Non-blocking connect with select() timeout.
        const int flags = ::fcntl(sock, F_GETFL, 0);
        ::fcntl(sock, F_SETFL, flags | O_NONBLOCK);

        const int rc = ::connect(sock, ai->ai_addr, ai->ai_addrlen);
        if (rc < 0 && errno != EINPROGRESS)
        {
            last_err = errno;
            ::close(sock);
            continue;
        }

        if (rc < 0)
        {
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(sock, &wfds);
            timeval tv{};
            tv.tv_sec = connect_timeout_ms / 1000;
            tv.tv_usec = (connect_timeout_ms % 1000) * 1000;

            const int sel = ::select(sock + 1, nullptr, &wfds, nullptr, &tv);
            if (sel <= 0)
            {
                last_err = (sel == 0) ? ETIMEDOUT : errno;
                ::close(sock);
                continue;
            }

            int so_err = 0;
            socklen_t so_len = sizeof(so_err);
            if (::getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_err, &so_len) < 0
                || so_err != 0)
            {
                last_err = (so_err != 0) ? so_err : errno;
                ::close(sock);
                continue;
            }
        }

        // Restore blocking mode and set TCP_NODELAY for ILP throughput.
        ::fcntl(sock, F_SETFL, flags);
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

bool TcpClient::write_all(std::string_view data)
{
    if (fd_ < 0) return false;

    const char* p = data.data();
    std::size_t remaining = data.size();
    while (remaining > 0)
    {
        const ssize_t n = ::send(fd_, p, remaining, MSG_NOSIGNAL);
        if (n < 0)
        {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        p += n;
        remaining -= static_cast<std::size_t>(n);
    }
    return true;
}

namespace {

bool wait_readable(int fd, int timeout_ms)
{
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    const int sel = ::select(fd + 1, &rfds, nullptr, nullptr, &tv);
    return sel > 0;
}

}

std::string TcpClient::read_until_header_end(std::size_t max_bytes,
                                             int read_timeout_ms)
{
    std::string out;
    if (fd_ < 0) return out;

    char buf[4096];
    while (out.size() < max_bytes)
    {
        if (!wait_readable(fd_, read_timeout_ms)) break;
        const ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
        if (n < 0)
        {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        out.append(buf, static_cast<std::size_t>(n));
        if (out.find("\r\n\r\n") != std::string::npos) break;
    }
    return out;
}

std::string TcpClient::read_n(std::size_t n, int read_timeout_ms)
{
    std::string out;
    if (fd_ < 0 || n == 0) return out;

    out.reserve(n);
    char buf[4096];
    while (out.size() < n)
    {
        if (!wait_readable(fd_, read_timeout_ms)) break;
        const std::size_t want = std::min(n - out.size(), sizeof(buf));
        const ssize_t r = ::recv(fd_, buf, want, 0);
        if (r < 0)
        {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) break;
        out.append(buf, static_cast<std::size_t>(r));
    }
    return out;
}

}

#endif // HAS_QUESTDB
