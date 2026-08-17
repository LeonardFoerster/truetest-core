#pragma once
#ifdef HAS_QUESTDB

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace truetest::questdb {

struct HttpResponse
{
    int status = 0;
    std::string body;
    std::string raw_headers;
};

inline constexpr std::size_t kMaxQuestdbHttpSqlBytes = 64U * 1024U;
inline constexpr std::size_t kDefaultMaxQuestdbHttpHeaderBytes = 64U * 1024U;
inline constexpr std::size_t kDefaultMaxQuestdbHttpBodyBytes = 1U * 1024U * 1024U;

struct HttpResponseLimits
{
    std::size_t max_header_bytes = kDefaultMaxQuestdbHttpHeaderBytes;
    std::size_t max_body_bytes = kDefaultMaxQuestdbHttpBodyBytes;
};

namespace detail {

using HttpDeadline = std::chrono::steady_clock::time_point;

enum class HttpReadState
{
    complete,
    eof,
    deadline,
    error,
    limit,
};

struct HttpReadResult
{
    std::string data;
    HttpReadState state = HttpReadState::error;
};

// Internal seam: it keeps response framing and the one-deadline contract
// testable without a live socket or oversized test payloads.
class IHttpTransport
{
public:
    virtual ~IHttpTransport() = default;
    virtual bool connect(const std::string& host, std::uint16_t port,
                         HttpDeadline deadline) = 0;
    virtual bool write_all(std::string_view data, HttpDeadline deadline) = 0;
    virtual HttpReadResult read_until_header_end(
        std::size_t max_bytes, HttpDeadline deadline) = 0;
    virtual HttpReadResult read_exact(std::size_t bytes,
                                      HttpDeadline deadline) = 0;
};

std::optional<HttpResponse> query_exec_via_transport(
    IHttpTransport& transport, const std::string& host, std::uint16_t port,
    const std::string& sql, HttpDeadline deadline,
    HttpResponseLimits limits = {});

}

// Percent-encode a SQL string for inclusion in a URL query value. Only
// characters unsafe for query values are escaped; RFC 3986 unreserved
// characters pass through. Spaces are encoded as %20, not '+'.
std::string url_encode(std::string_view s);

// Issue GET /exec?query=<url_encoded(sql)> against the QuestDB HTTP endpoint.
// Blocking. Responses require one valid Content-Length and no transfer coding;
// malformed, incomplete, or over-limit responses return std::nullopt. The
// timeout covers socket phases, while platform DNS resolution is not
// interruptible here. Status 200 with a complete body indicates success.
std::optional<HttpResponse> query_exec(const std::string& host,
                                       std::uint16_t port,
                                       const std::string& sql,
                                       int timeout_ms = 5000,
                                       HttpResponseLimits limits = {});

// Cheap health probe: GET /exec?query=SELECT%201 and check status == 200.
bool ping(const std::string& host, std::uint16_t port,
          int timeout_ms = 2000);

}

#endif // HAS_QUESTDB
