#pragma once
#ifdef HAS_GATE

// Gate.io REST client (APIv4) — Boost.Beast TLS + HMAC-SHA512 signing.
// Pure helpers (time parse, contract probe, path builders) are offline-testable.
// Pattern mirrors BitgetRestClient / BinanceRestClient (keep-alive, TLS session
// cache, async write+read with optional run_for timeout).

#include "execution/instrument.h"
#include "providers/gate/gate_auth.h"
#include "providers/gate/gate_endpoints.h"
#include "providers/gate/gate_parser.h"

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
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

namespace gate {

// Local wall-clock ms (system_clock). Used for offset math vs /spot/time.
inline int64_t local_time_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Unix seconds for REST Timestamp header (after offset correction).
inline int64_t local_time_s()
{
    return local_time_ms() / 1000;
}

// Gate REST success is HTTP 2xx. Error bodies carry label/message, not a
// Bitget-style business code envelope.
inline bool is_http_success(int http_status)
{
    return http_status >= 200 && http_status < 300;
}

// Extract error label from Gate JSON error body (cold path diagnostics).
inline std::string_view extract_error_label(std::string_view body)
{
    auto sv = extract_sv_string(body, "label");
    if (!sv.empty()) return sv;
    return extract_sv_string(body, "message");
}

// Parse GET /api/v4/spot/time body → server_time in **milliseconds**.
// Accepts number or string. Returns false on miss/malformed.
inline bool parse_server_time_ms(std::string_view body, long long& out_ms)
{
    auto sv = extract_sv_number(body, "server_time");
    if (sv.empty())
        sv = extract_sv_string(body, "server_time");
    // Tolerate camelCase if Gate ever renames the field.
    if (sv.empty())
        sv = extract_sv_number(body, "serverTime");
    if (sv.empty())
        sv = extract_sv_string(body, "serverTime");
    if (sv.empty()) return false;
    int64_t parsed = 0;
    if (!parse_int64_sv(sv, parsed)) return false;
    // Guard: if value looks like seconds (pre-year-2286 in s, post-2001 in ms
    // threshold), promote to ms. Gate docs say ms; this softens unit mixups.
    if (parsed > 0 && parsed < 1'000'000'000'000LL)
        parsed *= 1000;
    out_ms = static_cast<long long>(parsed);
    return true;
}

// Convert corrected wall-clock ms → Unix seconds string for SIGN Timestamp.
inline std::string timestamp_seconds_string(long long wall_ms)
{
    const long long s = wall_ms / 1000;
    char buf[32];
    int n = std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(s));
    if (n <= 0) return "0";
    return std::string(buf, static_cast<std::size_t>(n));
}

// Full signed path for a futures REST resource (includes /api/v4).
// `tail` e.g. "/contracts/BTC_USDT" or "/orders".
inline std::string futures_sign_path(const endpoints& ep, std::string_view tail)
{
    return futures_path(ep, tail);
}

// Public contract path for GET .../contracts/{symbol}.
inline std::string contract_path(const endpoints& ep, std::string_view symbol)
{
    std::string tail;
    tail.reserve(11 + symbol.size());
    tail.append("/contracts/");
    tail.append(symbol.data(), symbol.size());
    return futures_path(ep, tail);
}

// Spot time path (ms server_time).
inline std::string time_path(const endpoints& ep)
{
    return spot_time_path(ep);
}

// Contract probe result (startup gate + encoder precision + notional).
struct contract_probe
{
    bool ok = false;            // tradable + required fields present
    bool found = false;         // response looked like a contract object
    bool trading = false;       // not in_delisting / status tradable
    instrument_spec spec;
    double quanto_multiplier = 0.0;
    std::string note;
    std::string status;         // raw status / delisting flag summary
    std::string name;           // contract name from body
};

// Notional in USDT for linear contracts:
//   notional ≈ |size_contracts| * quanto_multiplier * mark_price
inline double notional_usdt(double abs_size_contracts,
                            double mark_price,
                            double quanto_multiplier)
{
    if (abs_size_contracts < 0.0) abs_size_contracts = -abs_size_contracts;
    return abs_size_contracts * quanto_multiplier * mark_price;
}

// Parse GET /futures/{settle}/contracts/{symbol} body (canned or live).
// Pure — no network. Gate returns a single object (not an array).
inline contract_probe parse_contract_response(std::string_view body,
                                              std::string_view want_symbol = {})
{
    contract_probe out;
    if (body.empty())
    {
        out.note = "contract: empty body";
        return out;
    }

    // Error envelope: {"label":"...","message":"..."}
    auto label = extract_sv_string(body, "label");
    if (!label.empty() && extract_sv_string(body, "name").empty()
        && extract_sv_number(body, "name").empty())
    {
        out.note = "contract: error label ";
        out.note.append(label);
        auto msg = extract_sv_string(body, "message");
        if (!msg.empty())
        {
            out.note.append(": ");
            out.note.append(msg);
        }
        return out;
    }

    auto name = extract_sv_string(body, "name");
    if (name.empty())
        name = extract_sv_string(body, "contract");
    if (name.empty())
    {
        out.note = "contract: missing name";
        return out;
    }

    out.found = true;
    out.name.assign(name.data(), name.size());
    out.spec.symbol = out.name;

    if (!want_symbol.empty()
        && normalize_contract_symbol(want_symbol)
               != normalize_contract_symbol(name))
    {
        out.note = "contract: name mismatch want=";
        out.note.append(want_symbol);
        out.note.append(" got=");
        out.note.append(name);
        return out;
    }

    auto parse_d = [](std::string_view obj, std::string_view key,
                      double& dst) -> bool {
        auto sv = extract_sv_string(obj, key);
        if (sv.empty()) sv = extract_sv_number(obj, key);
        double v = 0.0;
        if (sv.empty() || !parse_double_sv(sv, v)) return false;
        dst = v;
        return true;
    };

    // tick_size ← order_price_round
    if (!parse_d(body, "order_price_round", out.spec.tick_size))
    {
        out.note = "contract: missing/invalid order_price_round";
        return out;
    }

    // min_qty ← order_size_min; lot_size ← order_size_step (default 1)
    if (!parse_d(body, "order_size_min", out.spec.min_qty))
    {
        out.note = "contract: missing/invalid order_size_min";
        return out;
    }
    if (!parse_d(body, "order_size_step", out.spec.lot_size)
        || out.spec.lot_size <= 0.0)
    {
        // Gate integer contracts historically omit step → lot = 1.
        out.spec.lot_size = 1.0;
    }

    if (!parse_d(body, "quanto_multiplier", out.quanto_multiplier)
        || out.quanto_multiplier <= 0.0)
    {
        out.note = "contract: missing/invalid quanto_multiplier";
        return out;
    }

    // Optional fees / min notional (not always present).
    parse_d(body, "maker_fee_rate", out.spec.maker_rate);
    parse_d(body, "taker_fee_rate", out.spec.taker_rate);

    // Trading gate: in_delisting == true → refuse. status string if present.
    auto delist = extract_sv_optional_bool(body, "in_delisting");
    auto st = extract_sv_string(body, "status");
    if (!st.empty())
        out.status.assign(st.data(), st.size());
    else if (delist && *delist)
        out.status = "in_delisting";
    else
        out.status = "trading";

    const bool delisting = delist && *delist;
    // status values observed: empty, "trading", "delisting", "offline"
    const bool status_bad =
        (!st.empty() && st != "trading" && st != "open" && st != "online");
    out.trading = !delisting && !status_bad;

    if (!out.trading)
    {
        out.ok = false;
        out.note = "contract: not trading (status='";
        out.note.append(out.status);
        out.note.append("')");
        return out;
    }

    if (out.spec.tick_size <= 0.0 || out.spec.lot_size <= 0.0
        || out.spec.min_qty <= 0.0)
    {
        out.ok = false;
        out.note = "contract: tick/lot/min_qty must be > 0";
        return out;
    }

    out.ok = true;
    out.note = "contract: ok";
    return out;
}

// Truncate body for cold-path logs (pair with redact_for_log for secrets).
inline std::string truncate_for_log(std::string_view body,
                                    std::size_t max_len = 240)
{
    if (body.size() <= max_len) return std::string(body);
    std::string out(body.substr(0, max_len));
    out.append("...");
    return out;
}

} // namespace gate

// ---------------------------------------------------------------------------
// GateRestClient — Boost.Beast TLS HTTP client (Gate APIv4).
// ---------------------------------------------------------------------------
class GateRestClient
{
public:
    GateRestClient(
        const std::string& api_key,
        const std::string& api_secret,
        const std::string& host = "api.gateio.ws",
        const std::string& port = "443",
        const std::string& time_path = "/api/v4/spot/time")
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

        // TLS session resumption: skip asymmetric crypto on reconnect.
        SSL_CTX* raw = ctx_.native_handle();
        SSL_CTX_set_session_cache_mode(
            raw, SSL_SESS_CACHE_CLIENT | SSL_SESS_CACHE_NO_INTERNAL_STORE);
        SSL_CTX_set_ex_data(raw, ex_data_index(), this);
        SSL_CTX_sess_set_new_cb(raw, &GateRestClient::on_new_session);
    }

    // Convenience: host/port/time from endpoints preset.
    explicit GateRestClient(const gate::endpoints& ep,
                            const std::string& api_key = {},
                            const std::string& api_secret = {})
        : GateRestClient(api_key, api_secret, ep.rest_host, ep.rest_port,
                         gate::time_path(ep))
    {}

    ~GateRestClient()
    {
        close_connection_locked();
        SSL_SESSION* sess = cached_session_.exchange(
            nullptr, std::memory_order_acq_rel);
        if (sess) SSL_SESSION_free(sess);
    }

    GateRestClient(const GateRestClient&) = delete;
    GateRestClient& operator=(const GateRestClient&) = delete;
    GateRestClient(GateRestClient&&) = delete;
    GateRestClient& operator=(GateRestClient&&) = delete;

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
        // True when HTTP 2xx (Gate has no Bitget-style business code).
        bool business_ok = false;
    };

    // Signed GET: `path` is full URL path including /api/v4; query without '?'.
    response get(const std::string& path, const std::string& query = "")
    {
        return do_signed_request(http::verb::get, path, query, /*body=*/"");
    }

    // Signed POST with exact JSON body (no re-serialize after sign).
    response post_json(const std::string& path, const std::string& json_body)
    {
        return do_signed_request(http::verb::post, path, /*query=*/"",
                                 json_body);
    }

    // Signed DELETE; query without leading '?'.
    response del(const std::string& path, const std::string& query = "")
    {
        return do_signed_request(http::verb::delete_, path, query,
                                 /*body=*/"");
    }

    // Signed POST with optional query (e.g. DMS countdown_cancel_all).
    response post_json_query(const std::string& path,
                             const std::string& query,
                             const std::string& json_body)
    {
        return do_signed_request(http::verb::post, path, query, json_body);
    }

    response get_unsigned(const std::string& path,
                          const std::string& params = "")
    {
        return execute(http::verb::get,
                       path + (params.empty() ? "" : "?" + params),
                       "");
    }

    // server_time_ms - local_time_ms; negative = local ahead. LLONG_MIN on fail.
    using get_fn_t =
        std::function<response(const std::string&, const std::string&)>;

    static long long server_time_offset_ms(
        const get_fn_t& get_fn,
        const std::string& time_path = "/api/v4/spot/time")
    {
        if (!get_fn) return LLONG_MIN;
        try
        {
            auto resp = get_fn(time_path, "");
            if (!gate::is_http_success(resp.status))
                return LLONG_MIN;

            long long server_ms = 0;
            if (!gate::parse_server_time_ms(resp.body, server_ms))
                return LLONG_MIN;
            long long local_ms = static_cast<long long>(gate::local_time_ms());
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
                std::cerr << "GateRestClient: clock resync failed; "
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

    const std::string& host() const { return host_; }
    const std::string& time_path() const { return time_path_; }

private:
    static int ex_data_index()
    {
        static const int idx = SSL_CTX_get_ex_new_index(
            0, const_cast<char*>("GateRestClient::this"),
            nullptr, nullptr, nullptr);
        return idx;
    }

    static int on_new_session(SSL* ssl, SSL_SESSION* session)
    {
        SSL_CTX* ctx = SSL_get_SSL_CTX(ssl);
        auto* self = static_cast<GateRestClient*>(
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
    gate::HmacSha512HexSigner signer_;
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
                               const std::string& path,
                               const std::string& query,
                               const std::string& body)
    {
        maybe_resync_clock();

        // SIGN Timestamp is Unix **seconds** (not ms). Apply ms offset then /1000.
        const long long wall_ms =
            static_cast<long long>(gate::local_time_ms())
            + clock_offset_ms_.load(std::memory_order_acquire);
        const std::string ts = gate::timestamp_seconds_string(wall_ms);

        std::string sign;
        {
            std::lock_guard<std::mutex> lk(signer_mu_);
            sign = signer_.sign(gate::build_rest_sign_string(
                verb_name(method), path, query, body, ts));
        }

        const std::string target =
            query.empty() ? path : (path + "?" + query);

        return execute(method, target, body, /*signed_req=*/true, ts, sign);
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
                    req.set(http::field::accept, "application/json");
                    // G9: decimal size wire form for REST.
                    req.set("X-Gate-Size-Decimal", "1");

                    if (signed_req)
                    {
                        req.set("KEY", api_key_);
                        req.set("SIGN", std::string(sign));
                        req.set("Timestamp", std::string(ts));
                    }

                    if (method == http::verb::post
                        || method == http::verb::put
                        || !body.empty())
                    {
                        req.set(http::field::content_type, "application/json");
                    }

                    if (!body.empty())
                    {
                        req.body() = body;
                        req.prepare_payload();
                    }
                    else if (method == http::verb::post
                             || method == http::verb::put)
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
                            std::cerr << "GateRestClient: connection failed "
                                         "before request was sent ("
                                      << (op_ec ? op_ec.message()
                                                : std::string("deadline elapsed"))
                                      << "), reconnecting and retrying once\n";
                            continue;
                        }

                        if (io_done)
                            std::cerr << "GateRestClient: request failed: "
                                      << op_ec.message() << "\n";
                        else
                            std::cerr << "GateRestClient: request timed out "
                                         "after "
                                      << timeout_ms << " ms\n";
                        r = {};
                        break;
                    }

                    r.status = static_cast<int>(res.result_int());
                    r.body = res.body();
                    r.business_ok = gate::is_http_success(r.status);

                    if (r.status >= 400)
                    {
                        std::cerr << "GateRestClient: HTTP " << r.status
                                  << " - "
                                  << gate::redact_for_log(
                                         gate::truncate_for_log(r.body))
                                  << "\n";
                    }

                    break;
                }
                catch (const std::exception& e)
                {
                    close_connection_locked();

                    if (!request_sent && attempt == 0)
                    {
                        std::cerr << "GateRestClient: connect failed ("
                                  << e.what()
                                  << "), reconnecting and retrying once\n";
                        continue;
                    }

                    std::cerr << "GateRestClient: request failed: "
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

#endif // HAS_GATE
