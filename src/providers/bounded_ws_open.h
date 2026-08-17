#pragma once

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <utility>

#include <openssl/ssl.h>
#include <openssl/x509_vfy.h>

namespace provider_ws
{

// Configure both SNI and certificate identity verification for one peer.
// SNI chooses the certificate presented by a multi-tenant endpoint; it does
// not verify that the certificate belongs to `host`. Keep the two operations
// together so callers cannot accidentally enable only SNI.
inline bool configure_tls_peer_identity(SSL* handle, const std::string& host)
{
    if (!handle || host.empty() || host.find('\0') != std::string::npos)
        return false;
    if (!SSL_set_tlsext_host_name(handle, host.c_str()))
        return false;

    boost::system::error_code ec;
    boost::asio::ip::make_address(host, ec);
    if (!ec)
    {
        return X509_VERIFY_PARAM_set1_ip_asc(
                   SSL_get0_param(handle), host.c_str()) == 1;
    }
    return SSL_set1_host(handle, host.c_str()) == 1;
}

template <typename IoContext, typename Operation, typename Cancel>
bool run_bounded(IoContext& ioc, std::chrono::milliseconds timeout,
                 Operation&& operation, Cancel&& cancel)
{
    struct operation_state
    {
        boost::beast::error_code error;
        bool completed{false};
    };
    auto state = std::make_shared<operation_state>();
    if (ioc.stopped()) ioc.restart();
    operation([state](boost::beast::error_code ec) noexcept {
        state->error = ec;
        state->completed = true;
    });
    ioc.run_for(timeout);
    if (!state->completed)
    {
        cancel();
        if (ioc.stopped()) ioc.restart();
        // Cancellation is itself bounded. In particular, resolver.cancel()
        // need not synchronously interrupt a platform resolver operation.
        // The shared completion state keeps a late handler safe; callers must
        // likewise keep any buffers captured by an outstanding operation alive.
        constexpr auto cancellation_grace = std::chrono::milliseconds(100);
        ioc.run_for(cancellation_grace);
        if (!state->completed) ioc.stop();
    }
    return state->completed && !state->error;
}

template <typename IoContext, typename WebSocket,
          typename AfterConnect, typename BeforeWsHandshake>
bool open_tls_websocket(
    IoContext& ioc, WebSocket& ws,
    const std::string& host, const std::string& port,
    const std::string& target, std::chrono::milliseconds timeout,
    AfterConnect&& after_connect,
    BeforeWsHandshake&& before_ws_handshake)
{
    namespace beast = boost::beast;
    namespace net = boost::asio;
    namespace ssl = net::ssl;
    using tcp = net::ip::tcp;

    auto resolver = std::make_shared<tcp::resolver>(ioc);
    auto results = std::make_shared<tcp::resolver::results_type>();
    if (!run_bounded(
            ioc, timeout,
            [resolver, results, host, port](auto done) {
                resolver->async_resolve(
                    host, port,
                    [resolver, results, done](
                        beast::error_code ec,
                        tcp::resolver::results_type found) mutable {
                        if (!ec) *results = std::move(found);
                        done(ec);
                    });
            },
            [resolver] { resolver->cancel(); }))
        return false;

    auto& lowest = beast::get_lowest_layer(ws);
    if (!run_bounded(
            ioc, timeout,
            [&](auto done) {
                net::async_connect(
                    lowest, *results,
                    [done, results](beast::error_code ec,
                                    const tcp::endpoint&) mutable {
                        done(ec);
                    });
            },
            [&] {
                beast::error_code ignored;
                lowest.cancel(ignored);
                lowest.close(ignored);
            }))
        return false;

    after_connect(ws);
    if (!configure_tls_peer_identity(ws.next_layer().native_handle(), host))
        return false;

    if (!run_bounded(
            ioc, timeout,
            [&](auto done) {
                ws.next_layer().async_handshake(
                    ssl::stream_base::client,
                    [done](beast::error_code ec) mutable { done(ec); });
            },
            [&] {
                beast::error_code ignored;
                lowest.cancel(ignored);
                lowest.close(ignored);
            }))
        return false;

    before_ws_handshake(ws);
    return run_bounded(
        ioc, timeout,
        [&](auto done) {
            ws.async_handshake(
                host + ":" + port, target,
                [done](beast::error_code ec) mutable { done(ec); });
        },
        [&] {
            beast::error_code ignored;
            lowest.cancel(ignored);
            lowest.close(ignored);
        });
}

} // namespace provider_ws
