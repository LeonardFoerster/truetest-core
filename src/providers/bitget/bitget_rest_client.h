#pragma once
#ifdef HAS_BITGET

#include "execution/instrument.h"
#include "providers/bitget/bitget_auth.h"
#include "providers/bitget/bitget_parser.h"

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

namespace bitget {

// Local wall-clock ms (system_clock). Used for ACCESS-TIMESTAMP + offset.
inline int64_t local_time_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Bitget envelope: success iff HTTP 2xx AND "code":"00000".
// Fail-closed when code is missing/malformed (Bitget always sends it).
inline bool is_business_success(int http_status, std::string_view body)
{
    if (http_status < 200 || http_status >= 300) return false;
    auto code = extract_sv_string(body, "code");
    if (code.empty())
        code = extract_sv_number(body, "code");
    return code == "00000";
}

// Extract business code string ("00000" on success). Empty if absent.
inline std::string_view extract_business_code(std::string_view body)
{
    auto code = extract_sv_string(body, "code");
    if (!code.empty()) return code;
    return extract_sv_number(body, "code");
}

// Parse server time from GET /api/v2/public/time body.
// Prefers data.serverTime; falls back to top-level serverTime / requestTime.
// Returns false on miss/malformed.
inline bool parse_server_time_ms(std::string_view body, long long& out_ms)
{
    auto sv = extract_sv_number(body, "serverTime");
    if (sv.empty())
        sv = extract_sv_string(body, "serverTime");
    if (sv.empty())
        sv = extract_sv_number(body, "requestTime");
    if (sv.empty()) return false;
    int64_t parsed = 0;
    if (!parse_int64_sv(sv, parsed)) return false;
    out_ms = static_cast<long long>(parsed);
    return true;
}

// Instruments probe result (startup gate + encoder precision).
struct instrument_probe
{
    bool ok = false;           // symbol found, envelope ok, status tradable
    bool found = false;        // symbol present in data[]
    bool trading = false;      // status == "online"
    instrument_spec spec;
    std::string note;
    std::string status;        // raw venue status string
};

// Build query for GET /api/v3/market/instruments.
// Keys are emitted in alphabetical order (category, symbol) for sign stability.
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

// Bitget REST prehash requires query parameters sorted by key (A–Z).
// Splits on '&', sorts by the key segment before '=', re-joins.
// Empty / single-param inputs are returned as-is (minus no-op copies).
inline std::string sort_query_string(std::string_view query)
{
    if (query.empty())
        return {};

    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (start <= query.size())
    {
        auto amp = query.find('&', start);
        if (amp == std::string_view::npos)
        {
            if (start < query.size())
                parts.push_back(query.substr(start));
            break;
        }
        if (amp > start)
            parts.push_back(query.substr(start, amp - start));
        start = amp + 1;
    }
    if (parts.size() <= 1)
        return std::string(query);

    std::sort(parts.begin(), parts.end(),
              [](std::string_view a, std::string_view b) {
                  const auto ea = a.find('=');
                  const auto eb = b.find('=');
                  const auto ka = a.substr(0, ea == std::string_view::npos
                                                 ? a.size()
                                                 : ea);
                  const auto kb = b.substr(0, eb == std::string_view::npos
                                                 ? b.size()
                                                 : eb);
                  return ka < kb;
              });

    std::string out;
    out.reserve(query.size());
    for (std::size_t i = 0; i < parts.size(); ++i)
    {
        if (i > 0)
            out.push_back('&');
        out.append(parts[i].data(), parts[i].size());
    }
    return out;
}

// Parse instruments envelope (canned or live). Pure — no network.
// want_symbol empty → first data[] entry; otherwise match symbol (case-sensitive).
inline instrument_probe parse_instruments_response(
    std::string_view body,
    std::string_view want_symbol = {})
{
    instrument_probe out;
    if (!is_business_success(200, body))
    {
        auto code = extract_business_code(body);
        out.note = "instruments: business code ";
        out.note.append(code.empty() ? "<missing>" : code);
        return out;
    }

    auto arr = detail::extract_array(body, "data");
    if (arr.empty())
    {
        out.note = "instruments: missing data array";
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
    // UTA instruments: "online" = tradable. Other states refuse live.
    out.trading = (st == "online");

    auto parse_d = [](std::string_view obj, std::string_view key, double& dst) {
        auto sv = extract_sv_string(obj, key);
        if (sv.empty()) sv = extract_sv_number(obj, key);
        double v = 0.0;
        if (!sv.empty() && parse_double_sv(sv, v))
            dst = v;
    };

    // UTA v3: priceMultiplier = tick step; quantityMultiplier = lot step.
    parse_d(matched, "priceMultiplier", out.spec.tick_size);
    parse_d(matched, "quantityMultiplier", out.spec.lot_size);
    parse_d(matched, "minOrderQty", out.spec.min_qty);
    parse_d(matched, "minOrderAmount", out.spec.min_notional);
    parse_d(matched, "makerFeeRate", out.spec.maker_rate);
    parse_d(matched, "takerFeeRate", out.spec.taker_rate);

    if (!out.trading)
    {
        out.ok = false;
        out.note = "instruments: status '";
        out.note.append(out.status);
        out.note.append("' is not online/trading");
        return out;
    }

    out.ok = true;
    out.note = "instruments: ok";
    return out;
}

// Truncate body for cold-path logs (no secret redaction depth needed here).
inline std::string truncate_for_log(std::string_view body, std::size_t max_len = 240)
{
    if (body.size() <= max_len) return std::string(body);
    std::string out(body.substr(0, max_len));
    out.append("...");
    return out;
}

} // namespace bitget

// ---------------------------------------------------------------------------
// BitgetRestClient — Boost.Beast TLS HTTP client (UTA REST).
// Pattern mirrors BinanceRestClient: keep-alive stream, TLS session cache,
// async write+read with optional run_for timeout (SO_RCVTIMEO ineffective).
// ---------------------------------------------------------------------------
class BitgetRestClient
{
public:
    BitgetRestClient(
        const std::string& api_key,
        const std::string& api_secret,
        const std::string& api_passphrase,
        const std::string& host = "api.bitget.com",
        const std::string& port = "443",
        const std::string& time_path = "/api/v2/public/time",
        bool paptrading = false)
        : api_key_(api_key)
        , api_secret_(api_secret)
        , api_passphrase_(api_passphrase)
        , host_(host)
        , port_(port)
        , time_path_(time_path)
        , paptrading_(paptrading)
        , ctx_(ssl::context::tlsv12_client)
        , signer_(api_secret)
    {
        ctx_.set_default_verify_paths();
        ctx_.set_verify_mode(ssl::verify_peer);

        // TLS session resumption: skip asymmetric crypto on reconnect.
        // NO_INTERNAL_STORE — we own a single cached session slot.
        SSL_CTX* raw = ctx_.native_handle();
        SSL_CTX_set_session_cache_mode(
            raw, SSL_SESS_CACHE_CLIENT | SSL_SESS_CACHE_NO_INTERNAL_STORE);
        SSL_CTX_set_ex_data(raw, ex_data_index(), this);
        SSL_CTX_sess_set_new_cb(raw, &BitgetRestClient::on_new_session);
    }

    ~BitgetRestClient()
    {
        close_connection_locked();
        SSL_SESSION* sess = cached_session_.exchange(
            nullptr, std::memory_order_acq_rel);
        if (sess) SSL_SESSION_free(sess);
    }

    BitgetRestClient(const BitgetRestClient&) = delete;
    BitgetRestClient& operator=(const BitgetRestClient&) = delete;
    BitgetRestClient(BitgetRestClient&&) = delete;
    BitgetRestClient& operator=(BitgetRestClient&&) = delete;

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
        // True only when HTTP 2xx AND Bitget code == "00000".
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
        const std::string& time_path = "/api/v2/public/time")
    {
        if (!get_fn) return LLONG_MIN;
        try
        {
            auto resp = get_fn(time_path, "");
            // HTTP must be 2xx; business code checked loosely so pure time
            // fixtures without a code field still work when parseable.
            if (resp.status < 200 || resp.status >= 300)
                return LLONG_MIN;

            long long server_ms = 0;
            if (!bitget::parse_server_time_ms(resp.body, server_ms))
                return LLONG_MIN;
            long long local_ms = static_cast<long long>(bitget::local_time_ms());
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
    // SO_RCVTIMEO is ineffective under Asio; kill-path must set this.
    void set_per_call_timeout(std::chrono::milliseconds t)
    {
        per_call_timeout_ms_.store(static_cast<long long>(t.count()),
                                   std::memory_order_release);
    }

    // Current per-call timeout (for kill-switch restore after temporary tighten).
    std::chrono::milliseconds per_call_timeout() const
    {
        return std::chrono::milliseconds(
            per_call_timeout_ms_.load(std::memory_order_acquire));
    }

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
                std::cerr << "BitgetRestClient: clock resync failed; "
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

    bool paptrading() const { return paptrading_; }

private:
    static int ex_data_index()
    {
        static const int idx = SSL_CTX_get_ex_new_index(
            0, const_cast<char*>("BitgetRestClient::this"),
            nullptr, nullptr, nullptr);
        return idx;
    }

    static int on_new_session(SSL* ssl, SSL_SESSION* session)
    {
        SSL_CTX* ctx = SSL_get_SSL_CTX(ssl);
        auto* self = static_cast<BitgetRestClient*>(
            SSL_CTX_get_ex_data(ctx, ex_data_index()));
        if (!self) return 0;
        SSL_SESSION* old = self->cached_session_.exchange(
            session, std::memory_order_acq_rel);
        if (old) SSL_SESSION_free(old);
        return 1;
    }

    std::string api_key_;
    std::string api_secret_;
    std::string api_passphrase_;
    std::string host_;
    std::string port_;
    std::string time_path_;
    bool paptrading_ = false;
    ssl::context ctx_;
    std::atomic<SSL_SESSION*> cached_session_{nullptr};
    bitget::HmacSha256Base64Signer signer_;
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

    static const char* verb_name(http::verb method)
    {
        switch (method)
        {
        case http::verb::get: return "GET";
        case http::verb::post: return "POST";
        case http::verb::delete_: return "DELETE";
        case http::verb::put: return "PUT";
        default: return "GET";
        }
    }

    response do_signed_request(http::verb method,
                               const std::string& endpoint,
                               const std::string& query,
                               const std::string& body)
    {
        maybe_resync_clock();

        // Venue requires alphabetical query keys in the prehash AND URL.
        const std::string sorted_query = bitget::sort_query_string(query);

        long long ts = static_cast<long long>(bitget::local_time_ms())
                     + clock_offset_ms_.load(std::memory_order_acquire);
        char ts_buf[32];
        int ts_n = std::snprintf(ts_buf, sizeof(ts_buf), "%lld", ts);
        std::string_view ts_sv(ts_buf, static_cast<std::size_t>(ts_n > 0 ? ts_n : 0));

        std::string sign;
        {
            std::lock_guard<std::mutex> lk(signer_mu_);
            sign = signer_.sign(bitget::build_prehash(
                ts_sv, verb_name(method), endpoint, sorted_query, body));
        }

        const std::string target =
            sorted_query.empty() ? endpoint : (endpoint + "?" + sorted_query);

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
                    req.set("locale", "en-US");
                    if (paptrading_)
                        req.set("paptrading", "1");

                    if (signed_req)
                    {
                        req.set("ACCESS-KEY", api_key_);
                        req.set("ACCESS-SIGN", std::string(sign));
                        req.set("ACCESS-TIMESTAMP", std::string(ts));
                        req.set("ACCESS-PASSPHRASE", api_passphrase_);
                    }

                    if (!body.empty())
                    {
                        req.body() = body;
                        req.prepare_payload();
                    }
                    else if (method == http::verb::post)
                    {
                        // Empty POST body still needs Content-Length for some
                        // intermediaries; prepare_payload handles it.
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
                            std::cerr << "BitgetRestClient: connection failed "
                                         "before request was sent ("
                                      << (op_ec ? op_ec.message()
                                                : std::string("deadline elapsed"))
                                      << "), reconnecting and retrying once\n";
                            continue;
                        }

                        if (io_done)
                            std::cerr << "BitgetRestClient: request failed: "
                                      << op_ec.message() << "\n";
                        else
                            std::cerr << "BitgetRestClient: request timed out "
                                         "after "
                                      << timeout_ms << " ms\n";
                        r = {};
                        break;
                    }

                    r.status = static_cast<int>(res.result_int());
                    r.body = res.body();
                    r.business_ok = bitget::is_business_success(r.status, r.body);

                    if (r.status >= 400)
                    {
                        std::cerr << "BitgetRestClient: HTTP " << r.status
                                  << " - "
                                  << bitget::truncate_for_log(r.body) << "\n";
                    }
                    else if (!r.business_ok && r.status >= 200 && r.status < 300)
                    {
                        // Critical Bitget quirk: HTTP 200 + code!="00000".
                        std::cerr << "BitgetRestClient: business error code="
                                  << bitget::extract_business_code(r.body)
                                  << " - "
                                  << bitget::truncate_for_log(r.body) << "\n";
                    }

                    break;
                }
                catch (const std::exception& e)
                {
                    close_connection_locked();

                    if (!request_sent && attempt == 0)
                    {
                        std::cerr << "BitgetRestClient: connect failed ("
                                  << e.what()
                                  << "), reconnecting and retrying once\n";
                        continue;
                    }

                    std::cerr << "BitgetRestClient: request failed: "
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

#endif // HAS_BITGET
