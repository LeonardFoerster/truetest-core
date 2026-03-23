#ifdef HAS_BINANCE

#include "providers/provider_registry.h"
#include "providers/binance/binance_provider.h"

#include <stdexcept>

REGISTER_PROVIDER("binance", [](const provider_config& cfg) {
    auto get = [&](const std::string& key) -> std::string {
        auto it = cfg.find(key);
        if (it == cfg.end()) return "";
        return it->second;
    };

    auto symbol = get("symbol");
    if (symbol.empty())
        throw std::runtime_error("binance provider requires 'symbol' (e.g. btcusdt)");

    auto stream = get("stream");
    if (stream.empty())
        stream = "trade";  // default to trade stream

    auto host = get("host");
    if (host.empty())
        host = "stream.binance.com";

    auto port = get("port");
    if (port.empty())
        port = "9443";

    return std::make_shared<BinanceProvider>(
        symbol, stream,
        get("api_key"), get("api_secret"),
        host, port
    );
});

#endif // HAS_BINANCE
