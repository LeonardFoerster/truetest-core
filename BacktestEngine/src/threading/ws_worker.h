#pragma once
#ifdef HAS_WEB_UI

#include "worker.h"
#include "ring_buffer.h"
#include "http_handler.h"
#include "../core/event_json.h"
#include "../utils/log/logger.h"

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

struct ws_command
{
    std::string command;
    std::string side;
    double quantity = 0.0;
    double price = 0.0;
    std::string order_type;
    std::string timeframe;
    std::string value;
};

using on_client_connect_fn = std::function<void()>;

struct ws_event_filter
{
    std::set<std::string> event_types;
    std::set<std::string> symbols;

    bool accepts_all() const { return event_types.empty() && symbols.empty(); }

    bool matches(const std::string& json) const
    {
        if (accepts_all()) return true;

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
        }

        return true;
    }
};

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

    template<class Body, class Allocator>
    void start_with_request(const http::request<Body, http::basic_fields<Allocator>>& req)
    {
        apply_options();
        ws_.async_accept(req, [self = shared_from_this()](beast::error_code ec) {
            if (!ec)
                self->do_read();
        });
    }

    void send(const std::string& msg)
    {
        if (!open_.load(std::memory_order_acquire)) return;

        if (!ring_.try_push(msg))
            return;

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
    beast::flat_buffer buffer_;
    command_callback_t on_message_;
    bool compress_ = true;

    mutable std::mutex filter_mu_;
    ws_event_filter filter_;

    RingBuffer<std::string, QUEUE_CAPACITY> ring_;
    std::atomic<bool> open_{true};
    std::atomic<bool> writing_{false};
    std::string write_buf_;

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
        if (!open_.load(std::memory_order_acquire))
        {
            writing_.store(false, std::memory_order_release);
            return;
        }

        if (!ring_.try_pop(write_buf_))
        {
            writing_.store(false, std::memory_order_release);

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
                self->do_write();
            });
    }

    void close()
    {
        open_.store(false, std::memory_order_release);
    }
};

class WebSocketWorker : public Worker
{
public:
    explicit WebSocketWorker(uint16_t port = 8765, bool compress = true)
        : port_(port)
        , compress_(compress)
        , acceptor_(ioc_, tcp::endpoint(tcp::v4(), port))
    {
        do_accept();

        io_thread_ = std::thread([this]() {
            ioc_.run();
        });
    }

    ~WebSocketWorker()
    {
        ioc_.stop();
        if (io_thread_.joinable())
            io_thread_.join();
    }

    const char* worker_name() const override { return "websocket"; }

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


    bool poll_command(ws_command& cmd)
    {
        std::lock_guard<std::mutex> lk(cmd_mu_);
        if (command_queue_.empty()) return false;
        cmd = std::move(command_queue_.front());
        command_queue_.pop();
        return true;
    }

    void broadcast(const std::string& msg)
    {
        std::lock_guard<std::mutex> lk(sessions_mu_);

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

    void set_on_client_connect(on_client_connect_fn fn)
    {
        std::lock_guard<std::mutex> lk(connect_mu_);
        on_client_connect_ = std::move(fn);
    }

    bool has_pending_connect()
    {
        return pending_connect_.exchange(false, std::memory_order_acquire);
    }


    BacktestRunManager& run_manager() { return run_manager_; }

    void set_on_backtest_submit(on_backtest_submit_fn fn)
    {
        std::lock_guard<std::mutex> lk(submit_mu_);
        on_backtest_submit_ = std::move(fn);
    }

    void set_on_list_runs(on_list_runs_fn fn)
    {
        std::lock_guard<std::mutex> lk(submit_mu_);
        on_list_runs_ = std::move(fn);
    }

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

    std::mutex cmd_mu_;
    std::queue<ws_command> command_queue_;

    std::mutex connect_mu_;
    on_client_connect_fn on_client_connect_;
    std::atomic<bool> pending_connect_{false};

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

                if (acceptor_.is_open())
                    do_accept();
            });
    }

    void handle_new_connection(tcp::socket socket)
    {
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

                if (beast::websocket::is_upgrade(conn->req))
                {
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

                    pending_connect_.store(true, std::memory_order_release);
                    {
                        std::lock_guard<std::mutex> lk(connect_mu_);
                        if (on_client_connect_)
                            on_client_connect_();
                    }
                }
                else
                {
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

                    auto sp = std::make_shared<http::response<http::string_body>>(std::move(res));
                    http::async_write(conn->socket, *sp,
                        [conn, sp](beast::error_code, std::size_t) {
                            beast::error_code shutdown_ec;
                            conn->socket.shutdown(tcp::socket::shutdown_send, shutdown_ec);
                        });
                }
            });
    }

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

    void send_error(std::shared_ptr<WsSession>& session, const std::string& message)
    {
        char buf[512];
        std::snprintf(buf, sizeof(buf),
            R"({"type":"error","data":{"message":"%s","source":"ws_validator"}})",
            message.c_str());
        session->send(std::string(buf));
    }

    static bool has_control_chars(const std::string& s)
    {
        for (unsigned char c : s)
            if (c < 0x20 && c != '\t') return true;
        return false;
    }

    void on_client_message(const std::string& raw,
                           std::shared_ptr<WsSession> session = {})
    {
        static constexpr std::size_t MAX_MSG_LEN = 4096;
        if (raw.size() > MAX_MSG_LEN)
        {
            if (session)
                send_error(session, "message too large (max 4096 bytes)");
            LOG_WARN("ws", "rejected: message too large (%zu bytes)", raw.size());
            return;
        }

        if (raw.find('\0') != std::string::npos)
        {
            if (session)
                send_error(session, "message contains null bytes");
            LOG_WARN("ws", "rejected: message contains null bytes");
            return;
        }

        ws_command cmd;

        auto extract = [&](const char* key) -> std::string {
            std::string search = std::string("\"") + key + "\":\"";
            auto pos = raw.find(search);
            if (pos == std::string::npos) return {};
            pos += search.size();
            auto end = raw.find('"', pos);
            if (end == std::string::npos) return {};
            return raw.substr(pos, end - pos);
        };

        auto extract_num = [&](const char* key, bool& valid) -> double {
            valid = true;
            std::string search = std::string("\"") + key + "\":";
            auto pos = raw.find(search);
            if (pos == std::string::npos) return 0.0;
            pos += search.size();
            while (pos < raw.size() && (raw[pos] == ' ' || raw[pos] == '\t')) pos++;
            if (pos >= raw.size()) { valid = false; return 0.0; }
            char first = raw[pos];
            if (first != '-' && first != '.' && !(first >= '0' && first <= '9'))
            {
                valid = false;
                return 0.0;
            }
            try { return std::stod(raw.substr(pos)); }
            catch (...) { valid = false; return 0.0; }
        };

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

        if (has_control_chars(cmd.command))
        {
            if (session) send_error(session, "command contains control characters");
            LOG_WARN("ws", "rejected: command contains control characters");
            return;
        }

        if (cmd.command == "order")
        {
            cmd.side = extract("side");
            cmd.order_type = extract("type");
            bool qty_valid = true, price_valid = true;
            cmd.quantity = extract_num("quantity", qty_valid);
            cmd.price = extract_num("price", price_valid);

            if (!qty_valid)
            {
                if (session) send_error(session, "order: 'quantity' is not a valid number");
                LOG_WARN("ws", "rejected order: non-numeric quantity");
                return;
            }
            if (!price_valid)
            {
                if (session) send_error(session, "order: 'price' is not a valid number");
                LOG_WARN("ws", "rejected order: non-numeric price");
                return;
            }
            if (cmd.side.empty())
            {
                if (session) send_error(session, "order: missing required field 'side'");
                LOG_WARN("ws", "rejected order: missing 'side'");
                return;
            }
            if (has_control_chars(cmd.side))
            {
                if (session) send_error(session, "order: 'side' contains control characters");
                LOG_WARN("ws", "rejected order: side contains control characters");
                return;
            }
            if (cmd.side != "buy" && cmd.side != "sell")
            {
                if (session) send_error(session, "order: 'side' must be 'buy' or 'sell'");
                LOG_WARN("ws", "rejected order: invalid side '%s'", cmd.side.c_str());
                return;
            }
            if (cmd.quantity <= 0.0)
            {
                if (session) send_error(session, "order: 'quantity' must be > 0");
                LOG_WARN("ws", "rejected order: invalid quantity");
                return;
            }
            if (cmd.order_type.empty())
                cmd.order_type = "market";
            if (has_control_chars(cmd.order_type))
            {
                if (session) send_error(session, "order: 'type' contains control characters");
                LOG_WARN("ws", "rejected order: type contains control characters");
                return;
            }
            if (cmd.order_type != "market" && cmd.order_type != "limit")
            {
                if (session) send_error(session, "order: 'type' must be 'market' or 'limit'");
                LOG_WARN("ws", "rejected order: invalid type '%s'", cmd.order_type.c_str());
                return;
            }
            if (cmd.order_type == "limit" && cmd.price <= 0.0)
            {
                if (session) send_error(session, "order: limit order requires 'price' > 0");
                LOG_WARN("ws", "rejected order: limit without price");
                return;
            }
        }
        else if (cmd.command == "set_timeframe")
        {
            cmd.timeframe = extract("timeframe");
            if (cmd.timeframe.empty())
            {
                if (session) send_error(session, "set_timeframe: missing required field 'timeframe'");
                LOG_WARN("ws", "rejected set_timeframe: missing 'timeframe'");
                return;
            }
            if (has_control_chars(cmd.timeframe))
            {
                if (session) send_error(session, "set_timeframe: 'timeframe' contains control characters");
                LOG_WARN("ws", "rejected set_timeframe: control characters");
                return;
            }
        }
        else if (cmd.command == "set_symbol")
        {
            cmd.value = extract("value");
            if (cmd.value.empty())
            {
                if (session) send_error(session, "set_symbol: missing required field 'value'");
                LOG_WARN("ws", "rejected set_symbol: missing 'value'");
                return;
            }
            if (has_control_chars(cmd.value))
            {
                if (session) send_error(session, "set_symbol: 'value' contains control characters");
                LOG_WARN("ws", "rejected set_symbol: control characters");
                return;
            }
        }
        else if (cmd.command == "set_strategy")
        {
            cmd.value = extract("value");
            if (cmd.value.empty())
            {
                if (session) send_error(session, "set_strategy: missing required field 'value'");
                LOG_WARN("ws", "rejected set_strategy: missing 'value'");
                return;
            }
            if (has_control_chars(cmd.value))
            {
                if (session) send_error(session, "set_strategy: 'value' contains control characters");
                LOG_WARN("ws", "rejected set_strategy: control characters");
                return;
            }
        }
        else if (cmd.command == "subscribe")
        {
            if (session)
            {
                ws_event_filter filter;

                auto parse_array = [&](const char* key) -> std::set<std::string> {
                    std::set<std::string> result;
                    std::string search = std::string("\"") + key + "\":[";
                    auto pos = raw.find(search);
                    if (pos == std::string::npos) return result;
                    pos += search.size();
                    auto end = raw.find(']', pos);
                    if (end == std::string::npos) return result;
                    std::string arr = raw.substr(pos, end - pos);
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

                char buf[256];
                std::snprintf(buf, sizeof(buf),
                    R"({"type":"subscribed","data":{"events":%zu,"symbols":%zu}})",
                    filter.event_types.size(), filter.symbols.size());
                session->send(std::string(buf));
            }
            return;
        }
        else
        {
            cmd.timeframe = extract("timeframe");
            cmd.value = extract("value");
            bool price_ok = true;
            cmd.price = extract_num("price", price_ok);
        }

        std::lock_guard<std::mutex> lk(cmd_mu_);
        command_queue_.push(std::move(cmd));
    }
};

#endif // HAS_WEB_UI
