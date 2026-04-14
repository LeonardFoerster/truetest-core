#pragma once
#ifdef HAS_WEB_UI

#include "worker.h"
#include "ring_buffer.h"
#include "http_handler.h"
#include "../core/event_json.h"

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http.hpp>
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
    std::string command;    // "start", "pause", "stop", "order", "set_timeframe", "set_symbol", "set_strategy"
    std::string side;       // for order commands
    double quantity = 0.0;
    double price = 0.0;
    std::string order_type; // "market" or "limit"
    std::string timeframe;  // for set_timeframe (e.g. "1m", "5m", "1h")
    std::string value;      // generic value for set_symbol, set_strategy
};

// Callback type: called when a new client connects (for sending state snapshot)
using on_client_connect_fn = std::function<void()>;

// Per-session event filter. When empty (default), all events pass through
// (backward compatible). When populated, only matching events are sent.
struct ws_event_filter
{
    std::set<std::string> event_types;  // e.g. {"fill", "tick", "market"}
    std::set<std::string> symbols;      // e.g. {"BTCUSDT", "ETHUSDT"}

    // Returns true if the filter is empty (accept everything).
    bool accepts_all() const { return event_types.empty() && symbols.empty(); }

    // Check if a JSON event message passes this filter.
    // Extracts "type" and optionally "symbol" from the JSON to match.
    bool matches(const std::string& json) const
    {
        if (accepts_all()) return true;

        // Extract event type from JSON: look for "type":"<value>"
        if (!event_types.empty())
        {
            std::string search = "\"type\":\"";
            auto pos = json.find(search);
            if (pos != std::string::npos)
            {
                pos += search.size();
                auto end = json.find('"', pos);
                if (end != std::string::npos)
                {
                    std::string type = json.substr(pos, end - pos);
                    if (event_types.find(type) == event_types.end())
                        return false;
                }
            }
        }

        // Extract symbol from JSON: look for "symbol":"<value>" in data
        if (!symbols.empty())
        {
            std::string search = "\"symbol\":\"";
            auto pos = json.find(search);
            if (pos != std::string::npos)
            {
                pos += search.size();
                auto end = json.find('"', pos);
                if (end != std::string::npos)
                {
                    std::string symbol = json.substr(pos, end - pos);
                    // Convert to uppercase for comparison
                    std::string upper_symbol = symbol;
                    for (auto& c : upper_symbol) c = static_cast<char>(std::toupper(c));
                    bool found = false;
                    for (const auto& s : symbols)
                    {
                        std::string upper_s = s;
                        for (auto& c : upper_s) c = static_cast<char>(std::toupper(c));
                        if (upper_s == upper_symbol) { found = true; break; }
                    }
                    if (!found) return false;
                }
            }
            // If no symbol field in JSON, let it pass (status, error, etc.)
        }

        return true;
    }
};

// A single WebSocket session, created per client connection.
//
// Lock-free design: the engine thread (producer) pushes messages into an SPSC
// ring buffer via send(). The Boost.Asio io_context thread (consumer) pops
// messages and feeds them to async_write. An atomic writing_ flag coordinates
// the async_write chain startup between the two threads.
class WsSession : public std::enable_shared_from_this<WsSession>
{
public:
    using command_callback_t = std::function<void(const std::string&)>;

    explicit WsSession(tcp::socket socket, net::io_context& ioc,
                       command_callback_t on_msg = {},
                       bool compress = true)
        : ws_(std::move(socket))
        , ioc_(ioc)
        , on_message_(std::move(on_msg))
        , compress_(compress) {}

    void start()
    {
        apply_options();
        ws_.async_accept([self = shared_from_this()](beast::error_code ec) {
            if (!ec)
                self->do_read();
        });
    }

    // Accept a WebSocket upgrade from an already-read HTTP request.
    template<class Body, class Allocator>
    void start_with_request(const http::request<Body, http::basic_fields<Allocator>>& req)
    {
        apply_options();
        ws_.async_accept(req, [self = shared_from_this()](beast::error_code ec) {
            if (!ec)
                self->do_read();
        });
    }

    // Called from the engine thread (single producer).
    // Lock-free: pushes to SPSC ring, uses CAS + net::post to kick the
    // async_write chain on the io_context thread when needed.
    void send(const std::string& msg)
    {
        if (!open_.load(std::memory_order_acquire)) return;

        if (!ring_.try_push(msg))
            return;  // ring full — drop (backpressure)

        // If no async_write chain is active, start one on the io_context thread
        bool expected = false;
        if (writing_.compare_exchange_strong(expected, true,
                                             std::memory_order_acq_rel))
        {
            net::post(ioc_, [self = shared_from_this()]() {
                self->do_write();
            });
        }
    }

    bool is_open() const
    {
        return open_.load(std::memory_order_acquire);
    }

    // Event filter: set by "subscribe" command, checked by broadcast.
    // Thread safety: set from io_context thread (on_message), read from
    // engine thread (broadcast). Uses a mutex since updates are rare.
    void set_filter(const ws_event_filter& f)
    {
        std::lock_guard<std::mutex> lk(filter_mu_);
        filter_ = f;
    }

    bool passes_filter(const std::string& json) const
    {
        std::lock_guard<std::mutex> lk(filter_mu_);
        return filter_.matches(json);
    }

private:
    static constexpr std::size_t QUEUE_CAPACITY = 4096;

    websocket::stream<tcp::socket> ws_;
    net::io_context& ioc_;
    beast::flat_buffer buffer_;          // for do_read (io_context thread only)
    command_callback_t on_message_;
    bool compress_ = true;

    mutable std::mutex filter_mu_;
    ws_event_filter filter_;             // default: accept all

    // Lock-free outbound queue (SPSC: engine thread pushes, io_context pops)
    RingBuffer<std::string, QUEUE_CAPACITY> ring_;
    std::atomic<bool> open_{true};
    std::atomic<bool> writing_{false};   // true while an async_write chain is active
    std::string write_buf_;              // holds in-flight message (io_context thread only)

    void apply_options()
    {
        ws_.set_option(websocket::stream_base::timeout::suggested(
            beast::role_type::server));

        if (compress_)
        {
            websocket::permessage_deflate pmd;
            pmd.client_enable = true;
            pmd.server_enable = true;
            pmd.compLevel = 6;
            ws_.set_option(pmd);
        }
    }

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

    // Runs exclusively on the io_context thread (single consumer).
    // Pops one message from the ring, writes it, then chains to itself.
    // When the ring is empty, releases writing_ and posts a deferred
    // recheck to close the push-between-pop-and-release race window.
    void do_write()
    {
        if (!open_.load(std::memory_order_acquire))
        {
            writing_.store(false, std::memory_order_release);
            return;
        }

        if (!ring_.try_pop(write_buf_))
        {
            // Ring appears empty. Release the writing flag.
            writing_.store(false, std::memory_order_release);

            // A send() may have pushed between our try_pop and the flag
            // release, with its CAS failing because writing_ was still true.
            // Post a deferred recheck on the io_context to catch this case.
            // This runs after any pending net::post from send(), ensuring
            // we see the pushed data.
            net::post(ioc_, [self = shared_from_this()]() {
                if (self->ring_.empty()) return;
                bool expected = false;
                if (self->writing_.compare_exchange_strong(
                        expected, true, std::memory_order_acq_rel))
                {
                    self->do_write();
                }
            });
            return;
        }

        ws_.text(true);
        ws_.async_write(
            net::buffer(write_buf_),
            [self = shared_from_this()](beast::error_code ec, std::size_t) {
                if (ec)
                {
                    self->open_.store(false, std::memory_order_release);
                    self->writing_.store(false, std::memory_order_release);
                    return;
                }
                self->do_write();  // chain next message
            });
    }

    void close()
    {
        open_.store(false, std::memory_order_release);
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
    explicit WebSocketWorker(uint16_t port = 8765, bool compress = true)
        : port_(port)
        , compress_(compress)
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
    // Each session's event filter is checked — messages that don't
    // match a session's subscription are skipped for that session.
    void broadcast(const std::string& msg)
    {
        std::lock_guard<std::mutex> lk(sessions_mu_);

        // Clean up closed sessions while iterating
        auto it = sessions_.begin();
        while (it != sessions_.end())
        {
            if ((*it)->is_open())
            {
                if ((*it)->passes_filter(msg))
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

    // --- REST API ---

    // Access the backtest run manager (for engine to update run status).
    BacktestRunManager& run_manager() { return run_manager_; }

    // Register callback for backtest submissions via REST API.
    void set_on_backtest_submit(on_backtest_submit_fn fn)
    {
        std::lock_guard<std::mutex> lk(submit_mu_);
        on_backtest_submit_ = std::move(fn);
    }

    // Register callback for GET /api/runs (historical run list from SQLite).
    void set_on_list_runs(on_list_runs_fn fn)
    {
        std::lock_guard<std::mutex> lk(submit_mu_);
        on_list_runs_ = std::move(fn);
    }

    // L2: Register callback for GET /metrics (Prometheus text format).
    void set_on_metrics(on_metrics_fn fn)
    {
        std::lock_guard<std::mutex> lk(submit_mu_);
        on_metrics_ = std::move(fn);
    }

private:
    uint16_t port_;
    bool compress_;
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

    // REST API
    BacktestRunManager run_manager_;
    std::mutex submit_mu_;
    on_backtest_submit_fn on_backtest_submit_;
    on_list_runs_fn on_list_runs_;
    on_metrics_fn on_metrics_;

    void do_accept()
    {
        acceptor_.async_accept(
            [this](beast::error_code ec, tcp::socket socket) {
                if (!ec)
                    handle_new_connection(std::move(socket));

                // Continue accepting
                if (acceptor_.is_open())
                    do_accept();
            });
    }

    // Read the initial HTTP request, then either serve REST or upgrade to WebSocket.
    void handle_new_connection(tcp::socket socket)
    {
        // Shared state for the async read chain
        struct pending_conn : public std::enable_shared_from_this<pending_conn>
        {
            tcp::socket socket;
            beast::flat_buffer buffer;
            http::request<http::string_body> req;
            WebSocketWorker* owner;

            pending_conn(tcp::socket s, WebSocketWorker* o)
                : socket(std::move(s)), owner(o) {}
        };

        auto conn = std::make_shared<pending_conn>(std::move(socket), this);

        http::async_read(conn->socket, conn->buffer, conn->req,
            [this, conn](beast::error_code ec, std::size_t) {
                if (ec) return;

                // Check if this is a WebSocket upgrade
                if (beast::websocket::is_upgrade(conn->req))
                {
                    // Upgrade to WebSocket
                    auto session_holder = std::make_shared<std::shared_ptr<WsSession>>();
                    *session_holder = std::make_shared<WsSession>(
                        std::move(conn->socket), ioc_,
                        [this, session_holder](const std::string& msg) {
                            on_client_message(msg, *session_holder);
                        },
                        compress_
                    );
                    auto& session = *session_holder;
                    {
                        std::lock_guard<std::mutex> lk(sessions_mu_);
                        sessions_.insert(session);
                    }
                    session->start_with_request(conn->req);

                    // Notify engine
                    pending_connect_.store(true, std::memory_order_release);
                    {
                        std::lock_guard<std::mutex> lk(connect_mu_);
                        if (on_client_connect_)
                            on_client_connect_();
                    }
                }
                else
                {
                    // Handle as HTTP REST request
                    http::response<http::string_body> res;
                    res.version(conn->req.version());

                    on_backtest_submit_fn submit_fn;
                    on_list_runs_fn list_runs_fn;
                    on_metrics_fn metrics_fn;
                    {
                        std::lock_guard<std::mutex> lk(submit_mu_);
                        submit_fn = on_backtest_submit_;
                        list_runs_fn = on_list_runs_;
                        metrics_fn = on_metrics_;
                    }

                    route_http_request(conn->req, res, run_manager_, submit_fn, list_runs_fn, metrics_fn);

                    // Send HTTP response
                    auto sp = std::make_shared<http::response<http::string_body>>(std::move(res));
                    http::async_write(conn->socket, *sp,
                        [conn, sp](beast::error_code, std::size_t) {
                            beast::error_code shutdown_ec;
                            conn->socket.shutdown(tcp::socket::shutdown_send, shutdown_ec);
                        });
                }
            });
    }

    // Valid command names (used for schema validation)
    static constexpr const char* valid_commands_[] = {
        "start", "pause", "stop", "order", "set_timeframe",
        "set_symbol", "set_strategy", "query_fills", "backfill",
        "subscribe"
    };

    static bool is_valid_command(const std::string& cmd)
    {
        for (const auto* c : valid_commands_)
            if (cmd == c) return true;
        return false;
    }

    // Send an error response back to a specific session.
    void send_error(std::shared_ptr<WsSession>& session, const std::string& message)
    {
        char buf[512];
        std::snprintf(buf, sizeof(buf),
            R"({"type":"error","data":{"message":"%s","source":"ws_validator"}})",
            message.c_str());
        session->send(std::string(buf));
    }

    // Parse an incoming JSON command from a client.
    // Hand-rolled extraction (no JSON library, consistent with project conventions).
    // Validates field presence and types per command schema before enqueuing.
    void on_client_message(const std::string& raw,
                           std::shared_ptr<WsSession> session = {})
    {
        ws_command cmd;

        // Extract string field from JSON
        auto extract = [&](const char* key) -> std::string {
            std::string search = std::string("\"") + key + "\":\"";
            auto pos = raw.find(search);
            if (pos == std::string::npos) return {};
            pos += search.size();
            auto end = raw.find('"', pos);
            if (end == std::string::npos) return {};
            return raw.substr(pos, end - pos);
        };

        // Extract numeric field from JSON
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

        // Check for "command" or "cmd" field (accept both)
        cmd.command = extract("command");
        if (cmd.command.empty())
            cmd.command = extract("cmd");

        if (cmd.command.empty())
        {
            if (session)
                send_error(session, "missing required field: command");
            std::fprintf(stderr, "[ws] rejected: missing 'command' field: %.200s\n", raw.c_str());
            return;
        }

        if (!is_valid_command(cmd.command))
        {
            if (session)
                send_error(session, "unknown command: " + cmd.command);
            std::fprintf(stderr, "[ws] rejected: unknown command '%s'\n", cmd.command.c_str());
            return;
        }

        // Per-command schema validation
        if (cmd.command == "order")
        {
            cmd.side = extract("side");
            cmd.order_type = extract("type");
            cmd.quantity = extract_num("quantity");
            cmd.price = extract_num("price");

            if (cmd.side.empty())
            {
                if (session) send_error(session, "order: missing required field 'side'");
                std::fprintf(stderr, "[ws] rejected order: missing 'side'\n");
                return;
            }
            if (cmd.side != "buy" && cmd.side != "sell")
            {
                if (session) send_error(session, "order: 'side' must be 'buy' or 'sell'");
                std::fprintf(stderr, "[ws] rejected order: invalid side '%s'\n", cmd.side.c_str());
                return;
            }
            if (cmd.quantity <= 0.0)
            {
                if (session) send_error(session, "order: 'quantity' must be > 0");
                std::fprintf(stderr, "[ws] rejected order: invalid quantity\n");
                return;
            }
            if (cmd.order_type.empty())
                cmd.order_type = "market";  // default
            if (cmd.order_type != "market" && cmd.order_type != "limit")
            {
                if (session) send_error(session, "order: 'type' must be 'market' or 'limit'");
                std::fprintf(stderr, "[ws] rejected order: invalid type '%s'\n", cmd.order_type.c_str());
                return;
            }
            if (cmd.order_type == "limit" && cmd.price <= 0.0)
            {
                if (session) send_error(session, "order: limit order requires 'price' > 0");
                std::fprintf(stderr, "[ws] rejected order: limit without price\n");
                return;
            }
        }
        else if (cmd.command == "set_timeframe")
        {
            cmd.timeframe = extract("timeframe");
            if (cmd.timeframe.empty())
            {
                if (session) send_error(session, "set_timeframe: missing required field 'timeframe'");
                std::fprintf(stderr, "[ws] rejected set_timeframe: missing 'timeframe'\n");
                return;
            }
        }
        else if (cmd.command == "set_symbol")
        {
            cmd.value = extract("value");
            if (cmd.value.empty())
            {
                if (session) send_error(session, "set_symbol: missing required field 'value'");
                std::fprintf(stderr, "[ws] rejected set_symbol: missing 'value'\n");
                return;
            }
        }
        else if (cmd.command == "set_strategy")
        {
            cmd.value = extract("value");
            if (cmd.value.empty())
            {
                if (session) send_error(session, "set_strategy: missing required field 'value'");
                std::fprintf(stderr, "[ws] rejected set_strategy: missing 'value'\n");
                return;
            }
        }
        else if (cmd.command == "subscribe")
        {
            // Handle subscribe directly — apply filter to this session,
            // do not enqueue to the engine command queue.
            if (session)
            {
                ws_event_filter filter;

                // Parse "events":["fill","tick",...] — extract array items
                auto parse_array = [&](const char* key) -> std::set<std::string> {
                    std::set<std::string> result;
                    std::string search = std::string("\"") + key + "\":[";
                    auto pos = raw.find(search);
                    if (pos == std::string::npos) return result;
                    pos += search.size();
                    auto end = raw.find(']', pos);
                    if (end == std::string::npos) return result;
                    std::string arr = raw.substr(pos, end - pos);
                    // Extract quoted strings from the array
                    std::size_t p = 0;
                    while (p < arr.size())
                    {
                        auto q1 = arr.find('"', p);
                        if (q1 == std::string::npos) break;
                        auto q2 = arr.find('"', q1 + 1);
                        if (q2 == std::string::npos) break;
                        result.insert(arr.substr(q1 + 1, q2 - q1 - 1));
                        p = q2 + 1;
                    }
                    return result;
                };

                filter.event_types = parse_array("events");
                filter.symbols = parse_array("symbols");
                session->set_filter(filter);

                // Acknowledge
                char buf[256];
                std::snprintf(buf, sizeof(buf),
                    R"({"type":"subscribed","data":{"events":%zu,"symbols":%zu}})",
                    filter.event_types.size(), filter.symbols.size());
                session->send(std::string(buf));
            }
            return;  // don't enqueue to engine
        }
        else
        {
            // Commands like start, pause, stop, query_fills, backfill — no extra fields required
            cmd.timeframe = extract("timeframe");
            cmd.value = extract("value");
            cmd.price = extract_num("price");
        }

        std::lock_guard<std::mutex> lk(cmd_mu_);
        command_queue_.push(std::move(cmd));
    }
};

#endif // HAS_WEB_UI
