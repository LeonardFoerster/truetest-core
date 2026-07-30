#pragma once

#include <string>

namespace binance {

enum class venue
{
    spot,
    usdm_futures,
};

struct endpoints
{
    std::string ws_host;
    std::string ws_port;
    std::string rest_host;
    std::string rest_port;
    bool is_testnet = false;
    venue v = venue::spot;
};

inline endpoints spot_mainnet()
{
    return {
        "stream.binance.com", "9443",
        "api.binance.com",    "443",
        false,
        venue::spot
    };
}

inline endpoints spot_testnet()
{
    return {
        "stream.testnet.binance.vision", "9443",
        "testnet.binance.vision",        "443",
        true,
        venue::spot
    };
}

inline endpoints usdm_mainnet()
{
    return {
        "fstream.binancefuture.com", "443",
        "fapi.binance.com",          "443",
        false,
        venue::usdm_futures
    };
}

// USDT-M futures testnet uses the dedicated `binancefuture.com` domain
// (not `*.testnet.binance.vision`). The WS host is `stream.binancefuture.com`
// - note the absence of any "testnet" substring in it; the REST host
// `testnet.binancefuture.com` does carry it.
inline endpoints usdm_testnet()
{
    return {
        "stream.binancefuture.com",  "9443",
        "testnet.binancefuture.com", "443",
        true,
        venue::usdm_futures
    };
}

// `binancefuture.com` is exclusively Binance's futures testnet domain;
// mainnet futures lives under plain `binance.com`. Matching either
// substring keeps spot detection unchanged while picking up the futures
// WS host that has no "testnet" token in it.
inline bool looks_like_testnet_host(const std::string& host)
{
    return host.find("testnet") != std::string::npos
        || host.find("binancefuture") != std::string::npos;
}

inline endpoints from_host(const std::string& ws_host)
{
    if (looks_like_testnet_host(ws_host))
    {
        auto ep = spot_testnet();
        if (!ws_host.empty()) ep.ws_host = ws_host;
        return ep;
    }
    auto ep = spot_mainnet();
    if (!ws_host.empty()) ep.ws_host = ws_host;
    return ep;
}

}
