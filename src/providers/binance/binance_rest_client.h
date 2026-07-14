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
        // Cleanly close any persistent connection
        close_connection_locked();

        SSL_SESSION* sess = cached_session_.exchange(
            nullptr, std::memory_order_acq_rel);
        if (sess) SSL_SESSION_free(sess);
    }

    BinanceRestClient(const BinanceRestClient&) = delete;
    BinanceRestClient& operator=(const BinanceRestClient&) = delete;
    BinanceRestClient(BinanceRestClient&&) = delete;
    BinanceRestClient& operator=(BinanceRestClient&&) = delete;

    // Test-only seam: trust one additional CA/cert (PEM) so tests can point
    // the client at a local TLS server. This is strictly ADDITIVE - it can
    // only extend the trust set for a specific certificate; it never relaxes
    // verify_peer or touches the production trust roots. Not used in any
    // non-test code path.
    void add_trusted_ca_for_testing(const std::string& ca_pem)
    {
        ctx_.add_certificate_authority(
            net::buffer(ca_pem.data(), ca_pem.size()));
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

    // Per-call I/O timeout. When > 0, the write+read of each request is run
    // as async ops on the connection's io_context and bounded with
    // run_for(t); on expiry the connection is dropped and the call returns a
    // failure. Zero (default) preserves the legacy "no timeout - kernel TCP
    // retransmit limits dominate" behaviour. Engine wires this from the
    // kill-switch path so shutdown can't get wedged on a still-down LAN.
    // (SO_RCVTIMEO/SO_SNDTIMEO are deliberately NOT used: Asio polls with an
    // infinite timeout on EAGAIN, so kernel socket timeouts never fire here.)
    void set_per_call_timeout(std::chrono::milliseconds t)
    {
        per_call_timeout_ms_.store(static_cast<long long>(t.count()),
                                   std::memory_order_release);
    }

    // Signed requests stamp timestamp = local + clock_offset, learned from
    // /api/v3/time. Without this, drift past recvWindow (5s) -> -1021 on
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

    // last_sync_ms_ <= 0 = never synced -> always due. Pure for tests.
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
    // OpenSSL frees). Multiple tickets can arrive on TLS 1.3 - keep only
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

    // === Persistent HTTP connection state for Keep-Alive ===
    // Replaces the previous "new ioc + stream per request" pattern.
    // All access to the stream / connection state is protected by connection_mu_.
    std::optional<net::io_context>          persistent_ioc_;
    std::optional<beast::ssl_stream<tcp::socket>> persistent_stream_;
    bool                                    connected_ = false;
    std::mutex                              connection_mu_;

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
    // after a resync - must live above execute_with_retry for that reason.
    // Builds the signed query into a thread-local scratch buffer to avoid
    // 4-6 string allocations per order. Reuses a keyed HMAC_CTX (signer_)
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

        response r{0, "", 0};
        long long rate_limit_sleep_ms = 0;
        bool should_retry_after_429 = false;

        {
            // Hold connection_mu_ across the whole transaction (connect + I/O)
            // so concurrent callers can't interleave on the shared persistent
            // stream. ensure_connected_locked() and close_connection_locked()
            // both assume this lock is held. The 429 / throttle backoffs run
            // after the lock is released so a sleep never blocks another
            // caller (e.g. the kill-switch on shutdown).
            std::lock_guard<std::mutex> lk(connection_mu_);

            // A reused keep-alive connection can be closed server-side while
            // idle; locally it still looks open, so the first write after the
            // gap fails. We retry exactly once, but ONLY when the failure
            // happened before the request reached the wire (write not yet
            // complete) - the venue saw nothing, so replaying a non-idempotent
            // POST is safe. A post-write failure is never replayed.
            for (int attempt = 0; attempt < 2; ++attempt)
            {
                bool request_sent = false;
                try
                {
                    ensure_connected_locked();

                    auto& stream = *persistent_stream_;
                    auto& ioc = *persistent_ioc_;

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

                    // Bounded async transaction. SO_RCVTIMEO/SO_SNDTIMEO do NOT
                    // bound Asio synchronous I/O: Asio drives the socket in
                    // non-blocking mode and, on EAGAIN, polls with an infinite
                    // timeout, so the kernel socket timeout never fires. To give
                    // the kill-switch a real wall-clock bound we issue the
                    // write+read as async ops on the connection's own io_context
                    // and cap them with run_for(per_call_timeout). A timeout of
                    // 0 (the default, non-kill-switch path) runs unbounded.
                    beast::flat_buffer buffer;
                    http::response<http::string_body> res;
                    beast::error_code op_ec;
                    bool write_done = false;
                    bool io_done = false;

                    http::async_write(stream, req,
                        [&](beast::error_code ec, std::size_t)
                        {
                            if (ec) { op_ec = ec; io_done = true; return; }
                            write_done = true;
                            http::async_read(stream, buffer, res,
                                [&](beast::error_code ec2, std::size_t)
                                {
                                    op_ec = ec2;
                                    io_done = true;
                                });
                        });

                    ioc.restart();
                    const long long timeout_ms =
                        per_call_timeout_ms_.load(std::memory_order_acquire);
                    if (timeout_ms > 0)
                        ioc.run_for(std::chrono::milliseconds(timeout_ms));
                    else
                        ioc.run();

                    // write_done tells the retry below whether the request
                    // reached the wire.
                    request_sent = write_done;

                    if (!io_done || op_ec)
                    {
                        // Deadline elapsed mid-transaction (!io_done) or an
                        // async op errored. Drop the connection; on a timeout
                        // the still-pending handlers are discarded (never
                        // invoked) when the io_context is destroyed inside
                        // close_connection_locked(), so the stack locals they
                        // captured by reference are never touched afterwards.
                        close_connection_locked();

                        if (!request_sent && attempt == 0)
                        {
                            std::cerr << "BinanceRestClient: connection failed before "
                                         "request was sent ("
                                      << (op_ec ? op_ec.message()
                                                : std::string("deadline elapsed"))
                                      << "), reconnecting and retrying once\n";
                            continue;
                        }

                        if (io_done)
                            std::cerr << "BinanceRestClient: request failed: "
                                      << op_ec.message() << "\n";
                        else
                            std::cerr << "BinanceRestClient: request timed out after "
                                      << timeout_ms << " ms\n";
                        r = {0, "", 0};
                        break;
                    }

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

                    // No shutdown() - the connection is kept alive for the next request.

                    if (r.status == 429)
                    {
                        rate_limit_sleep_ms = 2000;
                        auto retry_after = res.find("Retry-After");
                        if (retry_after != res.end())
                        {
                            try
                            {
                                long long sec = std::stoll(
                                    std::string(retry_after->value()));
                                if (sec > 0) rate_limit_sleep_ms = std::min<long long>(sec * 1000,
                                                                                        60'000);
                            }
                            catch (...) {}
                        }
                        should_retry_after_429 = retry_on_429;
                    }
                    else if (r.status >= 400)
                    {
                        std::cerr << "BinanceRestClient: HTTP " << r.status
                                  << " - " << binance::redact_for_log(r.body, 240)
                                  << "\n";
                    }

                    break;  // transaction completed (success or HTTP error)
                }
                catch (const std::exception& e)
                {
                    // ensure_connected_locked() failed (connect / handshake);
                    // nothing was sent. Drop the connection and retry once.
                    close_connection_locked();

                    if (!request_sent && attempt == 0)
                    {
                        std::cerr << "BinanceRestClient: connect failed ("
                                  << e.what() << "), reconnecting and retrying once\n";
                        continue;
                    }

                    std::cerr << "BinanceRestClient: request failed: " << e.what() << "\n";
                    r = {0, "", 0};
                    break;
                }
            }
        }  // connection_mu_ released - 429 / throttle sleeps must not block other callers

        if (rate_limit_sleep_ms > 0)
        {
            std::cerr << "BinanceRestClient: rate limited (429). "
                      << "Weight used: " << r.used_weight
                      << ", sleeping " << rate_limit_sleep_ms << " ms\n";
            std::this_thread::sleep_for(
                std::chrono::milliseconds(rate_limit_sleep_ms));
            if (should_retry_after_429)
                return execute_with_retry(method, target, body, false);
            return r;
        }

        return r;
    }

    // =====================================================================
    // Persistent connection (Keep-Alive) implementation
    // =====================================================================

    void close_connection_locked()
    {
        connected_ = false;

        if (persistent_stream_)
        {
            beast::error_code ec;
            // Close the underlying TCP socket; this also terminates TLS
            auto& lowest = beast::get_lowest_layer(*persistent_stream_);
            lowest.close(ec);
        }

        persistent_stream_.reset();
        persistent_ioc_.reset();
    }

    // Caller must hold connection_mu_.
    void ensure_connected_locked()
    {
        if (connected_ && persistent_stream_ && persistent_ioc_)
        {
            auto& lowest = beast::get_lowest_layer(*persistent_stream_);
            if (lowest.is_open())
            {
                return;   // Happy path: warm connection
            }
        }

        // (Re)establish connection
        close_connection_locked();

        persistent_ioc_.emplace();
        persistent_stream_.emplace(*persistent_ioc_, ctx_);

        auto& stream = *persistent_stream_;
        auto& lowest = beast::get_lowest_layer(stream);

        if (!SSL_set_tlsext_host_name(stream.native_handle(), host_.c_str()))
            throw std::runtime_error("SNI setup failed");

        if (SSL_SESSION* sess = cached_session_.load(std::memory_order_acquire))
            SSL_set_session(stream.native_handle(), sess);

        // Re-resolve on every (re)connect. api.binance.com is a rotating
        // GSLB/round-robin record; caching the result for the whole process
        // lifetime would pin a long-running live session to start-of-process
        // IPs and keep failing once the venue shifts them. This is the cold
        // path (reconnect only), so the extra lookup is negligible.
        tcp::resolver resolver(*persistent_ioc_);
        auto resolver_results = resolver.resolve(host_, port_);
        net::connect(lowest, resolver_results);

        stream.handshake(ssl::stream_base::client);

        connected_ = true;
    }
};

#endif // HAS_BINANCE
