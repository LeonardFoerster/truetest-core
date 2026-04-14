#pragma once
#ifdef HAS_WEB_UI

#include <boost/beast.hpp>
#include <boost/beast/http.hpp>
#include <boost/asio.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

// Backtest run status tracking (used by REST API)
struct backtest_run
{
    std::string id;
    std::string config_json;
    std::string status;         // "pending", "running", "completed", "failed"
    std::string results_json;   // populated on completion
    std::chrono::system_clock::time_point started_at;
    std::chrono::system_clock::time_point ended_at;
};

// Manages backtest runs submitted via REST API.
// Thread-safe: runs are submitted from HTTP threads, updated from engine threads.
class BacktestRunManager
{
public:
    // Create a new run entry and return its ID.
    std::string create_run(const std::string& config_json)
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto id = std::to_string(next_id_++);
        backtest_run run;
        run.id = id;
        run.config_json = config_json;
        run.status = "pending";
        run.started_at = std::chrono::system_clock::now();
        runs_[id] = std::move(run);
        return id;
    }

    // Update run status (called from engine thread).
    void update_status(const std::string& id, const std::string& status)
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = runs_.find(id);
        if (it != runs_.end())
            it->second.status = status;
    }

    // Store results and mark completed.
    void complete_run(const std::string& id, const std::string& results_json)
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = runs_.find(id);
        if (it != runs_.end())
        {
            it->second.status = "completed";
            it->second.results_json = results_json;
            it->second.ended_at = std::chrono::system_clock::now();
        }
    }

    // Mark run as failed.
    void fail_run(const std::string& id, const std::string& error)
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = runs_.find(id);
        if (it != runs_.end())
        {
            it->second.status = "failed";
            it->second.results_json = R"({"error":")" + error + R"("})";
            it->second.ended_at = std::chrono::system_clock::now();
        }
    }

    // Get status JSON for a run. Returns empty string if not found.
    std::string get_status_json(const std::string& id) const
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = runs_.find(id);
        if (it == runs_.end()) return {};

        char buf[512];
        std::snprintf(buf, sizeof(buf),
            R"({"id":"%s","status":"%s","started_at":%lld})",
            it->second.id.c_str(),
            it->second.status.c_str(),
            static_cast<long long>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    it->second.started_at.time_since_epoch()).count()));
        return buf;
    }

    // Get results JSON for a run. Returns empty string if not found or not completed.
    std::string get_results_json(const std::string& id) const
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = runs_.find(id);
        if (it == runs_.end()) return {};
        if (it->second.status != "completed" && it->second.status != "failed")
            return R"({"id":")" + id + R"(","status":")" + it->second.status + R"("})";
        return it->second.results_json;
    }

    // List all runs as JSON array.
    std::string list_runs_json() const
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::string json = "[";
        bool first = true;
        for (const auto& [id, run] : runs_)
        {
            if (!first) json += ",";
            first = false;
            char buf[512];
            std::snprintf(buf, sizeof(buf),
                R"({"id":"%s","status":"%s","started_at":%lld})",
                run.id.c_str(), run.status.c_str(),
                static_cast<long long>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        run.started_at.time_since_epoch()).count()));
            json += buf;
        }
        json += "]";
        return json;
    }

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, backtest_run> runs_;
    uint64_t next_id_ = 1;
};

// Callback type: engine registers this to handle backtest submissions.
// Takes config JSON, returns run ID. The engine is responsible for
// spawning the backtest and updating the run manager.
using on_backtest_submit_fn = std::function<std::string(const std::string& config_json)>;

// Route an HTTP request and produce a response.
// Returns true if the request was handled (not a WebSocket upgrade).
// Returns false if this is a WebSocket upgrade request (caller should proceed with WS).
template<class Body, class Allocator>
bool route_http_request(
    const http::request<Body, http::basic_fields<Allocator>>& req,
    http::response<http::string_body>& res,
    BacktestRunManager& run_manager,
    const on_backtest_submit_fn& on_submit)
{
    auto target = std::string(req.target());

    // WebSocket upgrade — let the caller handle it
    if (beast::websocket::is_upgrade(req))
        return false;

    // Set common headers
    res.set(http::field::server, "TrueTest");
    res.set(http::field::content_type, "application/json");
    res.set(http::field::access_control_allow_origin, "*");
    res.set(http::field::access_control_allow_methods, "GET, POST, OPTIONS");
    res.set(http::field::access_control_allow_headers, "Content-Type");
    res.keep_alive(req.keep_alive());

    // CORS preflight
    if (req.method() == http::verb::options)
    {
        res.result(http::status::no_content);
        res.prepare_payload();
        return true;
    }

    // POST /api/backtest — submit a new backtest
    if (req.method() == http::verb::post && target == "/api/backtest")
    {
        auto body = req.body();
        if (body.empty())
        {
            res.result(http::status::bad_request);
            res.body() = R"({"error":"empty request body"})";
            res.prepare_payload();
            return true;
        }

        if (on_submit)
        {
            auto run_id = on_submit(body);
            if (!run_id.empty())
            {
                res.result(http::status::accepted);
                res.body() = R"({"id":")" + run_id + R"(","status":"pending"})";
            }
            else
            {
                res.result(http::status::internal_server_error);
                res.body() = R"({"error":"failed to create backtest run"})";
            }
        }
        else
        {
            res.result(http::status::service_unavailable);
            res.body() = R"({"error":"backtest submission not available"})";
        }
        res.prepare_payload();
        return true;
    }

    // GET /api/backtest — list all runs
    if (req.method() == http::verb::get && target == "/api/backtest")
    {
        res.result(http::status::ok);
        res.body() = run_manager.list_runs_json();
        res.prepare_payload();
        return true;
    }

    // GET /api/backtest/<id>/status
    if (req.method() == http::verb::get && target.rfind("/api/backtest/", 0) == 0)
    {
        // Extract ID from path
        auto rest = target.substr(14); // after "/api/backtest/"
        auto slash = rest.find('/');
        std::string id = (slash == std::string::npos) ? rest : rest.substr(0, slash);
        std::string action = (slash == std::string::npos) ? "" : rest.substr(slash + 1);

        if (id.empty())
        {
            res.result(http::status::bad_request);
            res.body() = R"({"error":"missing run id"})";
            res.prepare_payload();
            return true;
        }

        if (action.empty() || action == "status")
        {
            auto json = run_manager.get_status_json(id);
            if (json.empty())
            {
                res.result(http::status::not_found);
                res.body() = R"({"error":"run not found"})";
            }
            else
            {
                res.result(http::status::ok);
                res.body() = json;
            }
        }
        else if (action == "results")
        {
            auto json = run_manager.get_results_json(id);
            if (json.empty())
            {
                res.result(http::status::not_found);
                res.body() = R"({"error":"run not found"})";
            }
            else
            {
                res.result(http::status::ok);
                res.body() = json;
            }
        }
        else
        {
            res.result(http::status::not_found);
            res.body() = R"({"error":"unknown endpoint"})";
        }
        res.prepare_payload();
        return true;
    }

    // GET /api/health — simple health check
    if (req.method() == http::verb::get && target == "/api/health")
    {
        res.result(http::status::ok);
        res.body() = R"({"status":"ok"})";
        res.prepare_payload();
        return true;
    }

    // 404 for everything else
    res.result(http::status::not_found);
    res.body() = R"({"error":"not found"})";
    res.prepare_payload();
    return true;
}

#endif // HAS_WEB_UI
