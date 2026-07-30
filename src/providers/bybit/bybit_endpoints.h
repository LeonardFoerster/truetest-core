#pragma once
#ifdef HAS_BYBIT

#include <string>

namespace bybit {

// Bybit V5 linear (USDT perpetual) endpoint set.
// category=linear is request-level; hosts differ by environment.
// Demo public market data reuses mainnet public streams (no dedicated
// demo public MD host) — private REST/WS are demo-specific.
struct endpoints
{
    std::string rest_host;
    std::string rest_port;
    std::string ws_public_host;
    std::string ws_private_host;
    std::string ws_port;
    std::string ws_public_path;
    std::string ws_private_path;
    bool is_testnet = false;
    bool is_demo = false;
};

// Common REST path helpers (linear USDT-M). Used by later phases;
// kept here so unit tests can pin the contract.
namespace paths {
inline constexpr const char* market_time        = "/v5/market/time";
inline constexpr const char* instruments_info   = "/v5/market/instruments-info";
inline constexpr const char* kline              = "/v5/market/kline";
inline constexpr const char* order_create       = "/v5/order/create";
inline constexpr const char* order_amend        = "/v5/order/amend";
inline constexpr const char* order_cancel       = "/v5/order/cancel";
inline constexpr const char* order_cancel_all   = "/v5/order/cancel-all";
inline constexpr const char* order_realtime     = "/v5/order/realtime";
inline constexpr const char* position_list      = "/v5/position/list";
inline constexpr const char* wallet_balance     = "/v5/account/wallet-balance";
inline constexpr const char* account_info       = "/v5/account/info";
} // namespace paths

inline endpoints linear_mainnet()
{
    return {
        "api.bybit.com",
        "443",
        "stream.bybit.com",
        "stream.bybit.com",
        "443",
        "/v5/public/linear",
        "/v5/private",
        false,
        false,
    };
}

inline endpoints linear_testnet()
{
    return {
        "api-testnet.bybit.com",
        "443",
        "stream-testnet.bybit.com",
        "stream-testnet.bybit.com",
        "443",
        "/v5/public/linear",
        "/v5/private",
        true,
        false,
    };
}

// Demo Trading: REST + private WS on demo hosts; public MD uses mainnet
// streams (Bybit has no separate demo public market-data host).
inline endpoints linear_demo()
{
    return {
        "api-demo.bybit.com",
        "443",
        "stream.bybit.com",
        "stream-demo.bybit.com",
        "443",
        "/v5/public/linear",
        "/v5/private",
        false,
        true,
    };
}

// Host classification helpers used by register factory / config overrides.
inline bool looks_like_testnet_host(const std::string& host)
{
    return host.find("testnet") != std::string::npos;
}

inline bool looks_like_demo_host(const std::string& host)
{
    return host.find("demo") != std::string::npos
        || host.find("api-demo") != std::string::npos
        || host.find("stream-demo") != std::string::npos;
}

} // namespace bybit

#endif // HAS_BYBIT
