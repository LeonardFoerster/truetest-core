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

struct backtest_run
{
    std::string id;
    std::string config_json;
    std::string status;
    std::string results_json;
    std::chrono::system_clock::time_point started_at;
    std::chrono::system_clock::time_point ended_at;
};

class BacktestRunManager
{
public:
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

    void update_status(const std::string& id, const std::string& status)
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = runs_.find(id);
        if (it != runs_.end())
            it->second.status = status;
    }

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

    std::string get_results_json(const std::string& id) const
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = runs_.find(id);
        if (it == runs_.end()) return {};
        if (it->second.status != "completed" && it->second.status != "failed")
            return R"({"id":")" + id + R"(","status":")" + it->second.status + R"("})";
        return it->second.results_json;
    }

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

using on_backtest_submit_fn = std::function<std::string(const std::string& config_json)>;

using on_list_runs_fn = std::function<std::string(int limit)>;

using on_metrics_fn = std::function<std::string()>;

template<class Body, class Allocator>
bool route_http_request(
    const http::request<Body, http::basic_fields<Allocator>>& req,
    http::response<http::string_body>& res,
    BacktestRunManager& run_manager,
    const on_backtest_submit_fn& on_submit,
    const on_list_runs_fn& on_list_runs = nullptr,
    const on_metrics_fn& on_metrics = nullptr)
{
    auto target = std::string(req.target());

    if (beast::websocket::is_upgrade(req))
        return false;

    res.set(http::field::server, "TrueTest");
    res.set(http::field::content_type, "application/json");
    res.set(http::field::access_control_allow_origin, "*");
    res.set(http::field::access_control_allow_methods, "GET, POST, OPTIONS");
    res.set(http::field::access_control_allow_headers, "Content-Type");
    res.keep_alive(req.keep_alive());

    if (req.method() == http::verb::options)
    {
        res.result(http::status::no_content);
        res.prepare_payload();
        return true;
    }

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

    if (req.method() == http::verb::get && target == "/api/backtest")
    {
        res.result(http::status::ok);
        res.body() = run_manager.list_runs_json();
        res.prepare_payload();
        return true;
    }

    if (req.method() == http::verb::get && target.rfind("/api/backtest/", 0) == 0)
    {
        auto rest = target.substr(14);
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

    if (req.method() == http::verb::get && target.rfind("/api/runs", 0) == 0)
    {
        int limit = 100;
        auto qpos = target.find('?');
        if (qpos != std::string::npos)
        {
            auto qs = target.substr(qpos + 1);
            auto eq = qs.find("limit=");
            if (eq != std::string::npos)
            {
                try { limit = std::stoi(qs.substr(eq + 6)); } catch (...) {}
                if (limit <= 0 || limit > 1000) limit = 100;
            }
        }

        if (on_list_runs)
        {
            auto body = on_list_runs(limit);
            if (body.empty()) body = "[]";
            res.result(http::status::ok);
            res.body() = body;
        }
        else
        {
            res.result(http::status::service_unavailable);
            res.body() = R"X({"error":"run history not available (SQLite disabled)"})X";
        }
        res.prepare_payload();
        return true;
    }

    if (req.method() == http::verb::get && target == "/api/health")
    {
        res.result(http::status::ok);
        res.body() = R"({"status":"ok"})";
        res.prepare_payload();
        return true;
    }

    if (req.method() == http::verb::get && target == "/metrics")
    {
        res.set(http::field::content_type, "text/plain; version=0.0.4");
        if (on_metrics)
        {
            auto body = on_metrics();
            if (body.empty())
            {
                res.result(http::status::service_unavailable);
                res.body() = "# metrics unavailable\n";
            }
            else
            {
                res.result(http::status::ok);
                res.body() = std::move(body);
            }
        }
        else
        {
            res.result(http::status::service_unavailable);
            res.body() = "# metrics callback not registered\n";
        }
        res.prepare_payload();
        return true;
    }

    res.result(http::status::not_found);
    res.body() = R"({"error":"not found"})";
    res.prepare_payload();
    return true;
}

#endif // HAS_WEB_UI
