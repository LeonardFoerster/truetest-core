#pragma once
#ifdef HAS_QUESTDB

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

// Percent-encode a SQL string for inclusion in a URL query value. Only
// characters unsafe for query values are escaped; RFC 3986 unreserved
// characters pass through. Spaces are encoded as %20, not '+'.
std::string url_encode(std::string_view s);

// Issue GET /exec?query=<url_encoded(sql)> against the QuestDB HTTP
// endpoint. Blocking. std::nullopt on TCP/I/O failure. Status 200 with a
// body on success.
std::optional<HttpResponse> query_exec(const std::string& host,
                                       std::uint16_t port,
                                       const std::string& sql,
                                       int timeout_ms = 5000);

// Cheap health probe: GET /exec?query=SELECT%201 and check status == 200.
bool ping(const std::string& host, std::uint16_t port,
          int timeout_ms = 2000);

} // namespace truetest::questdb

#endif // HAS_QUESTDB
