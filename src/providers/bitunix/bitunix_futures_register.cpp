#ifdef HAS_BITUNIX

#include "providers/provider_registry.h"
#include "providers/bitunix/bitunix_futures_provider.h"
#include "providers/bitunix/bitunix_endpoints.h"

#include <memory>
#include <stdexcept>
#include <string>

namespace {

std::shared_ptr<IProvider> make_bitunix_futures(const provider_config& cfg)
{
    auto get = [&](const std::string& key) -> std::string {
        auto it = cfg.find(key);
        if (it == cfg.end()) return "";
        return it->second;
    };

    auto symbol = get("symbol");
    if (symbol.empty())
        throw std::runtime_error(
            "bitunix-futures provider requires 'symbol' (e.g. BTCUSDT)");

    auto stream = get("stream");
    if (stream.empty())
        stream = "trade";

    auto provider = std::make_shared<BitunixFuturesProvider>(
        symbol, stream,
        get("api_key"), get("api_secret"),
        get("host"), get("port"));

    auto ep = bitunix::mainnet();
    if (!get("host").empty())
        ep.ws_public_host = get("host");
    if (!get("port").empty())
        ep.ws_port = get("port");
    provider->set_endpoints(ep);

    return provider;
}

} // namespace

namespace {
static const bool k_reg_bitunix_futures = []() {
    ProviderRegistry::instance().register_provider(
        "bitunix-futures", make_bitunix_futures);
    ProviderRegistry::instance().register_provider(
        "bitunix", make_bitunix_futures);
    return true;
}();
} // namespace

#endif // HAS_BITUNIX
