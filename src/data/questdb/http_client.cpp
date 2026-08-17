#ifdef HAS_QUESTDB

#include "http_client.h"
#include "tcp_client.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <limits>
#include <string>
#include <utility>

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
    const auto eol = header_block.find("\r\n");
    const auto line = header_block.substr(0, eol);
    if (!(line.starts_with("HTTP/1.0 ") || line.starts_with("HTTP/1.1 ")))
        return 0;
    constexpr std::size_t kCodeOffset = 9;
    if (line.size() < kCodeOffset + 3) return 0;
    const auto c0 = static_cast<unsigned char>(line[kCodeOffset]);
    const auto c1 = static_cast<unsigned char>(line[kCodeOffset + 1]);
    const auto c2 = static_cast<unsigned char>(line[kCodeOffset + 2]);
    if (!std::isdigit(c0) || !std::isdigit(c1) || !std::isdigit(c2)) return 0;
    if (line.size() > kCodeOffset + 3 && line[kCodeOffset + 3] != ' ')
        return 0;
    return static_cast<int>((c0 - '0') * 100 + (c1 - '0') * 10 + (c2 - '0'));
}

bool equals_ascii_case_insensitive(std::string_view lhs, std::string_view rhs)
{
    if (lhs.size() != rhs.size()) return false;
    for (std::size_t i = 0; i < lhs.size(); ++i)
    {
        if (std::tolower(static_cast<unsigned char>(lhs[i]))
            != std::tolower(static_cast<unsigned char>(rhs[i])))
            return false;
    }
    return true;
}

std::string_view trim_ows(std::string_view value)
{
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
        value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
        value.remove_suffix(1);
    return value;
}

enum class ContentLengthState
{
    absent,
    valid,
    invalid,
};

struct ContentLength
{
    ContentLengthState state = ContentLengthState::absent;
    std::size_t value = 0;
};

ContentLength parse_content_length(std::string_view headers)
{
    const auto first_eol = headers.find("\r\n");
    if (first_eol == std::string_view::npos) return {};

    bool seen_content_length = false;
    ContentLength result;
    std::size_t begin = first_eol + 2;
    while (begin < headers.size())
    {
        const auto eol = headers.find("\r\n", begin);
        const auto line = headers.substr(
            begin, eol == std::string_view::npos ? std::string_view::npos
                                                   : eol - begin);
        const auto colon = line.find(':');
        if (colon == std::string_view::npos)
            return {ContentLengthState::invalid, 0};
        const auto name = trim_ows(line.substr(0, colon));
        const auto value = trim_ows(line.substr(colon + 1));
        if (equals_ascii_case_insensitive(name, "transfer-encoding"))
            return {ContentLengthState::invalid, 0};
        if (equals_ascii_case_insensitive(name, "content-length"))
        {
            if (seen_content_length || value.empty())
                return {ContentLengthState::invalid, 0};
            std::size_t parsed = 0;
            for (const unsigned char c : value)
            {
                if (c < '0' || c > '9')
                    return {ContentLengthState::invalid, 0};
                const auto digit = static_cast<std::size_t>(c - '0');
                if (parsed > (std::numeric_limits<std::size_t>::max() - digit) / 10U)
                    return {ContentLengthState::invalid, 0};
                parsed = parsed * 10U + digit;
            }
            seen_content_length = true;
            result = {ContentLengthState::valid, parsed};
        }
        if (eol == std::string_view::npos) break;
        begin = eol + 2;
    }
    return result;
}

detail::HttpReadState map_read_state(TcpClient::ReadState state)
{
    switch (state)
    {
    case TcpClient::ReadState::complete: return detail::HttpReadState::complete;
    case TcpClient::ReadState::eof: return detail::HttpReadState::eof;
    case TcpClient::ReadState::deadline: return detail::HttpReadState::deadline;
    case TcpClient::ReadState::error: return detail::HttpReadState::error;
    case TcpClient::ReadState::limit: return detail::HttpReadState::limit;
    }
    return detail::HttpReadState::error;
}

class TcpHttpTransport final : public detail::IHttpTransport
{
public:
    bool connect(const std::string& host, std::uint16_t port,
                 detail::HttpDeadline deadline) override
    {
        return tcp_.connect_until(host, port, deadline);
    }

    bool write_all(std::string_view data, detail::HttpDeadline deadline) override
    {
        return tcp_.write_all_until(data, deadline);
    }

    detail::HttpReadResult read_until_header_end(
        std::size_t max_bytes, detail::HttpDeadline deadline) override
    {
        auto result = tcp_.read_until_header_end_until(max_bytes, deadline);
        return {std::move(result.data), map_read_state(result.state)};
    }

    detail::HttpReadResult read_exact(
        std::size_t bytes, detail::HttpDeadline deadline) override
    {
        auto result = tcp_.read_n_until(bytes, deadline);
        return {std::move(result.data), map_read_state(result.state)};
    }

private:
    TcpClient tcp_;
};

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

std::optional<HttpResponse> detail::query_exec_via_transport(
    IHttpTransport& transport, const std::string& host, std::uint16_t port,
    const std::string& sql, HttpDeadline deadline, HttpResponseLimits limits)
{
    const auto header_limit = std::min(
        limits.max_header_bytes, kDefaultMaxQuestdbHttpHeaderBytes);
    const auto body_limit = std::min(
        limits.max_body_bytes, kDefaultMaxQuestdbHttpBodyBytes);
    if (sql.size() > kMaxQuestdbHttpSqlBytes || header_limit == 0)
        return std::nullopt;

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

    if (!transport.connect(host, port, deadline)) return std::nullopt;
    if (!transport.write_all(request, deadline)) return std::nullopt;

    auto header = transport.read_until_header_end(header_limit, deadline);
    if (header.state != HttpReadState::complete || header.data.empty()
        || header.data.size() > header_limit)
        return std::nullopt;

    const auto sep = header.data.find("\r\n\r\n");
    if (sep == std::string::npos) return std::nullopt;

    HttpResponse resp;
    resp.raw_headers = header.data.substr(0, sep);
    resp.status = parse_status_line(resp.raw_headers);
    if (resp.status <= 0) return std::nullopt;

    const auto content_length = parse_content_length(resp.raw_headers);
    if (content_length.state != ContentLengthState::valid
        || content_length.value > body_limit)
        return std::nullopt;

    std::string body = header.data.substr(sep + 4);
    if (body.size() > content_length.value || body.size() > body_limit)
        return std::nullopt;
    body.reserve(content_length.value);
    if (content_length.value > body.size())
    {
        auto tail = transport.read_exact(content_length.value - body.size(), deadline);
        if (tail.state != HttpReadState::complete
            || tail.data.size() != content_length.value - body.size())
            return std::nullopt;
        body.append(tail.data);
    }
    if (body.size() != content_length.value) return std::nullopt;
    resp.body = std::move(body);
    return resp;
}

std::optional<HttpResponse> query_exec(const std::string& host,
                                       std::uint16_t port,
                                       const std::string& sql,
                                       int timeout_ms,
                                       HttpResponseLimits limits)
{
    if (timeout_ms <= 0) return std::nullopt;
    TcpHttpTransport transport;
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(timeout_ms);
    return detail::query_exec_via_transport(
        transport, host, port, sql, deadline, limits);
}

bool ping(const std::string& host, std::uint16_t port, int timeout_ms)
{
    auto r = query_exec(host, port, "SELECT 1", timeout_ms);
    return r && r->status == 200;
}

}

#endif // HAS_QUESTDB
