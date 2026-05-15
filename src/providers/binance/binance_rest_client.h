#pragma once
#ifdef HAS_BINANCE

#include "providers/binance/binance_auth.h"
#include "providers/binance/binance_parser.h"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>

#include <openssl/ssl.h>

#include <atomic>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <stdexcept>
#include <thread>

#include <sys/socket.h>
#include <sys/time.h>

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
        const std::string& port = "443",
        const std::string& time_path = "/api/v3/time")
        : api_key_(api_key)
        , api_secret_(api_secret)
        , host_(host)
        , port_(port)
        , time_path_(time_path)
        , ctx_(ssl::context::tlsv12_client)
        , signer_(api_secret)
    {
        ctx_.set_default_verify_paths();
        ctx_.set_verify_mode(ssl::verify_peer);

        // TLS session resumption: every signed request opens a new
        // connection (no keep-alive in execute_with_retry), so a cached
        // session skips the asymmetric-crypto round of every subsequent
        // handshake. NO_INTERNAL_STORE because we own the cache (one slot).
        // Boost.Asio reserves SSL_CTX ex_data slot 0 (used by app_data); we
        // allocate a private index so our `this` pointer doesn't clobber it.
        SSL_CTX* raw = ctx_.native_handle();
        SSL_CTX_set_session_cache_mode(
            raw, SSL_SESS_CACHE_CLIENT | SSL_SESS_CACHE_NO_INTERNAL_STORE);
        SSL_CTX_set_ex_data(raw, ex_data_index(), this);
        SSL_CTX_sess_set_new_cb(raw, &BinanceRestClient::on_new_session);
    }

    ~BinanceRestClient()
    {
        SSL_SESSION* sess = cached_session_.exchange(
            nullptr, std::memory_order_acq_rel);
        if (sess) SSL_SESSION_free(sess);
    }

    BinanceRestClient(const BinanceRestClient&) = delete;
    BinanceRestClient& operator=(const BinanceRestClient&) = delete;
    BinanceRestClient(BinanceRestClient&&) = delete;
    BinanceRestClient& operator=(BinanceRestClient&&) = delete;

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

    response get_unsigned(const std::string& endpoint, const std::string& params = "")
    {
        return execute(http::verb::get,
                       endpoint + (params.empty() ? "" : "?" + params),
                       "");
    }

    response put_unsigned(const std::string& endpoint, const std::string& params = "")
    {
        return execute(http::verb::put, endpoint + (params.empty() ? "" : "?" + params), "");
    }

    int last_used_weight() const { return last_used_weight_.load(); }

    // server_time_ms - local_time_ms; negative = we're ahead. LLONG_MIN on failure.
    using get_fn_t = std::function<response(const std::string&, const std::string&)>;

    // `time_path` defaults to spot's `/api/v3/time`; futures passes
    // `/fapi/v1/time`. Tests inject a get_fn that ignores the path arg.
    static long long server_time_offset_ms(
        const get_fn_t& get_fn,
        const std::string& time_path = "/api/v3/time")
    {
        if (!get_fn) return LLONG_MIN;
        try
        {
            auto resp = get_fn(time_path, "");
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
                return execute(http::verb::get,
                               ep + (p.empty() ? "" : "?" + p),
                               "");
            },
            time_path_);
    }

    void set_weight_cap(int cap) { weight_cap_ = cap; }
    void set_soft_threshold_pct(int pct) { soft_threshold_pct_ = pct; }

    // Per-call socket timeout. When > 0, applied via SO_RCVTIMEO and
    // SO_SNDTIMEO on each new connection right after net::connect, so
    // individual TLS-handshake / HTTP read+write syscalls return with
    // an error instead of blocking. Zero (default) preserves the
    // legacy "no timeout — kernel TCP retransmit limits dominate"
    // behaviour. Engine wires this from the kill-switch path so
    // shutdown can't get wedged on a still-down LAN.
    void set_per_call_timeout(std::chrono::milliseconds t)
    {
        per_call_timeout_ms_.store(static_cast<long long>(t.count()),
                                   std::memory_order_release);
    }

    // Signed requests stamp timestamp = local + clock_offset, learned from
    // /api/v3/time. Without this, drift past recvWindow (5s) → -1021 on
    // every call. Refreshed lazily and reactively on -1021.
    void set_sync_interval_ms(long long ms) { sync_interval_ms_ = ms; }

    long long clock_offset_ms() const
    {
        return clock_offset_ms_.load(std::memory_order_acquire);
    }

    bool resync_clock_now()
    {
        auto offset = server_time_offset_ms(
            [this](const std::string& ep, const std::string& p) {
                return execute(http::verb::get,
                               ep + (p.empty() ? "" : "?" + p), "");
            },
            time_path_);
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

    // last_sync_ms_ <= 0 = never synced → always due. Pure for tests.
    static bool resync_due(long long now_steady_ms,
                           long long last_sync_steady_ms,
                           long long interval_ms)
    {
        if (last_sync_steady_ms <= 0) return true;
        if (interval_ms <= 0) return false;
        return (now_steady_ms - last_sync_steady_ms) >= interval_ms;
    }

    // ms to sleep before next request; 0 = no throttle. Pure for tests.
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
    // Allocated once across all BinanceRestClient instances; OpenSSL
    // hands out monotonically-increasing indexes via get_ex_new_index.
    static int ex_data_index()
    {
        static const int idx = SSL_CTX_get_ex_new_index(
            0, const_cast<char*>("BinanceRestClient::this"),
            nullptr, nullptr, nullptr);
        return idx;
    }

    // OpenSSL hands us a session with refcount already bumped for us; we
    // either store it (return 1, keeping the ref) or decline (return 0 and
    // OpenSSL frees). Multiple tickets can arrive on TLS 1.3 — keep only
    // the most recent and free any predecessor.
    static int on_new_session(SSL* ssl, SSL_SESSION* session)
    {
        SSL_CTX* ctx = SSL_get_SSL_CTX(ssl);
        auto* self = static_cast<BinanceRestClient*>(
            SSL_CTX_get_ex_data(ctx, ex_data_index()));
        if (!self) return 0;
        SSL_SESSION* old = self->cached_session_.exchange(
            session, std::memory_order_acq_rel);
        if (old) SSL_SESSION_free(old);
        return 1;
    }

    std::string api_key_;
    std::string api_secret_;
    std::string host_;
    std::string port_;
    std::string time_path_;
    ssl::context ctx_;
    std::atomic<SSL_SESSION*> cached_session_{nullptr};
    binance::HmacSha256Signer signer_;
    std::mutex signer_mu_;
    std::atomic<int> last_used_weight_{0};
    std::atomic<long long> window_anchor_ms_{0};
    int weight_cap_ = 6000;
    int soft_threshold_pct_ = 80;
    std::atomic<long long> per_call_timeout_ms_{0};

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

    // Retries once on -1021 by rebuilding the signed query (new ts+sig)
    // after a resync — must live above execute_with_retry for that reason.
    // Builds the signed query into a thread-local scratch buffer to avoid
    // 4–6 string allocations per order. Reuses a keyed HMAC_CTX (signer_)
    // so we don't pay the per-call key-schedule setup. Hex encoding is
    // hand-rolled (vs ostringstream in binance::sign_query).
    response do_signed_request(http::verb method,
                               const std::string& endpoint,
                               const std::string& params,
                               bool in_query)
    {
        maybe_resync_clock();

        auto build_signed = [&](std::string& out) {
            long long ts = static_cast<long long>(binance::server_time_ms())
                         + clock_offset_ms_.load(std::memory_order_acquire);

            out.clear();
            out.reserve(params.size() + 96);
            out.append(params);

            char buf[64];
            int n = std::snprintf(buf, sizeof(buf),
                "&timestamp=%lld&recvWindow=5000", ts);
            out.append(buf, static_cast<std::size_t>(n));

            unsigned char digest[32];
            {
                std::lock_guard<std::mutex> lk(signer_mu_);
                signer_.sign(out, digest);
            }

            char hex[64];
            binance::bytes_to_hex_lower(digest, 32, hex);
            out.append("&signature=", 11);
            out.append(hex, 64);
        };

        auto send = [&]() -> response {
            thread_local std::string scratch;
            build_signed(scratch);
            return in_query
                ? execute(method, endpoint + "?" + scratch, "")
                : execute(method, endpoint, scratch);
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
        // Near the weight cap: wait for the 1-minute window to roll.
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

            // Apply the most recent resumable session, if any. OpenSSL
            // transparently falls back to a full handshake if rejected.
            // Refcount: SSL_set_session up_refs internally, so our cached
            // ref stays valid for the next call.
            if (SSL_SESSION* sess = cached_session_.load(std::memory_order_acquire))
                SSL_set_session(stream.native_handle(), sess);

            auto& lowest = beast::get_lowest_layer(stream);
            net::connect(lowest, results);

            // Optional per-call socket timeout. Applied after connect so
            // TLS handshake + HTTP read/write are bounded but the connect
            // itself relies on the kernel's TCP error path. The kill-switch
            // sets this aggressively at shutdown so a dead network can't
            // wedge cancel/flatten beyond the wall-clock budget.
            const long long pct_ms =
                per_call_timeout_ms_.load(std::memory_order_acquire);
            if (pct_ms > 0)
            {
                struct timeval tv{};
                tv.tv_sec  = pct_ms / 1000;
                tv.tv_usec = (pct_ms % 1000) * 1000;
                const int fd = lowest.native_handle();
                ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            }

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
