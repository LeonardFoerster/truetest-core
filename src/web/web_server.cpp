#include "web_server.h"

#include "snapshot_json.h"
#include "../ui/dashboard_snapshot.h"

#include <civetweb.h>

#include <chrono>
#include <cstring>
#include <iostream>
#include <vector>

namespace truetest::web {

namespace {

// Compare a bearer/query token in constant-ish time (length-checked equality).
bool token_matches(const char* got, const std::string& want)
{
    if (!got) return false;
    return want == got;
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
bool WebServer::authorized(const mg_connection* c) const
{
    if (cfg_.token.empty()) return true;

    const mg_request_info* ri = mg_get_request_info(c);
    // Authorization: Bearer <token>
    if (const char* h = mg_get_header(c, "Authorization"))
    {
        const char* p = std::strstr(h, "Bearer ");
        if (p && token_matches(p + 7, cfg_.token)) return true;
    }
    // ?token=<token> fallback (browsers can't set headers on a WS handshake).
    if (ri && ri->query_string)
    {
        char buf[256];
        int n = mg_get_var(ri->query_string, std::strlen(ri->query_string),
                           "token", buf, sizeof(buf));
        if (n > 0 && token_matches(buf, cfg_.token)) return true;
    }
    return false;
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
              "Access-Control-Allow-Origin: *\r\n"
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
    return self->authorized(c) ? 0 : 1; // 0 = accept, non-zero = reject
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
