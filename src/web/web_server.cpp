#include "web_server.h"

#include "snapshot_json.h"
#include "../ui/dashboard_snapshot.h"

#include <civetweb.h>

#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace truetest::web {

namespace {

// Upper bound on simultaneous /stream subscribers. The broadcast loop writes
// to each client serially, so an unbounded client set would let a flood (or a
// few slow clients) stall the cadence; this caps the blast radius. Operator
// tooling on localhost needs only a handful.
constexpr std::size_t max_ws_clients = 16;

// Compare a bearer/query token in constant-ish time (length-checked equality).
bool token_matches(const char* got, const std::string& want)
{
    if (!got) return false;
    return want == got;
}

bool starts_with(const std::string& s, const char* prefix)
{
    const std::size_t n = std::strlen(prefix);
    return s.size() >= n && s.compare(0, n, prefix) == 0;
}

} // namespace

WebServer::WebServer(web_config cfg, snapshot_fn snap, report_fn report)
    : cfg_(std::move(cfg)), snap_(std::move(snap)), report_(std::move(report))
{
}

WebServer::~WebServer()
{
    stop();
}

bool WebServer::start()
{
    if (running_.load()) return true;

    mg_init_library(0);

    const std::string listen = cfg_.listen_spec();
    const std::string threads = "6";

    std::vector<const char*> options = {
        "listening_ports", listen.c_str(),
        "num_threads",     threads.c_str(),
        "enable_directory_listing", "no",
        "tcp_nodelay",     "1",
    };
    if (!cfg_.asset_dir.empty())
    {
        options.push_back("document_root");
        options.push_back(cfg_.asset_dir.c_str());
        options.push_back("index_files");
        options.push_back("index.html");
    }
    options.push_back(nullptr);

    mg_callbacks callbacks;
    std::memset(&callbacks, 0, sizeof(callbacks));

    ctx_ = mg_start(&callbacks, this, options.data());
    if (!ctx_)
    {
        std::cerr << "  ! web: failed to bind " << listen
                  << " — web UI disabled (run continues)\n";
        mg_exit_library();
        return false;
    }

    mg_set_request_handler(ctx_, "/api/snapshot", &WebServer::on_snapshot, this);
    mg_set_request_handler(ctx_, "/api/results",  &WebServer::on_results,  this);
    mg_set_websocket_handler(ctx_, "/stream",
                             &WebServer::ws_connect, &WebServer::ws_ready,
                             &WebServer::ws_data,    &WebServer::ws_close, this);

    running_.store(true, std::memory_order_release);
    poller_ = std::thread(&WebServer::broadcast_loop, this);

    std::cerr << "  + web UI serving on " << cfg_.url()
              << (cfg_.asset_dir.empty() ? " (API/WS only)" : "") << "\n";
    return true;
}

void WebServer::stop()
{
    if (!running_.exchange(false)) return;

    if (poller_.joinable()) poller_.join();

    if (ctx_)
    {
        mg_stop(ctx_);
        ctx_ = nullptr;
        mg_exit_library();
    }
    std::lock_guard<std::mutex> lk(conns_mu_);
    conns_.clear();
}

// ── auth ────────────────────────────────────────────────────────────────────
bool WebServer::authorized(const mg_connection* c, bool allow_query_token) const
{
    if (cfg_.token.empty()) return true;

    const mg_request_info* ri = mg_get_request_info(c);
    // Authorization: Bearer <token>
    if (const char* h = mg_get_header(c, "Authorization"))
    {
        const char* p = std::strstr(h, "Bearer ");
        if (p && token_matches(p + 7, cfg_.token)) return true;
    }
    // ?token=<token> fallback is allowed only where browser APIs cannot send
    // headers (WebSocket handshake). REST uses Authorization to keep tokens
    // out of URLs, logs and browser history.
    if (allow_query_token && ri && ri->query_string)
    {
        char buf[256];
        int n = mg_get_var(ri->query_string, std::strlen(ri->query_string),
                           "token", buf, sizeof(buf));
        if (n > 0 && token_matches(buf, cfg_.token)) return true;
    }
    return false;
}

bool WebServer::origin_allowed(const mg_connection* c) const
{
    const char* origin = mg_get_header(c, "Origin");
    if (!origin || !*origin) return true; // CLI/local non-browser clients.

    const char* host = mg_get_header(c, "Host");
    if (!host || !*host) return false;

    const std::string o(origin);
    return o == "http://" + std::string(host)
        || o == "https://" + std::string(host)
        || (starts_with(o, "http://localhost:")
            && cfg_.bind_addr == "127.0.0.1")
        || (starts_with(o, "http://127.0.0.1:")
            && cfg_.bind_addr == "localhost");
}

// ── serialization helpers ────────────────────────────────────────────────────
std::string WebServer::current_snapshot_json() const
{
    truetest::ui::dashboard_snapshot snap;
    if (snap_ && snap_(snap)) return snapshot_to_json(snap);
    return std::string();
}

void WebServer::send_json(mg_connection* c, const std::string& body, const char* status) const
{
    mg_printf(c,
              "HTTP/1.1 %s\r\n"
              "Content-Type: application/json\r\n"
              "Content-Length: %zu\r\n"
              "Cache-Control: no-store\r\n"
              "Connection: close\r\n\r\n",
              status, body.size());
    if (!body.empty()) mg_write(c, body.data(), body.size());
}

// ── REST handlers ─────────────────────────────────────────────────────────────
int WebServer::on_snapshot(mg_connection* c, void* cbdata)
{
    auto* self = static_cast<WebServer*>(cbdata);
    if (!self->authorized(c)) { self->send_json(c, "{\"error\":\"unauthorized\"}", "401 Unauthorized"); return 1; }
    std::string body = self->current_snapshot_json();
    if (body.empty()) body = "{\"error\":\"no snapshot\"}";
    self->send_json(c, body);
    return 1;
}

int WebServer::on_results(mg_connection* c, void* cbdata)
{
    auto* self = static_cast<WebServer*>(cbdata);
    if (!self->authorized(c)) { self->send_json(c, "{\"error\":\"unauthorized\"}", "401 Unauthorized"); return 1; }
    std::string body = self->report_ ? self->report_() : std::string("{}");
    self->send_json(c, body);
    return 1;
}

// ── WebSocket handlers ───────────────────────────────────────────────────────
int WebServer::ws_connect(const mg_connection* c, void* cbdata)
{
    auto* self = static_cast<WebServer*>(cbdata);
    if (!self->origin_allowed(c)) return 1; // reject
    if (!self->authorized(c, /*allow_query_token=*/true)) return 1; // reject
    std::lock_guard<std::mutex> lk(self->conns_mu_);
    if (self->conns_.size() >= max_ws_clients) return 1; // backpressure: cap subscribers
    return 0; // accept
}

void WebServer::ws_ready(mg_connection* c, void* cbdata)
{
    auto* self = static_cast<WebServer*>(cbdata);
    std::lock_guard<std::mutex> lk(self->conns_mu_);
    self->conns_.insert(c);
    // Hydrate the client immediately so it doesn't wait for the next tick.
    const std::string js = self->current_snapshot_json();
    if (!js.empty())
        mg_websocket_write(c, MG_WEBSOCKET_OPCODE_TEXT, js.data(), js.size());
}

int WebServer::ws_data(mg_connection*, int, char*, size_t, void*)
{
    // Read-only feed: ignore anything the client sends, keep the socket open.
    return 1;
}

void WebServer::ws_close(const mg_connection* c, void* cbdata)
{
    auto* self = static_cast<WebServer*>(cbdata);
    std::lock_guard<std::mutex> lk(self->conns_mu_);
    self->conns_.erase(const_cast<mg_connection*>(c));
}

// ── broadcast ─────────────────────────────────────────────────────────────────
void WebServer::broadcast(const std::string& payload)
{
    if (payload.empty()) return;
    // The lock is intentionally held across the writes: civetweb keeps a
    // connection object alive until its close handler returns, and ws_close
    // takes the same mutex, so holding it here prevents a connection from being
    // torn down (and freed) mid-write. With max_ws_clients capped and localhost
    // framing, the serial writes are cheap.
    std::lock_guard<std::mutex> lk(conns_mu_);
    for (mg_connection* c : conns_)
        mg_websocket_write(c, MG_WEBSOCKET_OPCODE_TEXT, payload.data(), payload.size());
}

void WebServer::broadcast_loop()
{
    const int hz = cfg_.poll_hz > 0 ? cfg_.poll_hz : 10;
    const auto period = std::chrono::milliseconds(1000 / hz);
    while (running_.load(std::memory_order_acquire))
    {
        bool has_clients;
        {
            std::lock_guard<std::mutex> lk(conns_mu_);
            has_clients = !conns_.empty();
        }
        // Only pay for serialization when someone is listening.
        if (has_clients) broadcast(current_snapshot_json());
        std::this_thread::sleep_for(period);
    }
}

} // namespace truetest::web
