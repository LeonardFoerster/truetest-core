#pragma once
#ifdef HAS_BITGET

#include "execution/instrument.h"
#include "providers/bitget/bitget_auth.h"
#include "providers/bitget/bitget_parser.h"
#include "providers/bounded_ws_open.h"
#include "providers/recovery_payload.h"

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

inline std::string_view extract_business_code(std::string_view body);

// Bitget envelope: success iff HTTP 2xx AND "code":"00000".
// Fail-closed when code is missing/malformed (Bitget always sends it).
inline bool is_business_success(int http_status, std::string_view body)
{
    if (http_status < 200 || http_status >= 300) return false;
    return extract_business_code(body) == "00000";
}

// Extract business code string ("00000" on success). Empty if absent.
inline std::string_view extract_business_code(std::string_view body)
{
    if (!provider_recovery::is_authoritative_object(body)) return {};
    std::string_view code;
    return provider_recovery::top_level_scalar_text(body, "code", code)
        ? code : std::string_view{};
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

    std::string_view arr;
    if (!provider_recovery::top_level_member(body, "data", arr)
        || !provider_recovery::is_authoritative_object_array(arr))
    {
        out.note = "instruments: missing data array";
        return out;
    }

    std::string_view matched;
    std::size_t matches = 0;
    const bool rows_ok = provider_recovery::every_top_level_object(
        arr, [&](std::string_view obj) {
        std::string_view sym;
        if (!provider_recovery::top_level_plain_string(
                obj, "symbol", sym))
            return false;
        if (want_symbol.empty() || sym == want_symbol)
        {
            ++matches;
            matched = obj;
        }
        return true;
    });

    if (!rows_ok || matches != 1 || matched.empty())
    {
        out.note = rows_ok && matches == 0
            ? "instruments: symbol not found"
            : "instruments: ambiguous or malformed symbol rows";
        if (!want_symbol.empty())
        {
            out.note.push_back(' ');
            out.note.append(want_symbol);
        }
        return out;
    }

    out.found = true;
    {
        std::string_view sym;
        if (!provider_recovery::top_level_plain_string(
                matched, "symbol", sym))
        {
            out.note = "instruments: malformed symbol";
            return out;
        }
        out.spec.symbol.assign(sym.data(), sym.size());
    }

    std::string_view st;
    if (!provider_recovery::top_level_plain_string(
            matched, "status", st))
    {
        out.note = "instruments: missing status";
        return out;
    }
    out.status.assign(st.data(), st.size());
    // UTA instruments: "online" = tradable. Other states refuse live.
    out.trading = (st == "online");

    auto parse_required_d = [](std::string_view obj, std::string_view key,
                               double& dst) {
        std::string_view sv;
        double v = 0.0;
        if (!provider_recovery::top_level_scalar_text(obj, key, sv)
            || !parse_double_sv(sv, v))
            return false;
        dst = v;
        return true;
    };
    auto parse_optional_d = [](std::string_view obj, std::string_view key,
                               double& dst) {
        std::string_view raw;
        const auto state = provider_recovery::payload_parser(obj)
            .inspect_top_level_member(key, raw);
        if (state == provider_recovery::payload_parser::member_result::missing)
            return true;
        if (state
            != provider_recovery::payload_parser::member_result::unique)
            return false;
        std::string_view sv;
        double v = 0.0;
        if (!provider_recovery::top_level_scalar_text(obj, key, sv)
            || !parse_double_sv(sv, v))
            return false;
        dst = v;
        return true;
    };

    // UTA v3: priceMultiplier = tick step; quantityMultiplier = lot step.
    if (!parse_required_d(matched, "priceMultiplier", out.spec.tick_size)
        || !parse_required_d(
            matched, "quantityMultiplier", out.spec.lot_size)
        || !parse_optional_d(matched, "minOrderQty", out.spec.min_qty)
        || !parse_optional_d(
            matched, "minOrderAmount", out.spec.min_notional)
        || !parse_optional_d(matched, "makerFeeRate", out.spec.maker_rate)
        || !parse_optional_d(matched, "takerFeeRate", out.spec.taker_rate))
    {
        out.note = "instruments: malformed instrument precision fields";
        return out;
    }

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
        close_safety_connection_locked();
        SSL_SESSION* sess = nullptr;
        {
            std::lock_guard<std::mutex> lk(session_mu_);
            sess = std::exchange(cached_session_, nullptr);
        }
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
        // True once an HTTP write was initiated; after that, a zero-byte
        // completion cannot prove that no encrypted bytes reached the peer.
        bool request_written = false;
    };

    static bool request_may_have_been_written(
        const beast::error_code& ec, std::size_t bytes_transferred) noexcept
    {
        return !ec || bytes_transferred > 0;
    }

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

    response safety_get(const std::string& endpoint,
                        const std::string& query,
                        std::chrono::milliseconds deadline,
                        const std::atomic<bool>* cancelled = nullptr)
    {
        return do_safety_signed_request(
            http::verb::get, endpoint, query, "", deadline, cancelled);
    }

    response safety_post_json(const std::string& endpoint,
                              const std::string& json_body,
                              std::chrono::milliseconds deadline,
                              const std::atomic<bool>* cancelled = nullptr)
    {
        return do_safety_signed_request(
            http::verb::post, endpoint, "", json_body, deadline, cancelled);
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

    bool ensure_clock_fresh_for_order(std::chrono::milliseconds deadline)
    {
        if (!resync_due(steady_now_ms(),
                        last_sync_steady_ms_.load(std::memory_order_acquire),
                        sync_interval_ms_))
            return true;
        const auto expires_at = std::chrono::steady_clock::now() + deadline;
        std::unique_lock<std::timed_mutex> lk(clock_sync_mu_, std::defer_lock);
        if (!lk.try_lock_until(expires_at)) return false;
        if (!resync_due(steady_now_ms(),
                        last_sync_steady_ms_.load(std::memory_order_acquire),
                        sync_interval_ms_))
            return true;
        const auto prior = per_call_timeout();
        const auto now = std::chrono::steady_clock::now();
        if (now >= expires_at) return false;
        set_per_call_timeout(std::chrono::duration_cast<std::chrono::milliseconds>(
            expires_at - now));
        const bool ok = resync_clock_now();
        set_per_call_timeout(prior);
        return ok;
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
    response do_safety_signed_request(http::verb method,
                                      const std::string& endpoint,
                                      const std::string& query,
                                      const std::string& body,
                                      std::chrono::milliseconds deadline,
                                      const std::atomic<bool>* cancelled)
    {
        if (deadline <= std::chrono::milliseconds::zero()) return {};
        const auto expires_at = std::chrono::steady_clock::now() + deadline;
        std::unique_lock<std::timed_mutex> io_lock(
            safety_io_mu_, std::defer_lock);
        if (!io_lock.try_lock_until(expires_at)
            || (cancelled && cancelled->load(std::memory_order_acquire)))
            return {};
        const std::string sorted_query = bitget::sort_query_string(query);
        const long long ts = static_cast<long long>(bitget::local_time_ms())
                           + clock_offset_ms_.load(std::memory_order_acquire);
        char ts_buf[32];
        const int ts_n = std::snprintf(ts_buf, sizeof(ts_buf), "%lld", ts);
        const std::string timestamp(
            ts_buf, static_cast<std::size_t>(ts_n > 0 ? ts_n : 0));
        std::string signature;
        {
            std::unique_lock<std::timed_mutex> lk(signer_mu_, std::defer_lock);
            if (!lk.try_lock_until(expires_at)) return {};
            signature = signer_.sign(bitget::build_prehash(
                timestamp, verb_name(method), endpoint, sorted_query, body));
        }
        const std::string target = sorted_query.empty()
            ? endpoint : endpoint + "?" + sorted_query;
        const auto now = std::chrono::steady_clock::now();
        if (now >= expires_at) return {};
        return execute_safety_once(
            method, target, body, timestamp, signature,
            std::chrono::duration_cast<std::chrono::milliseconds>(
                expires_at - now));
    }

    response execute_safety_once(http::verb method,
                                 const std::string& target,
                                 const std::string& body,
                                 const std::string& timestamp,
                                 const std::string& signature,
                                 std::chrono::milliseconds deadline)
    {
        const bool warm = safety_connected_ && safety_ioc_ && safety_stream_
            && beast::get_lowest_layer(*safety_stream_).socket().is_open();
        if (!warm)
        {
            close_safety_connection_locked();
            safety_ioc_.emplace();
            safety_stream_.emplace(*safety_ioc_, ctx_);
            if (!provider_ws::configure_tls_peer_identity(
                    safety_stream_->native_handle(), host_))
            {
                close_safety_connection_locked();
                return {};
            }
            if (SSL_SESSION* sess = acquire_cached_session())
            {
                SSL_set_session(safety_stream_->native_handle(), sess);
                SSL_SESSION_free(sess);
            }
        }

        auto& ioc = *safety_ioc_;
        auto& stream = *safety_stream_;
        std::optional<tcp::resolver> resolver;
        if (!warm) resolver.emplace(ioc);
        std::optional<net::steady_timer> timer;
        timer.emplace(ioc);
        beast::flat_buffer buffer;
        http::request<http::string_body> req{method, target, 11};
        http::response<http::string_body> res;
        beast::error_code terminal_ec;
        bool completed = false;
        bool timed_out = false;
        bool request_written = false;

        req.set(http::field::host, host_);
        req.set(http::field::user_agent, "TrueTest/1.0 safety");
        req.set(http::field::content_type, "application/json");
        req.set("locale", "en-US");
        if (paptrading_) req.set("paptrading", "1");
        req.set("ACCESS-KEY", api_key_);
        req.set("ACCESS-SIGN", signature);
        req.set("ACCESS-TIMESTAMP", timestamp);
        req.set("ACCESS-PASSPHRASE", api_passphrase_);
        if (!body.empty()) req.body() = body;
        if (!body.empty() || method == http::verb::post) req.prepare_payload();

        auto finish = [&](beast::error_code ec) {
            if (completed) return;
            completed = true;
            terminal_ec = ec;
            timer->cancel();
        };
        timer->expires_after(deadline);
        timer->async_wait([&](beast::error_code ec) {
            if (ec || completed) return;
            timed_out = true;
            if (resolver) resolver->cancel();
            beast::error_code ignored;
            beast::get_lowest_layer(stream).socket().close(ignored);
            finish(net::error::timed_out);
        });
        auto start_request = [&] {
            request_written = true;
            http::async_write(
                stream, req,
                [&](beast::error_code ec, std::size_t bytes_transferred) {
                    request_written = request_written
                        || request_may_have_been_written(ec, bytes_transferred);
                    if (ec || completed) { finish(ec); return; }
                    http::async_read(
                        stream, buffer, res,
                        [&](beast::error_code read_ec, std::size_t) {
                            finish(read_ec);
                        });
                });
        };
        if (warm)
        {
            start_request();
        }
        else
        {
            resolver->async_resolve(
                host_, port_,
                [&](beast::error_code ec,
                    tcp::resolver::results_type results) {
                    if (ec || completed) { finish(ec); return; }
                    beast::get_lowest_layer(stream).async_connect(
                        results,
                        [&](beast::error_code connect_ec,
                            const tcp::resolver::results_type::endpoint_type&) {
                            if (connect_ec || completed)
                            { finish(connect_ec); return; }
                            stream.async_handshake(
                                ssl::stream_base::client,
                                [&](beast::error_code handshake_ec) {
                                    if (handshake_ec || completed)
                                    { finish(handshake_ec); return; }
                                    start_request();
                                });
                        });
                });
        }
        ioc.restart();
        ioc.run_for(deadline + std::chrono::milliseconds(100));
        if (!completed)
        {
            timed_out = true;
            if (resolver) resolver->cancel();
            beast::error_code ignored;
            beast::get_lowest_layer(stream).socket().close(ignored);
            terminal_ec = net::error::timed_out;
            ioc.stop();
        }

        if (timed_out || terminal_ec)
        {
            std::cerr << "BitgetRestClient: safety request failed once: "
                      << (timed_out ? "deadline elapsed"
                                    : terminal_ec.message()) << "\n";
            timer.reset();
            resolver.reset();
            close_safety_connection_locked();
            response failed;
            failed.request_written = request_written;
            return failed;
        }
        safety_connected_ = res.keep_alive();
        response out;
        out.status = static_cast<int>(res.result_int());
        out.body = res.body();
        out.business_ok = bitget::is_business_success(out.status, out.body);
        out.request_written = true;
        if (!safety_connected_)
        {
            timer.reset();
            resolver.reset();
            close_safety_connection_locked();
        }
        return out;
    }

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
        SSL_SESSION* old = nullptr;
        {
            std::lock_guard<std::mutex> lk(self->session_mu_);
            old = std::exchange(self->cached_session_, session);
        }
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
    SSL_SESSION* cached_session_ = nullptr;
    std::mutex session_mu_;

    SSL_SESSION* acquire_cached_session()
    {
        std::lock_guard<std::mutex> lk(session_mu_);
        SSL_SESSION* sess = cached_session_;
        if (sess) SSL_SESSION_up_ref(sess);
        return sess;
    }
    bitget::HmacSha256Base64Signer signer_;
    std::timed_mutex signer_mu_;
    std::timed_mutex safety_io_mu_;
    std::optional<net::io_context> safety_ioc_;
    std::optional<beast::ssl_stream<beast::tcp_stream>> safety_stream_;
    bool safety_connected_ = false;
    std::atomic<long long> per_call_timeout_ms_{0};

    std::atomic<long long> clock_offset_ms_{0};
    std::atomic<long long> last_sync_steady_ms_{0};
    long long sync_interval_ms_ = 5 * 60 * 1000;
    std::atomic<bool> sync_failed_logged_{false};
    std::timed_mutex clock_sync_mu_;

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
            std::lock_guard<std::timed_mutex> lk(signer_mu_);
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

                    request_sent = true;
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

                    request_sent = request_sent || write_done;

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

    // Caller holds safety_io_mu_, except during single-threaded destruction.
    void close_safety_connection_locked()
    {
        safety_connected_ = false;
        if (safety_stream_)
        {
            beast::error_code ec;
            beast::get_lowest_layer(*safety_stream_).socket().close(ec);
        }
        safety_stream_.reset();
        safety_ioc_.reset();
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

        if (!provider_ws::configure_tls_peer_identity(
                stream.native_handle(), host_))
            throw std::runtime_error("TLS peer identity setup failed");

        if (SSL_SESSION* sess = acquire_cached_session())
        {
            SSL_set_session(stream.native_handle(), sess);
            SSL_SESSION_free(sess);
        }

        const auto timeout = per_call_timeout();
        if (timeout > std::chrono::milliseconds::zero())
        {
            auto resolver = std::make_shared<tcp::resolver>(*persistent_ioc_);
            auto results = std::make_shared<tcp::resolver::results_type>();
            if (!provider_ws::run_bounded(
                    *persistent_ioc_, timeout,
                    [resolver, results, host = host_, port = port_](auto done) {
                        resolver->async_resolve(
                            host, port,
                            [resolver, results, done](
                                beast::error_code ec,
                                tcp::resolver::results_type found) mutable {
                                if (!ec) *results = std::move(found);
                                done(ec);
                            });
                    },
                    [resolver] { resolver->cancel(); }))
                throw std::runtime_error("bounded DNS resolve failed");
            if (!provider_ws::run_bounded(
                    *persistent_ioc_, timeout,
                    [&, results](auto done) {
                        net::async_connect(
                            lowest, *results,
                            [results, done](beast::error_code ec,
                                            const tcp::endpoint&) mutable {
                                done(ec);
                            });
                    },
                    [&] {
                        beast::error_code ignored;
                        lowest.cancel(ignored);
                        lowest.close(ignored);
                    }))
                throw std::runtime_error("bounded TCP connect failed");
            if (!provider_ws::run_bounded(
                    *persistent_ioc_, timeout,
                    [&](auto done) {
                        stream.async_handshake(
                            ssl::stream_base::client,
                            [done](beast::error_code ec) mutable { done(ec); });
                    },
                    [&] {
                        beast::error_code ignored;
                        lowest.cancel(ignored);
                        lowest.close(ignored);
                    }))
                throw std::runtime_error("bounded TLS handshake failed");
            connected_ = true;
            return;
        }

        // Re-resolve on every reconnect (CDN / GSLB can shift IPs).
        tcp::resolver resolver(*persistent_ioc_);
        auto resolver_results = resolver.resolve(host_, port_);
        net::connect(lowest, resolver_results);
        stream.handshake(ssl::stream_base::client);
        connected_ = true;
    }
};

#endif // HAS_BITGET
