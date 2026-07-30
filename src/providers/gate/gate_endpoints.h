#pragma once
#ifdef HAS_GATE

#include <string>
#include <string_view>

namespace gate {

// Gate.io USDT-M futures settle currency. v1 is usdt-only (G1).
enum class settle_ccy
{
    usdt,
    // btc, // deferred — inverse not in v1
};

// Host / path set for public WS + REST. Settle is request-path level
// (futures/{settle}/...), not a separate host family for USDT-M.
struct endpoints
{
    std::string ws_host;     // e.g. fx-ws.gateio.ws
    std::string ws_port;     // 443
    std::string ws_path;     // /v4/ws/usdt
    std::string rest_host;   // api.gateio.ws (or fx-api.gateio.ws)
    std::string rest_port;   // 443
    std::string rest_prefix; // /api/v4
    settle_ccy  settle = settle_ccy::usdt;
    bool is_testnet = false;
};

// Mainnet USDT-M perpetual futures.
inline endpoints usdt_mainnet()
{
    return {
        "fx-ws.gateio.ws",
        "443",
        "/v4/ws/usdt",
        "api.gateio.ws",
        "443",
        "/api/v4",
        settle_ccy::usdt,
        false,
    };
}

// Testnet USDT-M. Host families verified against Gate APIv4 docs
// (fx-ws-testnet / api-testnet.gateapi.io); callers may override via
// provider_config host / rest_host if Gate renames them.
inline endpoints usdt_testnet()
{
    return {
        "fx-ws-testnet.gateio.ws",
        "443",
        "/v4/ws/usdt",
        "api-testnet.gateapi.io",
        "443",
        "/api/v4",
        settle_ccy::usdt,
        true,
    };
}

inline std::string settle_str(const endpoints& e)
{
    return e.settle == settle_ccy::usdt ? "usdt" : "btc";
}

// REST path WITH /api/v4 prefix (signing uses the full path, no host).
// `tail` e.g. "/orders" or "/contracts/BTC_USDT" →
//   "/api/v4/futures/usdt/orders"
inline std::string futures_path(const endpoints& e, std::string_view tail)
{
    std::string out;
    out.reserve(e.rest_prefix.size() + 16 + settle_str(e).size() + tail.size());
    out.append(e.rest_prefix);
    out.append("/futures/");
    out.append(settle_str(e));
    out.append(tail.data(), tail.size());
    return out;
}

// Spot time path (ms server_time). Used for clock-skew checks (Phase 2).
inline std::string spot_time_path(const endpoints& e)
{
    return e.rest_prefix + "/spot/time";
}

// Host looks like Gate testnet (testnet token or gateapi.io demo family).
inline bool looks_like_testnet_host(const std::string& host)
{
    return host.find("testnet") != std::string::npos
        || host.find("gateapi.io") != std::string::npos;
}

} // namespace gate

#endif // HAS_GATE
