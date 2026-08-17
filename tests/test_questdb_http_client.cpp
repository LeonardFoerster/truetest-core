#ifdef HAS_QUESTDB

#include "data/questdb/http_client.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

using truetest::questdb::url_encode;

namespace {

using truetest::questdb::HttpResponseLimits;
using truetest::questdb::detail::HttpDeadline;
using truetest::questdb::detail::HttpReadResult;
using truetest::questdb::detail::HttpReadState;
using truetest::questdb::detail::IHttpTransport;

class ScriptedHttpTransport final : public IHttpTransport
{
public:
    bool connect(const std::string&, std::uint16_t, HttpDeadline deadline) override
    {
        deadlines.push_back(deadline);
        ++connect_calls;
        return connect_ok;
    }

    bool write_all(std::string_view, HttpDeadline deadline) override
    {
        deadlines.push_back(deadline);
        ++write_calls;
        return write_ok;
    }

    HttpReadResult read_until_header_end(std::size_t max_bytes,
                                         HttpDeadline deadline) override
    {
        deadlines.push_back(deadline);
        header_limits.push_back(max_bytes);
        ++header_reads;
        return header_result;
    }

    HttpReadResult read_exact(std::size_t bytes, HttpDeadline deadline) override
    {
        deadlines.push_back(deadline);
        body_sizes.push_back(bytes);
        ++body_reads;
        if (next_body_result < body_results.size())
            return body_results[next_body_result++];
        return {{}, HttpReadState::eof};
    }

    bool connect_ok = true;
    bool write_ok = true;
    HttpReadResult header_result;
    std::vector<HttpReadResult> body_results;
    std::vector<HttpDeadline> deadlines;
    std::vector<std::size_t> header_limits;
    std::vector<std::size_t> body_sizes;
    std::size_t next_body_result = 0;
    int connect_calls = 0;
    int write_calls = 0;
    int header_reads = 0;
    int body_reads = 0;
};

constexpr HttpResponseLimits kSmallLimits{64, 8};

std::optional<truetest::questdb::HttpResponse> execute(
    ScriptedHttpTransport& transport, std::string sql = "SELECT 1",
    HttpResponseLimits limits = kSmallLimits,
    HttpDeadline deadline = HttpDeadline::max())
{
    if (deadline == HttpDeadline::max())
        deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    return truetest::questdb::detail::query_exec_via_transport(
        transport, "127.0.0.1", 9000, sql, deadline, limits);
}

}

TEST(QuestdbHttpClient, UrlEncodeBasic)
{
    EXPECT_EQ(url_encode(" "), "%20");
    EXPECT_EQ(url_encode("="), "%3D");
    EXPECT_EQ(url_encode("&"), "%26");
    EXPECT_EQ(url_encode("?"), "%3F");
}

TEST(QuestdbHttpClient, UrlEncodeUnreservedPassesThrough)
{
    const std::string s = "abcXYZ012-_.~";
    EXPECT_EQ(url_encode(s), s);
}

TEST(QuestdbHttpClient, UrlEncodeSqlCreateTable)
{
    const std::string sql = "CREATE TABLE foo (a INT)";
    // Spaces -> %20, parens -> %28 / %29
    EXPECT_EQ(url_encode(sql),
              "CREATE%20TABLE%20foo%20%28a%20INT%29");
}

TEST(QuestdbHttpClient, UrlEncodeEmpty)
{
    EXPECT_EQ(url_encode(""), "");
}

TEST(QuestdbHttpClient, UrlEncodeNonAscii)
{
    // 'ü' is UTF-8 0xC3 0xBC.
    EXPECT_EQ(url_encode("\xC3\xBC"), "%C3%BC");
}

TEST(QuestdbHttpClient, AcceptsExactBoundedContentLengthWithOneDeadline)
{
    ScriptedHttpTransport transport;
    transport.header_result = {
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nabc",
        HttpReadState::complete};
    transport.body_results.push_back({"de", HttpReadState::complete});

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    const auto response = execute(transport, "SELECT 1", kSmallLimits, deadline);

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_EQ(response->body, "abcde");
    ASSERT_EQ(transport.deadlines.size(), 4u);
    EXPECT_TRUE(std::all_of(transport.deadlines.begin(), transport.deadlines.end(),
                            [deadline](HttpDeadline observed) {
                                return observed == deadline;
                            }));
    ASSERT_EQ(transport.header_limits.size(), 1u);
    EXPECT_EQ(transport.header_limits[0], kSmallLimits.max_header_bytes);
    ASSERT_EQ(transport.body_sizes.size(), 1u);
    EXPECT_EQ(transport.body_sizes[0], 2u);
}

TEST(QuestdbHttpClient, RejectsDeclaredBodyAboveLimitBeforeReading)
{
    ScriptedHttpTransport transport;
    transport.header_result = {
        "HTTP/1.1 200 OK\r\nContent-Length: 9\r\n\r\n",
        HttpReadState::complete};

    EXPECT_FALSE(execute(transport));
    EXPECT_EQ(transport.body_reads, 0);
}

TEST(QuestdbHttpClient, RejectsTruncatedDeclaredBody)
{
    ScriptedHttpTransport transport;
    transport.header_result = {
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nabc",
        HttpReadState::complete};
    transport.body_results.push_back({"", HttpReadState::eof});

    EXPECT_FALSE(execute(transport));
}

TEST(QuestdbHttpClient, RejectsAmbiguousOrUnsupportedResponseFraming)
{
    const std::vector<std::string> headers{
        "HTTP/1.1 200 OK\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 1x\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 184467440737095516160\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n",
    };

    for (const auto& header : headers)
    {
        ScriptedHttpTransport transport;
        transport.header_result = {header, HttpReadState::complete};
        SCOPED_TRACE(header);
        EXPECT_FALSE(execute(transport));
        EXPECT_EQ(transport.body_reads, 0);
    }
}

TEST(QuestdbHttpClient, RejectsExtraOrMalformedBodyAndHeaderLimit)
{
    ScriptedHttpTransport extra_body;
    extra_body.header_result = {
        "HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nab",
        HttpReadState::complete};
    EXPECT_FALSE(execute(extra_body));

    ScriptedHttpTransport oversized_header;
    oversized_header.header_result = {{}, HttpReadState::limit};
    EXPECT_FALSE(execute(oversized_header));

    ScriptedHttpTransport transport_ignoring_header_limit;
    transport_ignoring_header_limit.header_result = {
        "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nX: "
            + std::string(64, 'x') + "\r\n\r\n",
        HttpReadState::complete};
    EXPECT_FALSE(execute(transport_ignoring_header_limit));

    ScriptedHttpTransport short_complete_tail;
    short_complete_tail.header_result = {
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nabc",
        HttpReadState::complete};
    short_complete_tail.body_results.push_back({"d", HttpReadState::complete});
    EXPECT_FALSE(execute(short_complete_tail));
}

TEST(QuestdbHttpClient, RejectsMalformedHttpStatusLine)
{
    for (const std::string status : {
             "NOTHTTP 200 OK", "HTTP/1.1 200x OK", "HTTP/1.1 +200 OK"})
    {
        ScriptedHttpTransport transport;
        transport.header_result = {
            status + "\r\nContent-Length: 0\r\n\r\n",
            HttpReadState::complete};
        SCOPED_TRACE(status);
        EXPECT_FALSE(execute(transport));
    }
}

TEST(QuestdbHttpClient, AcceptsExplicitZeroLengthAndRejectsOversizedSql)
{
    ScriptedHttpTransport empty;
    empty.header_result = {
        "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n",
        HttpReadState::complete};
    const auto empty_response = execute(empty);
    ASSERT_TRUE(empty_response);
    EXPECT_TRUE(empty_response->body.empty());
    EXPECT_EQ(empty.body_reads, 0);

    ScriptedHttpTransport oversized_sql;
    EXPECT_FALSE(execute(
        oversized_sql,
        std::string(truetest::questdb::kMaxQuestdbHttpSqlBytes + 1, 'x')));
    EXPECT_EQ(oversized_sql.connect_calls, 0);
}

#endif // HAS_QUESTDB
