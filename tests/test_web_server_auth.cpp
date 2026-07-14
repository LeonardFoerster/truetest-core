#include <gtest/gtest.h>

#include "web/web_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

class socket_fd
{
public:
    explicit socket_fd(int fd = -1) : fd_(fd) {}
    ~socket_fd()
    {
        if (fd_ >= 0) ::close(fd_);
    }

    socket_fd(const socket_fd&) = delete;
    socket_fd& operator=(const socket_fd&) = delete;

    socket_fd(socket_fd&& other) noexcept : fd_(other.fd_)
    {
        other.fd_ = -1;
    }

    int get() const { return fd_; }

private:
    int fd_;
};

socket_fd make_socket()
{
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) throw std::runtime_error(std::strerror(errno));
    return socket_fd(fd);
}

int reserve_free_port()
{
    auto fd = make_socket();

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    if (::bind(fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
        throw std::runtime_error(std::strerror(errno));

    socklen_t len = sizeof(addr);
    if (::getsockname(fd.get(), reinterpret_cast<sockaddr*>(&addr), &len) != 0)
        throw std::runtime_error(std::strerror(errno));

    return ntohs(addr.sin_port);
}

socket_fd connect_localhost(int port)
{
    auto fd = make_socket();

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<std::uint16_t>(port));

    if (::connect(fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
        throw std::runtime_error(std::strerror(errno));

    return fd;
}

std::string request_raw(int port, const std::string& request)
{
    auto fd = connect_localhost(port);
    const char* data = request.data();
    std::size_t remaining = request.size();
    while (remaining > 0)
    {
        ssize_t n = ::send(fd.get(), data, remaining, MSG_NOSIGNAL);
        if (n <= 0) throw std::runtime_error(std::strerror(errno));
        data += n;
        remaining -= static_cast<std::size_t>(n);
    }

    std::string response;
    char buf[1024];
    for (;;)
    {
        ssize_t n = ::recv(fd.get(), buf, sizeof(buf), 0);
        if (n < 0) throw std::runtime_error(std::strerror(errno));
        if (n == 0) break;
        response.append(buf, static_cast<std::size_t>(n));
        if (response.find("\r\n\r\n") != std::string::npos &&
            response.find("101 Switching Protocols") != std::string::npos)
            break;
    }
    return response;
}

std::string http_get(int port,
                     const std::string& path,
                     const std::string& extra_headers = "")
{
    const std::string host = "127.0.0.1:" + std::to_string(port);
    return request_raw(port,
        "GET " + path + " HTTP/1.1\r\n"
        "Host: " + host + "\r\n" +
        extra_headers +
        "Connection: close\r\n\r\n");
}

std::string websocket_upgrade(int port,
                              const std::string& origin,
                              const std::string& path = "/stream?token=secret")
{
    const std::string host = "127.0.0.1:" + std::to_string(port);
    return request_raw(port,
        "GET " + path + " HTTP/1.1\r\n"
        "Host: " + host + "\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Origin: " + origin + "\r\n\r\n");
}

class WebServerAuthTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        std::signal(SIGPIPE, SIG_IGN);
    }

    void SetUp() override
    {
        truetest::web::web_config cfg;
        cfg.bind_addr = "127.0.0.1";
        cfg.port = reserve_free_port();
        cfg.token = "secret";
        cfg.poll_hz = 20;

        port_ = cfg.port;
        server_ = std::make_unique<truetest::web::WebServer>(
            std::move(cfg),
            [](truetest::ui::dashboard_snapshot&) { return false; },
            [] { return std::string("{\"ok\":true}"); });

        ASSERT_TRUE(server_->start());
    }

    void TearDown() override
    {
        server_.reset();
    }

    int port_ = 0;
    std::unique_ptr<truetest::web::WebServer> server_;
};

} // namespace

TEST_F(WebServerAuthTest, RestRejectsQueryTokenAndAcceptsBearer)
{
    const std::string query_token = http_get(port_, "/api/results?token=secret");
    EXPECT_NE(query_token.find("401 Unauthorized"), std::string::npos)
        << query_token;
    EXPECT_EQ(query_token.find("Access-Control-Allow-Origin: *"), std::string::npos)
        << query_token;

    const std::string bearer = http_get(
        port_, "/api/results", "Authorization: Bearer secret\r\n");
    EXPECT_NE(bearer.find("200 OK"), std::string::npos)
        << bearer;
    EXPECT_NE(bearer.find("{\"ok\":true}"), std::string::npos)
        << bearer;
}

TEST_F(WebServerAuthTest, WebSocketRejectsBadOrigin)
{
    const std::string response =
        websocket_upgrade(port_, "http://attacker.example");

    EXPECT_EQ(response.find("101 Switching Protocols"), std::string::npos)
        << response;
}

TEST_F(WebServerAuthTest, WebSocketAcceptsSameHostOriginWithQueryToken)
{
    const std::string response =
        websocket_upgrade(port_, "http://127.0.0.1:" + std::to_string(port_));

    EXPECT_NE(response.find("101 Switching Protocols"), std::string::npos)
        << response;
}

TEST_F(WebServerAuthTest, WebSocketAcceptsLocalhostAliasOrigin)
{
    const std::string response =
        websocket_upgrade(port_, "http://localhost:" + std::to_string(port_));

    EXPECT_NE(response.find("101 Switching Protocols"), std::string::npos)
        << response;
}
