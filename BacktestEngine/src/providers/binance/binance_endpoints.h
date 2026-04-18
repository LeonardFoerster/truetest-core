#pragma once

#include <string>

namespace binance {

struct endpoints
{
    std::string ws_host;
    std::string ws_port;
    std::string rest_host;
    std::string rest_port;
    bool is_testnet = false;
};

inline endpoints spot_mainnet()
{
    return {
        "stream.binance.com", "9443",
        "api.binance.com",    "443",
        false
    };
}

inline endpoints spot_testnet()
{
    return {
        "stream.testnet.binance.vision", "9443",
        "testnet.binance.vision",        "443",
        true
    };
}

inline bool looks_like_testnet_host(const std::string& host)
{
    return host.find("testnet") != std::string::npos;
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

} // namespace binance
