#pragma once
#ifdef HAS_BYBIT

#include "execution/instrument.h"
#include "providers/bybit/bybit_auth.h"
#include "providers/bybit/bybit_endpoints.h"
#include "providers/bybit/bybit_parser.h"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>

#include <openssl/ssl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

namespace bybit {

// Local wall-clock ms (system_clock). Used for X-BAPI-TIMESTAMP + offset.
inline int64_t local_time_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Bybit V5 envelope: success iff HTTP 2xx AND retCode == 0.
// Fail-closed when retCode is missing/malformed.
inline bool is_business_success(int http_status, std::string_view body)
{
    if (http_status < 200 || http_status >= 300) return false;
    auto code = extract_sv_number(body, "retCode");
    if (code.empty())
        code = extract_sv_string(body, "retCode");
    if (code.empty()) return false;
    int64_t v = 0;
    if (!parse_int64_sv(code, v)) return false;
    return v == 0;
}

// Extract retCode as decimal string (empty if absent).
inline std::string_view extract_ret_code(std::string_view body)
{
    auto code = extract_sv_number(body, "retCode");
    if (!code.empty()) return code;
    return extract_sv_string(body, "retCode");
}

// Parse server time from GET /v5/market/time body.
// Prefer top-level "time" (ms); fall back to result.timeSecond * 1000.
// Returns false on miss/malformed.
inline bool parse_server_time_ms(std::string_view body, long long& out_ms)
{
    auto sv = extract_sv_number(body, "time");
    if (sv.empty())
        sv = extract_sv_string(body, "time");
    if (!sv.empty())
    {
        int64_t parsed = 0;
        if (!parse_int64_sv(sv, parsed)) return false;
        // Guard: if value looks like seconds (< year 2001 in ms scale), *1000.
        if (parsed > 0 && parsed < 1'000'000'000'000LL)
            parsed *= 1000;
        out_ms = static_cast<long long>(parsed);
        return true;
    }

    auto result = detail::extract_object(body, "result");
    if (result.empty()) return false;

    auto sec = extract_sv_string(result, "timeSecond");
    if (sec.empty())
        sec = extract_sv_number(result, "timeSecond");
    if (sec.empty()) return false;
    int64_t s = 0;
    if (!parse_int64_sv(sec, s)) return false;
    out_ms = static_cast<long long>(s) * 1000LL;
    return true;
}

// Instruments probe result (startup gate + encoder precision).
struct instrument_probe
{
    bool ok = false;           // symbol found, envelope ok, status Trading
    bool found = false;        // symbol present in result.list[]
    bool trading = false;      // status == "Trading"
    instrument_spec spec;
    std::string note;
    std::string status;        // raw venue status string
};

// Build query for GET /v5/market/instruments-info.
inline std::string instruments_query(std::string_view category,
                                     std::string_view symbol)
{
    std::string q;
    q.reserve(32 + category.size() + symbol.size());
    q.append("category=");
    q.append(category);
    if (!symbol.empty())
    {
        q.append("&symbol=");
        q.append(symbol);
    }
    return q;
}

// Parse instruments-info envelope (canned or live). Pure — no network.
// want_symbol empty → first list[] entry; otherwise match symbol (case-sensitive).
inline instrument_probe parse_instruments_response(
    std::string_view body,
    std::string_view want_symbol = {})
{
    instrument_probe out;
    if (!is_business_success(200, body))
    {
        auto code = extract_ret_code(body);
        out.note = "instruments: retCode ";
        out.note.append(code.empty() ? "<missing>" : code);
        return out;
    }

    auto result = detail::extract_object(body, "result");
    if (result.empty())
    {
        out.note = "instruments: missing result object";
        return out;
    }

    auto arr = detail::extract_array(result, "list");
    if (arr.empty())
    {
        out.note = "instruments: missing result.list array";
        return out;
    }

    std::string_view matched;
    detail::for_each_array_object(arr, [&](std::string_view obj) {
        if (!matched.empty()) return;
        auto sym = extract_sv_string(obj, "symbol");
        if (want_symbol.empty() || sym == want_symbol)
            matched = obj;
    });

    if (matched.empty())
    {
        out.note = "instruments: symbol not found";
        if (!want_symbol.empty())
        {
            out.note.push_back(' ');
            out.note.append(want_symbol);
        }
        return out;
    }

    out.found = true;
    {
        auto sym = extract_sv_string(matched, "symbol");
        out.spec.symbol.assign(sym.data(), sym.size());
    }

    auto st = extract_sv_string(matched, "status");
    out.status.assign(st.data(), st.size());
    // Linear instruments: "Trading" = tradable. Other states refuse live.
    out.trading = (st == "Trading");

    auto parse_d = [](std::string_view obj, std::string_view key, double& dst) {
        auto sv = extract_sv_string(obj, key);
        if (sv.empty()) sv = extract_sv_number(obj, key);
        double v = 0.0;
        if (!sv.empty() && parse_double_sv(sv, v))
            dst = v;
    };

    // Nested filters: priceFilter.tickSize, lotSizeFilter.qtyStep / minOrderQty.
    auto price_f = detail::extract_object(matched, "priceFilter");
    if (!price_f.empty())
        parse_d(price_f, "tickSize", out.spec.tick_size);

    auto lot_f = detail::extract_object(matched, "lotSizeFilter");
    if (!lot_f.empty())
    {
        parse_d(lot_f, "qtyStep", out.spec.lot_size);
        parse_d(lot_f, "minOrderQty", out.spec.min_qty);
        parse_d(lot_f, "minNotionalValue", out.spec.min_notional);
    }

    // Flat fallbacks (some canned fixtures flatten filters).
    if (out.spec.tick_size <= 0.0)
        parse_d(matched, "tickSize", out.spec.tick_size);
    if (out.spec.lot_size <= 0.0)
        parse_d(matched, "qtyStep", out.spec.lot_size);
    if (out.spec.min_qty <= 0.0)
        parse_d(matched, "minOrderQty", out.spec.min_qty);

    if (!out.trading)
    {
        out.ok = false;
        out.note = "instruments: status '";
        out.note.append(out.status);
        out.note.append("' is not Trading");
        return out;
    }

    out.ok = true;
    out.note = "instruments: ok";
    return out;
}

// Truncate body for cold-path logs (pair with redact_for_log for secrets).
inline std::string truncate_for_log(std::string_view body, std::size_t max_len = 240)
{
    if (body.size() <= max_len) return std::string(body);
    std::string out(body.substr(0, max_len));
    out.append("...");
    return out;
}

// Short orderLinkId minter: Bybit caps at 36 chars.
// Stock ClientOrderIdMinter embeds full epoch+seed hex and can exceed 36.
class ShortOrderLinkIdMinter
{
public:
    explicit ShortOrderLinkIdMinter(std::uint64_t seed,
                                    std::int64_t epoch_ms = now_epoch_ms())
        : seq_(0)
    {
        // "tt" + 8 hex (mixed epoch) + 4 hex (seed low) = 14-char prefix.
        // Counter as up to 8 hex → total ≤ 22 << 36.
        const auto mix = static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(epoch_ms)
             ^ (static_cast<std::uint64_t>(epoch_ms) >> 32))
            & 0xffffffffu);
        const auto s16 = static_cast<std::uint32_t>(seed & 0xffffu);
        char buf[20];
        std::snprintf(buf, sizeof(buf), "tt%08x%04x", mix, s16);
        prefix_ = buf;
    }

    std::string next()
    {
        const std::uint64_t n = ++seq_;
        char buf[48];
        std::snprintf(buf, sizeof(buf), "%s%08llx",
                      prefix_.c_str(),
                      static_cast<unsigned long long>(n));
        std::string id(buf);
        if (id.size() > 36)
            id.resize(36);
        return id;
    }

    const std::string& prefix() const { return prefix_; }

private:
    static std::int64_t now_epoch_ms()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    std::string prefix_;
    std::atomic<std::uint64_t> seq_;
};

} // namespace bybit

// ---------------------------------------------------------------------------
// BybitRestClient — Boost.Beast TLS HTTP client (V5 REST).
// Pattern mirrors BitgetRestClient: keep-alive stream, TLS session cache,
// async write+read with optional run_for timeout.
// Auth: header X-BAPI-SIGN over timestamp+key+recv_window+(query|jsonBody).
// ---------------------------------------------------------------------------
class BybitRestClient
{
public:
    BybitRestClient(
        const std::string& api_key,
        const std::string& api_secret,
        const std::string& host = "api.bybit.com",
        const std::string& port = "443",
        const std::string& time_path = bybit::paths::market_time,
        std::string recv_window = bybit::k_default_recv_window)
        : api_key_(api_key)
        , api_secret_(api_secret)
        , host_(host)
        , port_(port)
        , time_path_(time_path)
        , recv_window_(std::move(recv_window))
        , ctx_(ssl::context::tlsv12_client)
        , signer_(api_secret)
    {
        ctx_.set_default_verify_paths();
        ctx_.set_verify_mode(ssl::verify_peer);

        // TLS session resumption: skip asymmetric crypto on reconnect.
        SSL_CTX* raw = ctx_.native_handle();
        SSL_CTX_set_session_cache_mode(
            raw, SSL_SESS_CACHE_CLIENT | SSL_SESS_CACHE_NO_INTERNAL_STORE);
        SSL_CTX_set_ex_data(raw, ex_data_index(), this);
        SSL_CTX_sess_set_new_cb(raw, &BybitRestClient::on_new_session);
    }

    explicit BybitRestClient(const std::string& api_key,
                             const std::string& api_secret,
                             const bybit::endpoints& ep)
        : BybitRestClient(api_key, api_secret, ep.rest_host, ep.rest_port)
    {
    }

    ~BybitRestClient()
    {
        close_connection_locked();
        SSL_SESSION* sess = cached_session_.exchange(
            nullptr, std::memory_order_acq_rel);
        if (sess) SSL_SESSION_free(sess);
    }

    BybitRestClient(const BybitRestClient&) = delete;
    BybitRestClient& operator=(const BybitRestClient&) = delete;
    BybitRestClient(BybitRestClient&&) = delete;
    BybitRestClient& operator=(BybitRestClient&&) = delete;

    // Test-only seam: additive CA trust for local TLS fixtures.
    void add_trusted_ca_for_testing(const std::string& ca_pem)
    {
        ctx_.add_certificate_authority(
            net::buffer(ca_pem.data(), ca_pem.size()));
    }

    struct response
    {
        int status = 0;
        std::string body;
        // Parsed retCode when present; -1 if missing/unparseable.
        int ret_code = -1;
        // True only when HTTP 2xx AND retCode == 0.
        bool business_ok = false;
    };

    // Signed GET: query string without leading '?'; path is bare endpoint.
    response get(const std::string& endpoint, const std::string& query = "")
    {
        return do_signed_request(http::verb::get, endpoint, query, /*body=*/"");
    }

    // Signed POST with exact JSON body (no re-serialize).
    response post_json(const std::string& endpoint, const std::string& json_body)
    {
        return do_signed_request(http::verb::post, endpoint, /*query=*/"", json_body);
    }

    response get_unsigned(const std::string& endpoint,
                          const std::string& params = "")
    {
        return execute(http::verb::get,
                       endpoint + (params.empty() ? "" : "?" + params),
                       "");
    }

    // server_time_ms - local_time_ms; negative = local ahead. LLONG_MIN on fail.
    using get_fn_t = std::function<response(const std::string&, const std::string&)>;

    static long long server_time_offset_ms(
        const get_fn_t& get_fn,
        const std::string& time_path = bybit::paths::market_time)
    {
        if (!get_fn) return LLONG_MIN;
        try
        {
            auto resp = get_fn(time_path, "");
            if (resp.status < 200 || resp.status >= 300)
                return LLONG_MIN;

            long long server_ms = 0;
            if (!bybit::parse_server_time_ms(resp.body, server_ms))
                return LLONG_MIN;
            long long local_ms = static_cast<long long>(bybit::local_time_ms());
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

    // Per-call I/O timeout via async + run_for. 0 = unbounded (default).
    void set_per_call_timeout(std::chrono::milliseconds t)
    {
        per_call_timeout_ms_.store(static_cast<long long>(t.count()),
                                   std::memory_order_release);
    }

    std::chrono::milliseconds per_call_timeout() const
    {
        return std::chrono::milliseconds(
            per_call_timeout_ms_.load(std::memory_order_acquire));
    }

    void set_sync_interval_ms(long long ms) { sync_interval_ms_ = ms; }

    void set_recv_window(std::string w) { recv_window_ = std::move(w); }
    const std::string& recv_window() const { return recv_window_; }

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
                std::cerr << "BybitRestClient: clock resync failed; "
                             "keeping offset "
                          << clock_offset_ms_.load() << " ms\n";
            return false;
        }
        clock_offset_ms_.store(offset, std::memory_order_release);
        last_sync_steady_ms_.store(steady_now_ms(), std::memory_order_release);
        sync_failed_logged_.store(false);
        return true;
    }

    // last_sync <= 0 = never synced → always due. Pure for tests.
    static bool resync_due(long long now_steady_ms,
                           long long last_sync_steady_ms,
                           long long interval_ms)
    {
        if (last_sync_steady_ms <= 0) return true;
        if (interval_ms <= 0) return false;
        return (now_steady_ms - last_sync_steady_ms) >= interval_ms;
    }

private:
    static int ex_data_index()
    {
        static const int idx = SSL_CTX_get_ex_new_index(
            0, const_cast<char*>("BybitRestClient::this"),
            nullptr, nullptr, nullptr);
        return idx;
    }

    static int on_new_session(SSL* ssl, SSL_SESSION* session)
    {
        SSL_CTX* ctx = SSL_get_SSL_CTX(ssl);
        auto* self = static_cast<BybitRestClient*>(
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
    std::string recv_window_;
    ssl::context ctx_;
    std::atomic<SSL_SESSION*> cached_session_{nullptr};
    bybit::HmacSha256HexSigner signer_;
    std::mutex signer_mu_;
    std::atomic<long long> per_call_timeout_ms_{0};

    std::atomic<long long> clock_offset_ms_{0};
    std::atomic<long long> last_sync_steady_ms_{0};
    long long sync_interval_ms_ = 5 * 60 * 1000;
    std::atomic<bool> sync_failed_logged_{false};

    std::optional<net::io_context> persistent_ioc_;
    std::optional<beast::ssl_stream<tcp::socket>> persistent_stream_;
    bool connected_ = false;
    std::mutex connection_mu_;

    static long long steady_now_ms()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    void maybe_resync_clock()
    {
        if (resync_due(steady_now_ms(),
                       last_sync_steady_ms_.load(std::memory_order_acquire),
                       sync_interval_ms_))
            resync_clock_now();
    }

    static int parse_ret_code_int(std::string_view body)
    {
        auto code = bybit::extract_ret_code(body);
        if (code.empty()) return -1;
        int64_t v = 0;
        if (!bybit::parse_int64_sv(code, v)) return -1;
        if (v < INT_MIN || v > INT_MAX) return -1;
        return static_cast<int>(v);
    }

    response do_signed_request(http::verb method,
                               const std::string& endpoint,
                               const std::string& query,
                               const std::string& body)
    {
        maybe_resync_clock();

        long long ts = static_cast<long long>(bybit::local_time_ms())
                     + clock_offset_ms_.load(std::memory_order_acquire);
        char ts_buf[32];
        int ts_n = std::snprintf(ts_buf, sizeof(ts_buf), "%lld", ts);
        std::string_view ts_sv(ts_buf, static_cast<std::size_t>(ts_n > 0 ? ts_n : 0));

        // Prehash payload: query for GET, raw JSON body for POST.
        const std::string_view payload =
            (method == http::verb::get) ? std::string_view(query)
                                        : std::string_view(body);

        std::string sign;
        {
            std::lock_guard<std::mutex> lk(signer_mu_);
            sign = signer_.sign(bybit::build_rest_prehash(
                ts_sv, api_key_, recv_window_, payload));
        }

        const std::string target =
            query.empty() ? endpoint : (endpoint + "?" + query);

        return execute(method, target, body, /*signed_req=*/true,
                       ts_sv, sign);
    }

    response execute(http::verb method,
                     const std::string& target,
                     const std::string& body)
    {
        return execute(method, target, body, /*signed_req=*/false, {}, {});
    }

    response execute(http::verb method,
                     const std::string& target,
                     const std::string& body,
                     bool signed_req,
                     std::string_view ts,
                     std::string_view sign)
    {
        response r;

        {
            // Hold connection_mu_ across connect + I/O so concurrent callers
            // cannot interleave on the shared keep-alive stream.
            std::lock_guard<std::mutex> lk(connection_mu_);

            // Retry once only if write never completed (venue saw nothing).
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
                    req.set(http::field::content_type, "application/json");

                    if (signed_req)
                    {
                        req.set("X-BAPI-API-KEY", api_key_);
                        req.set("X-BAPI-SIGN", std::string(sign));
                        req.set("X-BAPI-TIMESTAMP", std::string(ts));
                        req.set("X-BAPI-RECV-WINDOW", recv_window_);
                        req.set("X-BAPI-SIGN-TYPE", "2");
                    }

                    if (!body.empty())
                    {
                        req.body() = body;
                        req.prepare_payload();
                    }
                    else if (method == http::verb::post)
                    {
                        req.prepare_payload();
                    }

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

                    request_sent = write_done;

                    if (!io_done || op_ec)
                    {
                        close_connection_locked();

                        if (!request_sent && attempt == 0)
                        {
                            std::cerr << "BybitRestClient: connection failed "
                                         "before request was sent ("
                                      << (op_ec ? op_ec.message()
                                                : std::string("deadline elapsed"))
                                      << "), reconnecting and retrying once\n";
                            continue;
                        }

                        if (io_done)
                            std::cerr << "BybitRestClient: request failed: "
                                      << op_ec.message() << "\n";
                        else
                            std::cerr << "BybitRestClient: request timed out "
                                         "after "
                                      << timeout_ms << " ms\n";
                        r = {};
                        break;
                    }

                    r.status = static_cast<int>(res.result_int());
                    r.body = res.body();
                    r.ret_code = parse_ret_code_int(r.body);
                    r.business_ok = bybit::is_business_success(r.status, r.body);

                    if (r.status >= 400)
                    {
                        std::cerr << "BybitRestClient: HTTP " << r.status
                                  << " - "
                                  << bybit::redact_for_log(
                                         bybit::truncate_for_log(r.body))
                                  << "\n";
                    }
                    else if (!r.business_ok && r.status >= 200 && r.status < 300)
                    {
                        // Critical Bybit quirk: HTTP 200 + retCode != 0.
                        std::cerr << "BybitRestClient: business error retCode="
                                  << bybit::extract_ret_code(r.body)
                                  << " - "
                                  << bybit::redact_for_log(
                                         bybit::truncate_for_log(r.body))
                                  << "\n";
                    }

                    break;
                }
                catch (const std::exception& e)
                {
                    close_connection_locked();

                    if (!request_sent && attempt == 0)
                    {
                        std::cerr << "BybitRestClient: connect failed ("
                                  << e.what()
                                  << "), reconnecting and retrying once\n";
                        continue;
                    }

                    std::cerr << "BybitRestClient: request failed: "
                              << e.what() << "\n";
                    r = {};
                    break;
                }
            }
        }

        return r;
    }

    void close_connection_locked()
    {
        connected_ = false;
        if (persistent_stream_)
        {
            beast::error_code ec;
            auto& lowest = beast::get_lowest_layer(*persistent_stream_);
            lowest.close(ec);
        }
        persistent_stream_.reset();
        persistent_ioc_.reset();
    }

    void ensure_connected_locked()
    {
        if (connected_ && persistent_stream_ && persistent_ioc_)
        {
            auto& lowest = beast::get_lowest_layer(*persistent_stream_);
            if (lowest.is_open())
                return;
        }

        close_connection_locked();

        persistent_ioc_.emplace();
        persistent_stream_.emplace(*persistent_ioc_, ctx_);

        auto& stream = *persistent_stream_;
        auto& lowest = beast::get_lowest_layer(stream);

        if (!SSL_set_tlsext_host_name(stream.native_handle(), host_.c_str()))
            throw std::runtime_error("SNI setup failed");

        if (SSL_SESSION* sess = cached_session_.load(std::memory_order_acquire))
            SSL_set_session(stream.native_handle(), sess);

        // Re-resolve on every reconnect (CDN / GSLB can shift IPs).
        tcp::resolver resolver(*persistent_ioc_);
        auto resolver_results = resolver.resolve(host_, port_);
        net::connect(lowest, resolver_results);
        stream.handshake(ssl::stream_base::client);
        connected_ = true;
    }
};

#endif // HAS_BYBIT
