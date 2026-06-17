#pragma once
#ifdef HAS_QUESTDB

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace truetest::questdb {

// Non-blocking-friendly POSIX TCP client. Thin wrapper around socket(2) +
// connect(2) + send(2) + recv(2). Reused by both the HTTP client (DDL,
// short one-shot requests) and the ILP writer (persistent connection,
// batched line writes). Blocking I/O is fine - both use cases run off the
// hot path on the worker thread.
class TcpClient
{
public:
    TcpClient() = default;
    ~TcpClient();
    TcpClient(const TcpClient&) = delete;
    TcpClient& operator=(const TcpClient&) = delete;

    bool connect(const std::string& host, std::uint16_t port,
                 int connect_timeout_ms = 2000);

    bool write_all(std::string_view data);

    // Reads until "\r\n\r\n" (header terminator) or up to max_bytes.
    // Returns the accumulated bytes (may include body after header).
    std::string read_until_header_end(std::size_t max_bytes = 65536,
                                      int read_timeout_ms = 5000);

    std::string read_n(std::size_t n, int read_timeout_ms = 5000);

    bool is_connected() const { return fd_ >= 0; }
    void close();

private:
    int fd_ = -1;
};

}

#endif // HAS_QUESTDB
