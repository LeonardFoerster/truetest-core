#pragma once
#ifdef HAS_WEB_UI

#include "worker.h"
#include "../core/event_json.h"

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <set>
#include <string>
#include <thread>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

// Command received from a WebSocket client.
struct ws_command
{
    std::string command;    // "start", "pause", "stop", "order", "set_timeframe"
    std::string side;       // for order commands
    double quantity = 0.0;
    double price = 0.0;
    std::string order_type; // "market" or "limit"
    std::string timeframe;  // for set_timeframe (e.g. "1m", "5m", "1h")
};

// Callback type: called when a new client connects (for sending state snapshot)
using on_client_connect_fn = std::function<void()>;

// A single WebSocket session, created per client connection.
class WsSession : public std::enable_shared_from_this<WsSession>
{
public:
    using command_callback_t = std::function<void(const std::string&)>;

    explicit WsSession(tcp::socket socket, command_callback_t on_msg = {})
        : ws_(std::move(socket))
        , on_message_(std::move(on_msg)) {}

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
    command_callback_t on_message_;

    void do_read()
    {
        ws_.async_read(buffer_,
            [self = shared_from_this()](beast::error_code ec, std::size_t bytes) {
                if (ec)
                {
                    self->close();
                    return;
                }
                // Process incoming message
                if (bytes > 0 && self->on_message_)
                {
                    auto data = beast::buffers_to_string(self->buffer_.data());
                    self->on_message_(data);
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
// Bidirectional: also accepts incoming JSON commands from clients
// and queues them for the engine to poll via poll_command().
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

    // --- Bidirectional command interface ---

    // Poll for the next command from a WebSocket client.
    // Returns true if a command was dequeued, false if the queue is empty.
    bool poll_command(ws_command& cmd)
    {
        std::lock_guard<std::mutex> lk(cmd_mu_);
        if (command_queue_.empty()) return false;
        cmd = std::move(command_queue_.front());
        command_queue_.pop();
        return true;
    }

    // Broadcast a raw JSON string to all connected clients.
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

    // Broadcast a status update to all clients.
    void broadcast_status(const std::string& state,
                          const std::string& strategy = "",
                          const std::string& symbol = "")
    {
        char buf[512];
        std::snprintf(buf, sizeof(buf),
            R"({"type":"status","data":{"state":"%s","strategy":"%s","symbol":"%s"}})",
            state.c_str(), strategy.c_str(), symbol.c_str());
        broadcast(std::string(buf));
    }

    // Broadcast an orderbook snapshot (top N levels).
    void broadcast_orderbook(const std::vector<std::pair<double, double>>& bids,
                             const std::vector<std::pair<double, double>>& asks,
                             double spread)
    {
        std::string json = R"({"type":"orderbook","data":{"bids":[)";
        for (std::size_t i = 0; i < bids.size(); ++i)
        {
            if (i > 0) json += ",";
            char lbuf[128];
            std::snprintf(lbuf, sizeof(lbuf),
                R"({"price":%.6f,"quantity":%.8g})", bids[i].first, bids[i].second);
            json += lbuf;
        }
        json += R"(],"asks":[)";
        for (std::size_t i = 0; i < asks.size(); ++i)
        {
            if (i > 0) json += ",";
            char lbuf[128];
            std::snprintf(lbuf, sizeof(lbuf),
                R"({"price":%.6f,"quantity":%.8g})", asks[i].first, asks[i].second);
            json += lbuf;
        }
        char spread_buf[64];
        std::snprintf(spread_buf, sizeof(spread_buf), R"(],"spread":%.6f}})", spread);
        json += spread_buf;
        broadcast(json);
    }

    // Set callback for when a new client connects (used for state snapshot)
    void set_on_client_connect(on_client_connect_fn fn)
    {
        std::lock_guard<std::mutex> lk(connect_mu_);
        on_client_connect_ = std::move(fn);
    }

    // Check if a new client connected recently (for engine to send snapshot)
    bool has_pending_connect()
    {
        return pending_connect_.exchange(false, std::memory_order_acquire);
    }

private:
    uint16_t port_;
    net::io_context ioc_;
    tcp::acceptor acceptor_;
    std::thread io_thread_;

    mutable std::mutex sessions_mu_;
    std::set<std::shared_ptr<WsSession>> sessions_;

    // Command queue: WS clients → engine
    std::mutex cmd_mu_;
    std::queue<ws_command> command_queue_;

    // Client connect notification
    std::mutex connect_mu_;
    on_client_connect_fn on_client_connect_;
    std::atomic<bool> pending_connect_{false};

    void do_accept()
    {
        acceptor_.async_accept(
            [this](beast::error_code ec, tcp::socket socket) {
                if (!ec)
                {
                    // Create session with command callback
                    auto session = std::make_shared<WsSession>(
                        std::move(socket),
                        [this](const std::string& msg) { on_client_message(msg); }
                    );
                    {
                        std::lock_guard<std::mutex> lk(sessions_mu_);
                        sessions_.insert(session);
                    }
                    session->start();

                    // Notify engine that a new client connected
                    pending_connect_.store(true, std::memory_order_release);

                    // Call connect callback if set
                    {
                        std::lock_guard<std::mutex> lk(connect_mu_);
                        if (on_client_connect_)
                            on_client_connect_();
                    }
                }

                // Continue accepting
                if (acceptor_.is_open())
                    do_accept();
            });
    }

    // Parse an incoming JSON command from a client.
    // Hand-rolled extraction (no JSON library, consistent with project conventions).
    void on_client_message(const std::string& raw)
    {
        ws_command cmd;

        // Extract "command" field
        auto extract = [&](const char* key) -> std::string {
            std::string search = std::string("\"") + key + "\":\"";
            auto pos = raw.find(search);
            if (pos == std::string::npos) return {};
            pos += search.size();
            auto end = raw.find('"', pos);
            if (end == std::string::npos) return {};
            return raw.substr(pos, end - pos);
        };

        auto extract_num = [&](const char* key) -> double {
            std::string search = std::string("\"") + key + "\":";
            auto pos = raw.find(search);
            if (pos == std::string::npos) return 0.0;
            pos += search.size();
            // Skip whitespace
            while (pos < raw.size() && (raw[pos] == ' ' || raw[pos] == '\t')) pos++;
            try { return std::stod(raw.substr(pos)); }
            catch (...) { return 0.0; }
        };

        cmd.command = extract("command");
        if (cmd.command.empty()) return;

        cmd.side = extract("side");
        cmd.order_type = extract("type");
        cmd.quantity = extract_num("quantity");
        cmd.price = extract_num("price");
        cmd.timeframe = extract("timeframe");

        std::lock_guard<std::mutex> lk(cmd_mu_);
        command_queue_.push(std::move(cmd));
    }
};

#endif // HAS_WEB_UI
