#pragma once
#ifdef HAS_QUESTDB

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace truetest::questdb {

// Non-blocking-friendly POSIX TCP client. Thin wrapper around socket(2) +
// connect(2) + send(2) + recv(2). Reused by both the HTTP client (DDL,
// short one-shot requests) and the ILP writer (persistent connection,
// batched line writes). Read/write phases and all resolved connect addresses
// share one deadline. System DNS resolution remains subject to the platform
// resolver. Callers use configured/pre-resolved hosts for strict live capture
// so a wedged QuestDB socket cannot stall market ingestion or halt handling
// indefinitely.
class TcpClient
{
public:
    using Deadline = std::chrono::steady_clock::time_point;

    enum class WriteState
    {
        complete,
        no_bytes_sent,
        delivery_ambiguous,
    };

    struct WriteAttempt
    {
        WriteState state = WriteState::no_bytes_sent;
        std::size_t bytes_sent = 0;
    };

    enum class ReadState
    {
        complete,
        eof,
        deadline,
        error,
        limit,
    };

    struct ReadAttempt
    {
        std::string data;
        ReadState state = ReadState::error;
    };

    TcpClient() = default;
    ~TcpClient();
    TcpClient(const TcpClient&) = delete;
    TcpClient& operator=(const TcpClient&) = delete;

    bool connect(const std::string& host, std::uint16_t port,
                 int connect_timeout_ms = 2000);
    bool connect_until(const std::string& host, std::uint16_t port,
                       Deadline deadline);

    // A complete local send does not constitute an application-level ACK.
    // On failure, bytes_sent distinguishes a retry-safe pre-send failure from
    // a delivery-ambiguous partial write.
    WriteAttempt write_attempt(std::string_view data, int write_timeout_ms);
    WriteAttempt write_attempt_until(std::string_view data, Deadline deadline);
    bool write_all(std::string_view data, int write_timeout_ms = 500);
    bool write_all_until(std::string_view data, Deadline deadline);

    // Reads until "\r\n\r\n" (header terminator) or up to max_bytes.
    // Returns the accumulated bytes (may include body after header).
    std::string read_until_header_end(std::size_t max_bytes = 65536,
                                      int read_timeout_ms = 5000);
    ReadAttempt read_until_header_end_until(std::size_t max_bytes,
                                            Deadline deadline);

    std::string read_n(std::size_t n, int read_timeout_ms = 5000);
    ReadAttempt read_n_until(std::size_t n, Deadline deadline);

    bool is_connected() const { return fd_ >= 0; }
    void close();

private:
    int fd_ = -1;
};

}

#endif // HAS_QUESTDB
