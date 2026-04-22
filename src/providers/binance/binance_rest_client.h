#pragma once
#ifdef HAS_BINANCE

#include "providers/binance/binance_auth.h"
#include "providers/binance/binance_parser.h"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>

#include <atomic>
#include <chrono>
#include <climits>
#include <cstdint>
#include <functional>
#include <iostream>
#include <string>
#include <stdexcept>
#include <thread>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

class BinanceRestClient
{
public:
    BinanceRestClient(
        const std::string& api_key,
        const std::string& api_secret,
        const std::string& host = "api.binance.com",
        const std::string& port = "443")
        : api_key_(api_key)
        , api_secret_(api_secret)
        , host_(host)
        , port_(port)
        , ctx_(ssl::context::tlsv12_client)
    {
        ctx_.set_default_verify_paths();
        ctx_.set_verify_mode(ssl::verify_peer);
    }

    struct response
    {
        int status;
        std::string body;
        int used_weight = 0;
    };

    response post(const std::string& endpoint, const std::string& params)
    {
        return do_signed_request(http::verb::post, endpoint, params, /*in_query=*/false);
    }

    response get(const std::string& endpoint, const std::string& params)
    {
        return do_signed_request(http::verb::get, endpoint, params, /*in_query=*/true);
    }

    response del(const std::string& endpoint, const std::string& params)
    {
        return do_signed_request(http::verb::delete_, endpoint, params, /*in_query=*/true);
    }

    response post_unsigned(const std::string& endpoint)
    {
        return execute(http::verb::post, endpoint, "");
    }

    response put_unsigned(const std::string& endpoint, const std::string& params = "")
    {
        return execute(http::verb::put, endpoint + (params.empty() ? "" : "?" + params), "");
    }

    int last_used_weight() const { return last_used_weight_.load(); }

    // returns server_time_ms - local_time_ms; negative means we are ahead.
    // On any failure returns LLONG_MIN so callers can detect it.
    using get_fn_t = std::function<response(const std::string&, const std::string&)>;

    static long long server_time_offset_ms(const get_fn_t& get_fn)
    {
        if (!get_fn) return LLONG_MIN;
        try
        {
            auto resp = get_fn("/api/v3/time", "");
            if (resp.status < 200 || resp.status >= 300) return LLONG_MIN;
            auto sv = binance::extract_sv_number(resp.body, "serverTime");
            long long server_ms = 0;
            int64_t parsed = 0;
            if (!binance::parse_int64_sv(sv, parsed)) return LLONG_MIN;
            server_ms = static_cast<long long>(parsed);
            long long local_ms = static_cast<long long>(binance::server_time_ms());
            return server_ms - local_ms;
        }
        catch (...)
        {
            return LLONG_MIN;
        }
    }

    long long server_time_offset_ms()
    {
        return server_time_offset_ms(
            [this](const std::string& ep, const std::string& p) {
                // unsigned GET: do not sign, do not add timestamp.
                return execute(http::verb::get,
                               ep + (p.empty() ? "" : "?" + p),
                               "");
            });
    }

    // Throttle policy knobs (exposed for tests; not wired through config yet).
    void set_weight_cap(int cap) { weight_cap_ = cap; }
    void set_soft_threshold_pct(int pct) { soft_threshold_pct_ = pct; }

    // Clock-drift handling. Each signed request stamps
    //   timestamp = local_now_ms + clock_offset_ms_
    // where the offset is (server_ms - local_ms) learned from /api/v3/time.
    // Without this, a local clock that drifts past recvWindow (5s) starts
    // rejecting every signed call with -1021. The offset is refreshed
    // lazily — every `sync_interval_ms_` before a signed send — and
    // reactively when a -1021 response comes back (to catch NTP steps that
    // happen between lazy refreshes).

    void set_sync_interval_ms(long long ms) { sync_interval_ms_ = ms; }

    long long clock_offset_ms() const
    {
        return clock_offset_ms_.load(std::memory_order_acquire);
    }

    // Force a clock resync against /api/v3/time. Returns true on success.
    // Called at startup (BinanceProvider::open) to seed the offset, and
    // invoked reactively on -1021 responses.
    bool resync_clock_now()
    {
        auto offset = server_time_offset_ms(
            [this](const std::string& ep, const std::string& p) {
                return execute(http::verb::get,
                               ep + (p.empty() ? "" : "?" + p), "");
            });
        if (offset == LLONG_MIN)
        {
            if (!sync_failed_logged_.exchange(true))
                std::cerr << "BinanceRestClient: clock resync failed; "
                             "keeping offset " << clock_offset_ms_.load()
                          << " ms\n";
            return false;
        }
        clock_offset_ms_.store(offset, std::memory_order_release);
        last_sync_steady_ms_.store(steady_now_ms(), std::memory_order_release);
        sync_failed_logged_.store(false);
        return true;
    }

    // Pure decision so tests can exercise the lazy-resync rule without
    // a network: should we resync given elapsed steady-clock time?
    // last_sync_ms_ <= 0 means "never synced" → always due.
    static bool resync_due(long long now_steady_ms,
                           long long last_sync_steady_ms,
                           long long interval_ms)
    {
        if (last_sync_steady_ms <= 0) return true;
        if (interval_ms <= 0) return false;
        return (now_steady_ms - last_sync_steady_ms) >= interval_ms;
    }

    // Pure helper so tests can exercise the decision without a network.
    // Returns number of milliseconds to sleep before the next request;
    // 0 means no throttle needed.
    static long long throttle_delay_ms(int used_weight,
                                       long long anchor_ms,
                                       long long now_ms,
                                       int cap,
                                       int pct)
    {
        if (cap <= 0 || pct <= 0) return 0;
        if (used_weight <= 0) return 0;
        long long since = now_ms - anchor_ms;
        if (since < 0 || since >= 60'000) return 0;
        if (static_cast<long long>(used_weight) * 100 <
            static_cast<long long>(cap) * pct)
            return 0;
        long long remaining = 60'000 - since;
        if (remaining < 0) remaining = 0;
        return remaining;
    }

private:
    std::string api_key_;
    std::string api_secret_;
    std::string host_;
    std::string port_;
    ssl::context ctx_;
    std::atomic<int> last_used_weight_{0};
    std::atomic<long long> window_anchor_ms_{0};
    int weight_cap_ = 6000;
    int soft_threshold_pct_ = 80;

    std::atomic<long long> clock_offset_ms_{0};
    std::atomic<long long> last_sync_steady_ms_{0};
    long long sync_interval_ms_ = 5 * 60 * 1000;
    std::atomic<bool> sync_failed_logged_{false};

    static long long steady_now_ms()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    void maybe_resync_clock()
    {
        if (resync_due(steady_now_ms(),
                       last_sync_steady_ms_.load(std::memory_order_acquire),
                       sync_interval_ms_))
            resync_clock_now();
    }

    // Build + sign a query with the current offset, send it, and on a
    // -1021 "timestamp outside recvWindow" response resync once and retry.
    // Retrying requires rebuilding the signed query (new timestamp + new
    // signature), so the retry lives here above execute_with_retry rather
    // than inside it.
    response do_signed_request(http::verb method,
                               const std::string& endpoint,
                               const std::string& params,
                               bool in_query)
    {
        maybe_resync_clock();

        auto build_signed = [&]() {
            long long ts = static_cast<long long>(binance::server_time_ms())
                         + clock_offset_ms_.load(std::memory_order_acquire);
            auto q = params + "&timestamp=" + std::to_string(ts)
                     + "&recvWindow=5000";
            return binance::sign_query(q, api_secret_);
        };

        auto send = [&]() -> response {
            auto sq = build_signed();
            return in_query
                ? execute(method, endpoint + "?" + sq, "")
                : execute(method, endpoint, sq);
        };

        auto r = send();
        if (r.status >= 400 && r.body.find("-1021") != std::string::npos)
        {
            std::cerr << "BinanceRestClient: -1021 timestamp drift, "
                         "resyncing clock and retrying once\n";
            if (resync_clock_now())
                r = send();
        }
        return r;
    }

    response execute(http::verb method, const std::string& target, const std::string& body)
    {
        return execute_with_retry(method, target, body, /*retry_on_429=*/true);
    }

    response execute_with_retry(http::verb method,
                                const std::string& target,
                                const std::string& body,
                                bool retry_on_429)
    {
        // Proactive throttle: if we are near the weight cap, wait for the
        // 1-minute window to roll before firing the request.
        {
            long long now_ms = static_cast<long long>(binance::server_time_ms());
            long long delay = throttle_delay_ms(
                last_used_weight_.load(),
                window_anchor_ms_.load(),
                now_ms,
                weight_cap_,
                soft_threshold_pct_);
            if (delay > 0)
            {
                std::cerr << "BinanceRestClient: near weight cap, pausing "
                          << delay << " ms\n";
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                // Clear the anchor so a stale window does not throttle us again.
                window_anchor_ms_.store(0);
            }
        }

        try
        {
            net::io_context ioc;
            tcp::resolver resolver(ioc);
            auto results = resolver.resolve(host_, port_);

            beast::ssl_stream<tcp::socket> stream(ioc, ctx_);

            if (!SSL_set_tlsext_host_name(stream.native_handle(), host_.c_str()))
                throw std::runtime_error("SNI setup failed");

            auto& lowest = beast::get_lowest_layer(stream);
            net::connect(lowest, results);
            stream.handshake(ssl::stream_base::client);

            http::request<http::string_body> req{method, target, 11};
            req.set(http::field::host, host_);
            req.set(http::field::user_agent, "TrueTest/1.0");
            req.set("X-MBX-APIKEY", api_key_);

            if (!body.empty())
            {
                req.set(http::field::content_type, "application/x-www-form-urlencoded");
                req.body() = body;
                req.prepare_payload();
            }

            http::write(stream, req);

            beast::flat_buffer buffer;
            http::response<http::string_body> res;
            http::read(stream, buffer, res);

            response r;
            r.status = static_cast<int>(res.result_int());
            r.body = res.body();

            auto weight_it = res.find("X-MBX-USED-WEIGHT-1M");
            if (weight_it != res.end())
            {
                try { r.used_weight = std::stoi(std::string(weight_it->value())); }
                catch (...) {}
                last_used_weight_.store(r.used_weight);
                window_anchor_ms_.store(
                    static_cast<long long>(binance::server_time_ms()));
            }

            beast::error_code ec;
            stream.shutdown(ec);

            if (r.status == 429)
            {
                long long sleep_ms = 2000;
                auto retry_after = res.find("Retry-After");
                if (retry_after != res.end())
                {
                    try
                    {
                        long long sec = std::stoll(
                            std::string(retry_after->value()));
                        if (sec > 0) sleep_ms = std::min<long long>(sec * 1000,
                                                                    60'000);
                    }
                    catch (...) {}
                }
                std::cerr << "BinanceRestClient: rate limited (429). "
                          << "Weight used: " << r.used_weight
                          << ", sleeping " << sleep_ms << " ms\n";
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(sleep_ms));
                if (retry_on_429)
                    return execute_with_retry(method, target, body, false);
                return r;
            }
            else if (r.status >= 400)
            {
                std::cerr << "BinanceRestClient: HTTP " << r.status
                          << " - " << r.body << "\n";
            }

            return r;
        }
        catch (const std::exception& e)
        {
            std::cerr << "BinanceRestClient: request failed: " << e.what() << "\n";
            return {0, "", 0};
        }
    }
};

#endif // HAS_BINANCE
