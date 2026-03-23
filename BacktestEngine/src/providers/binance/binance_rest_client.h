#pragma once
#ifdef HAS_BINANCE

#include "providers/binance/binance_auth.h"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>

#include <iostream>
#include <string>
#include <stdexcept>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

// BinanceRestClient: synchronous HTTP client for Binance REST API.
//
// Handles:
// - GET/POST/PUT/DELETE requests with HMAC-SHA256 signing
// - TLS connection to api.binance.com or testnet.binance.vision
// - Rate limit tracking via X-MBX-USED-WEIGHT headers
// - Error response parsing (400/401/403/429)
class BinanceRestClient
{
public:
    BinanceRestClient(
        const std::string& api_key,
        const std::string& api_secret,
        const std::string& host = "api.binance.com",
        const std::string& port = "443")
        : api_key_(api_key)
        , api_secret_(api_secret)
        , host_(host)
        , port_(port)
        , ctx_(ssl::context::tlsv12_client)
    {
        ctx_.set_default_verify_paths();
        ctx_.set_verify_mode(ssl::verify_peer);
    }

    struct response
    {
        int status;
        std::string body;
        int used_weight = 0; // from X-MBX-USED-WEIGHT-1M header
    };

    // POST a signed request (e.g., new order).
    response post(const std::string& endpoint, const std::string& params)
    {
        auto query = params + "&timestamp=" + std::to_string(binance::server_time_ms())
                     + "&recvWindow=5000";
        auto signed_query = binance::sign_query(query, api_secret_);
        return execute(http::verb::post, endpoint, signed_query);
    }

    // GET a signed request (e.g., account info, trade history).
    response get(const std::string& endpoint, const std::string& params)
    {
        auto query = params + "&timestamp=" + std::to_string(binance::server_time_ms())
                     + "&recvWindow=5000";
        auto signed_query = binance::sign_query(query, api_secret_);
        return execute(http::verb::get, endpoint + "?" + signed_query, "");
    }

    // DELETE a signed request (e.g., cancel order).
    response del(const std::string& endpoint, const std::string& params)
    {
        auto query = params + "&timestamp=" + std::to_string(binance::server_time_ms())
                     + "&recvWindow=5000";
        auto signed_query = binance::sign_query(query, api_secret_);
        return execute(http::verb::delete_, endpoint + "?" + signed_query, "");
    }

    // POST an unsigned request (e.g., listen key creation).
    response post_unsigned(const std::string& endpoint)
    {
        return execute(http::verb::post, endpoint, "");
    }

    // PUT an unsigned request (e.g., listen key keepalive).
    response put_unsigned(const std::string& endpoint, const std::string& params = "")
    {
        return execute(http::verb::put, endpoint + (params.empty() ? "" : "?" + params), "");
    }

    int last_used_weight() const { return last_used_weight_; }

private:
    std::string api_key_;
    std::string api_secret_;
    std::string host_;
    std::string port_;
    ssl::context ctx_;
    int last_used_weight_ = 0;

    response execute(http::verb method, const std::string& target, const std::string& body)
    {
        try
        {
            net::io_context ioc;
            tcp::resolver resolver(ioc);
            auto results = resolver.resolve(host_, port_);

            beast::ssl_stream<tcp::socket> stream(ioc, ctx_);

            if (!SSL_set_tlsext_host_name(stream.native_handle(), host_.c_str()))
                throw std::runtime_error("SNI setup failed");

            auto& lowest = beast::get_lowest_layer(stream);
            net::connect(lowest, results);
            stream.handshake(ssl::stream_base::client);

            http::request<http::string_body> req{method, target, 11};
            req.set(http::field::host, host_);
            req.set(http::field::user_agent, "TrueTest/1.0");
            req.set("X-MBX-APIKEY", api_key_);

            if (!body.empty())
            {
                req.set(http::field::content_type, "application/x-www-form-urlencoded");
                req.body() = body;
                req.prepare_payload();
            }

            http::write(stream, req);

            beast::flat_buffer buffer;
            http::response<http::string_body> res;
            http::read(stream, buffer, res);

            response r;
            r.status = static_cast<int>(res.result_int());
            r.body = res.body();

            // Track rate limit weight
            auto weight_it = res.find("X-MBX-USED-WEIGHT-1M");
            if (weight_it != res.end())
            {
                try { r.used_weight = std::stoi(std::string(weight_it->value())); }
                catch (...) {}
                last_used_weight_ = r.used_weight;
            }

            // Graceful shutdown
            beast::error_code ec;
            stream.shutdown(ec);

            if (r.status == 429)
            {
                std::cerr << "BinanceRestClient: rate limited (429). "
                          << "Weight used: " << r.used_weight << "\n";
            }
            else if (r.status >= 400)
            {
                std::cerr << "BinanceRestClient: HTTP " << r.status
                          << " - " << r.body << "\n";
            }

            return r;
        }
        catch (const std::exception& e)
        {
            std::cerr << "BinanceRestClient: request failed: " << e.what() << "\n";
            return {0, "", 0};
        }
    }
};

#endif // HAS_BINANCE
