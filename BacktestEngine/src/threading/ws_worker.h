#pragma once
#ifdef HAS_WEB_UI

#include "worker.h"
#include "../core/event_json.h"

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

// A single WebSocket session, created per client connection.
class WsSession : public std::enable_shared_from_this<WsSession>
{
public:
    explicit WsSession(tcp::socket socket)
        : ws_(std::move(socket)) {}

    void start()
    {
        ws_.set_option(websocket::stream_base::timeout::suggested(
            beast::role_type::server));

        ws_.async_accept([self = shared_from_this()](beast::error_code ec) {
            if (!ec)
                self->do_read();
        });
    }

    void send(const std::string& msg)
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!open_) return;

        // Queue the message
        queue_.push_back(msg);
        if (queue_.size() == 1)
            do_write();
    }

    bool is_open() const
    {
        std::lock_guard<std::mutex> lk(mu_);
        return open_;
    }

private:
    websocket::stream<tcp::socket> ws_;
    mutable std::mutex mu_;
    bool open_ = true;
    beast::flat_buffer buffer_;
    std::vector<std::string> queue_;

    void do_read()
    {
        ws_.async_read(buffer_,
            [self = shared_from_this()](beast::error_code ec, std::size_t) {
                if (ec)
                {
                    self->close();
                    return;
                }
                self->buffer_.consume(self->buffer_.size());
                self->do_read();
            });
    }

    void do_write()
    {
        // mu_ must be held by caller
        if (queue_.empty() || !open_) return;

        ws_.text(true);
        ws_.async_write(
            net::buffer(queue_.front()),
            [self = shared_from_this()](beast::error_code ec, std::size_t) {
                std::lock_guard<std::mutex> lk(self->mu_);
                if (ec)
                {
                    self->open_ = false;
                    return;
                }
                self->queue_.erase(self->queue_.begin());
                if (!self->queue_.empty())
                    self->do_write();
            });
    }

    void close()
    {
        std::lock_guard<std::mutex> lk(mu_);
        open_ = false;
    }
};

// WebSocketWorker: consumes events from a ring buffer, serializes them
// to JSON, and broadcasts to all connected WebSocket clients.
//
// Runs a Boost.Asio io_context on a separate internal thread for the
// WS server. The worker's main thread (from the engine) processes
// events and posts broadcasts to the io_context.
class WebSocketWorker : public Worker
{
public:
    explicit WebSocketWorker(uint16_t port = 8765)
        : port_(port)
        , acceptor_(ioc_, tcp::endpoint(tcp::v4(), port))
    {
        // Start accepting connections
        do_accept();

        // Run io_context on its own thread
        io_thread_ = std::thread([this]() {
            ioc_.run();
        });
    }

    ~WebSocketWorker()
    {
        // Stop the io_context and join
        ioc_.stop();
        if (io_thread_.joinable())
            io_thread_.join();
    }

    void on_event(const event_pointer& ev) override
    {
        auto json = event_json::event_to_json(ev);
        if (json.empty()) return;

        broadcast(json);
    }

    uint16_t port() const { return port_; }

    std::size_t session_count() const
    {
        std::lock_guard<std::mutex> lk(sessions_mu_);
        return sessions_.size();
    }

private:
    uint16_t port_;
    net::io_context ioc_;
    tcp::acceptor acceptor_;
    std::thread io_thread_;

    mutable std::mutex sessions_mu_;
    std::set<std::shared_ptr<WsSession>> sessions_;

    void do_accept()
    {
        acceptor_.async_accept(
            [this](beast::error_code ec, tcp::socket socket) {
                if (!ec)
                {
                    auto session = std::make_shared<WsSession>(std::move(socket));
                    {
                        std::lock_guard<std::mutex> lk(sessions_mu_);
                        sessions_.insert(session);
                    }
                    session->start();
                }

                // Continue accepting
                if (acceptor_.is_open())
                    do_accept();
            });
    }

    void broadcast(const std::string& msg)
    {
        std::lock_guard<std::mutex> lk(sessions_mu_);

        // Clean up closed sessions while iterating
        auto it = sessions_.begin();
        while (it != sessions_.end())
        {
            if ((*it)->is_open())
            {
                (*it)->send(msg);
                ++it;
            }
            else
            {
                it = sessions_.erase(it);
            }
        }
    }
};

#endif // HAS_WEB_UI
