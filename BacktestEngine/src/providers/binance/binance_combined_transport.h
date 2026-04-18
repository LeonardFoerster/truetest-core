#pragma once
#ifdef HAS_BINANCE

#include "providers/transport.h"
#include "utils/retry.h"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

class BinanceCombinedTransport : public IDataTransport
{
public:
    BinanceCombinedTransport(
        const std::vector<std::string>& streams,
        const std::string& host = "stream.binance.com",
        const std::string& port = "9443")
        : streams_(streams)
        , host_(host)
        , port_(port)
        , ctx_(ssl::context::tlsv12_client)
    {
        ctx_.set_default_verify_paths();
        ctx_.set_verify_mode(ssl::verify_peer);
    }

    bool open() override
    {
        try
        {
            tcp::resolver resolver(ioc_);
            auto results = resolver.resolve(host_, port_);

            ws_ = std::make_unique<
                websocket::stream<beast::ssl_stream<tcp::socket>>>(ioc_, ctx_);

            auto& lowest = beast::get_lowest_layer(*ws_);
            net::connect(lowest, results);

            if (!SSL_set_tlsext_host_name(
                    ws_->next_layer().native_handle(), host_.c_str()))
                return false;

            ws_->next_layer().handshake(ssl::stream_base::client);

            ws_->set_option(websocket::stream_base::decorator(
                [](websocket::request_type& req) {
                    req.set(boost::beast::http::field::user_agent, "TrueTest/1.0");
                }));

            std::string target = "/stream?streams=";
            for (size_t i = 0; i < streams_.size(); ++i)
            {
                if (i > 0) target += "/";
                target += streams_[i];
            }

            ws_->handshake(host_ + ":" + port_, target);

            open_ = true;
            stopped_ = false;
            return true;
        }
        catch (const std::exception& e)
        {
            std::cerr << "BinanceCombinedTransport: open failed: " << e.what() << "\n";
            return false;
        }
    }

    void close() override
    {
        std::lock_guard<std::mutex> lk(mu_);
        open_ = false;

        if (ws_)
        {
            try
            {
                beast::error_code ec;
                ws_->close(websocket::close_code::normal, ec);
            }
            catch (...) {}
        }
    }

    bool is_open() const override { return open_.load(); }

    std::optional<std::string> read_line() override
    {
        return read_line_blocking();
    }

    bool is_streaming() const override { return true; }

    std::optional<std::string> read_line_blocking() override
    {
        if (!ws_ || stopped_.load())
            return std::nullopt;

        try
        {
            beast::flat_buffer buffer;
            ws_->read(buffer);

            if (stopped_.load())
                return std::nullopt;

            return beast::buffers_to_string(buffer.data());
        }
        catch (const beast::system_error& se)
        {
            if (se.code() == websocket::error::closed)
            {
                open_ = false;
                return std::nullopt;
            }

            if (!stopped_.load() && reconnect())
                return read_line_blocking();

            open_ = false;
            return std::nullopt;
        }
        catch (const std::exception&)
        {
            open_ = false;
            return std::nullopt;
        }
    }

    void request_stop() override
    {
        stopped_ = true;
        close();
    }

private:
    std::vector<std::string> streams_;
    std::string host_;
    std::string port_;

    net::io_context ioc_;
    ssl::context ctx_;
    std::unique_ptr<websocket::stream<beast::ssl_stream<tcp::socket>>> ws_;

    std::mutex mu_;
    std::atomic<bool> open_{false};
    std::atomic<bool> stopped_{false};

    static constexpr unsigned MAX_RECONNECTS = 5;

    bool reconnect()
    {
        return retry_with_backoff([this]() {
            ioc_.restart();
            ws_.reset();
            return open();
        }, MAX_RECONNECTS,
           std::chrono::milliseconds(1000),
           std::chrono::milliseconds(16000));
    }
};

#endif // HAS_BINANCE
