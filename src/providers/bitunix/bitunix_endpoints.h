#pragma once
#ifdef HAS_BITUNIX

#include <cctype>
#include <string>
#include <string_view>

namespace bitunix {

// Bitunix USDT-M futures hosts (public MD + REST). Live private path deferred.
struct endpoints
{
    std::string ws_public_host;
    std::string ws_private_host;
    std::string ws_port;
    std::string rest_host;
    std::string rest_port;
    std::string ws_public_path;
    std::string ws_private_path;
};

inline endpoints mainnet()
{
    return {
        "fapi.bitunix.com",
        "fapi.bitunix.com",
        "443",
        "fapi.bitunix.com",
        "443",
        "/public/",
        "/private/",
    };
}

// Uppercase symbol normalize (BTCUSDT).
inline std::string normalize_symbol(std::string_view raw)
{
    std::string out;
    out.reserve(raw.size());
    for (unsigned char c : raw)
        out.push_back(static_cast<char>(std::toupper(c)));
    return out;
}

} // namespace bitunix

#endif // HAS_BITUNIX
