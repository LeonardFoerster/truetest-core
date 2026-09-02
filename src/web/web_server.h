#pragma once

#include "web_config.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>

struct mg_context;
struct mg_connection;

namespace truetest::ui { struct dashboard_snapshot; }

namespace truetest::web {

// Embedded read-only web UI server (civetweb, HAS_WEB only).
//
// Runs on its own thread; touches the engine only through the same
// snapshot_dashboard() seam the ncurses TUI uses. A background poller calls
// the snapshot callback at poll_hz, serializes a SnapshotFrame, and broadcasts
// it to every connected WebSocket client on /stream. REST endpoints serve the
// current frame and the final analytics report.
//
// Endpoints:
//   GET /                 → static SPA (when asset_dir is set)
//   GET /api/snapshot     → one-shot current SnapshotFrame
//   GET /api/results      → ResultsReport JSON after explicit safe publication
//   WS  /stream           → live SnapshotFrame at poll_hz (initial frame on connect)
//
// Nothing here can place orders — there are no control routes.
class WebServer
{
public:
    // Fills `out` with the current engine snapshot; returns false if no
    // snapshot is available yet (engine not started / not initialised).
    using snapshot_fn = std::function<bool(truetest::ui::dashboard_snapshot& out)>;
    // Returns an immutable/published ResultsReport JSON. The server must not
    // call this while the engine owns and mutates the underlying Analytics.
    using report_fn = std::function<std::string()>;
    using report_ready_fn = std::function<bool()>;

    WebServer(web_config cfg, snapshot_fn snap, report_fn report,
              report_ready_fn report_ready = {});
    ~WebServer();

    WebServer(const WebServer&) = delete;
    WebServer& operator=(const WebServer&) = delete;

    // Binds and starts serving. Returns false (with a stderr warning) if the
    // listen socket could not be bound; the engine run continues regardless.
    bool start();
    void stop();

    bool running() const { return running_.load(std::memory_order_acquire); }
    const web_config& config() const { return cfg_; }

private:
    // civetweb C callbacks (static; cbdata == this).
    static int  on_snapshot(mg_connection* c, void* cbdata);
    static int  on_results(mg_connection* c, void* cbdata);
    static int  ws_connect(const mg_connection* c, void* cbdata);
    static void ws_ready(mg_connection* c, void* cbdata);
    static int  ws_data(mg_connection* c, int bits, char* data, size_t len, void* cbdata);
    static void ws_close(const mg_connection* c, void* cbdata);

    bool authorized(const mg_connection* c, bool allow_query_token = false) const;
    bool origin_allowed(const mg_connection* c) const;
    void send_json(mg_connection* c, const std::string& body, const char* status = "200 OK") const;
    std::string current_snapshot_json() const;
    void broadcast_loop();
    void broadcast(const std::string& payload);

    web_config  cfg_;
    snapshot_fn snap_;
    report_fn   report_;
    report_ready_fn report_ready_;

    mg_context* ctx_ = nullptr;
    std::thread poller_;
    std::atomic<bool> running_{false};

    mutable std::mutex conns_mu_;
    std::set<mg_connection*> conns_;
};

} // namespace truetest::web
