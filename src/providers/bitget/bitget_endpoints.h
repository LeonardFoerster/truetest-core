#pragma once
#ifdef HAS_BITGET

#include <string>

namespace bitget {

// Bitget UTA v3 endpoint set (USDT-M futures path lives under the same
// hosts; product selection is request-level, not host-level).
struct endpoints
{
    std::string ws_public_host;
    std::string ws_private_host;
    std::string ws_port;
    std::string rest_host;
    std::string rest_port;
    std::string ws_public_path;
    std::string ws_private_path;
    bool is_demo = false;
};

inline endpoints uta_mainnet()
{
    return {
        "ws.bitget.com",
        "ws.bitget.com",
        "443",
        "api.bitget.com",
        "443",
        "/v3/ws/public",
        "/v3/ws/private",
        false,
    };
}

// Demo / paptrading: WS moves to wspap.bitget.com; REST stays on
// api.bitget.com and is gated by the `paptrading: 1` header (Task 3+).
inline endpoints uta_demo()
{
    return {
        "wspap.bitget.com",
        "wspap.bitget.com",
        "443",
        "api.bitget.com",
        "443",
        "/v3/ws/public",
        "/v3/ws/private",
        true,
    };
}

} // namespace bitget

#endif // HAS_BITGET
