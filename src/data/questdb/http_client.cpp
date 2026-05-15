#ifdef HAS_QUESTDB

#include "http_client.h"
#include "tcp_client.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <iostream>
#include <string>

namespace truetest::questdb {

namespace {

bool is_unreserved(unsigned char c)
{
    // RFC 3986: ALPHA / DIGIT / "-" / "." / "_" / "~"
    return (c >= '0' && c <= '9')
        || (c >= 'A' && c <= 'Z')
        || (c >= 'a' && c <= 'z')
        || c == '-' || c == '.' || c == '_' || c == '~';
}

int parse_status_line(std::string_view header_block)
{
    // Expect "HTTP/1.x <status> <reason>\r\n..."
    const auto sp1 = header_block.find(' ');
    if (sp1 == std::string_view::npos) return 0;
    const auto sp2 = header_block.find(' ', sp1 + 1);
    if (sp2 == std::string_view::npos) return 0;
    const std::string code(header_block.substr(sp1 + 1, sp2 - sp1 - 1));
    try { return std::stoi(code); }
    catch (...) { return 0; }
}

// Returns the integer value of a Content-Length header, or 0 if absent.
std::size_t find_content_length(std::string_view headers)
{
    // Case-insensitive search for "content-length:"
    std::string lower;
    lower.reserve(headers.size());
    for (char c : headers) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    const auto pos = lower.find("\r\ncontent-length:");
    if (pos == std::string::npos) return 0;
    const auto eol = lower.find("\r\n", pos + 2);
    if (eol == std::string::npos) return 0;
    auto value = headers.substr(pos + 17, eol - (pos + 17));
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))  value.remove_suffix(1);
    try { return static_cast<std::size_t>(std::stoull(std::string(value))); }
    catch (...) { return 0; }
}

}

std::string url_encode(std::string_view s)
{
    std::string out;
    out.reserve(s.size() * 3);
    char buf[4];
    for (unsigned char c : s)
    {
        if (is_unreserved(c))
        {
            out.push_back(static_cast<char>(c));
        }
        else
        {
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            out.append(buf);
        }
    }
    return out;
}

std::optional<HttpResponse> query_exec(const std::string& host,
                                       std::uint16_t port,
                                       const std::string& sql,
                                       int timeout_ms)
{
    TcpClient tcp;
    if (!tcp.connect(host, port, timeout_ms)) return std::nullopt;

    std::string request;
    request.reserve(sql.size() * 3 + 256);
    request.append("GET /exec?query=");
    request.append(url_encode(sql));
    request.append(" HTTP/1.1\r\n");
    request.append("Host: ").append(host).append(":").append(std::to_string(port)).append("\r\n");
    request.append("User-Agent: truetest-questdb/1.0\r\n");
    request.append("Accept: */*\r\n");
    request.append("Connection: close\r\n");
    request.append("\r\n");

    if (!tcp.write_all(request)) return std::nullopt;

    std::string raw = tcp.read_until_header_end(65536, timeout_ms);
    if (raw.empty()) return std::nullopt;

    const auto sep = raw.find("\r\n\r\n");
    if (sep == std::string::npos) return std::nullopt;

    HttpResponse resp;
    resp.raw_headers = raw.substr(0, sep);
    resp.status = parse_status_line(resp.raw_headers);

    std::string body = raw.substr(sep + 4);
    const std::size_t content_length = find_content_length(resp.raw_headers);
    if (content_length > body.size())
    {
        body.append(tcp.read_n(content_length - body.size(), timeout_ms));
    }
    else if (content_length == 0)
    {
        // No Content-Length — drain until close.
        while (true)
        {
            const auto chunk = tcp.read_n(4096, timeout_ms);
            if (chunk.empty()) break;
            body.append(chunk);
        }
    }
    resp.body = std::move(body);
    return resp;
}

bool ping(const std::string& host, std::uint16_t port, int timeout_ms)
{
    auto r = query_exec(host, port, "SELECT 1", timeout_ms);
    return r && r->status == 200;
}

}

#endif // HAS_QUESTDB
