#ifdef HAS_BITGET

#include "providers/provider_registry.h"
#include "providers/bitget/bitget_futures_provider.h"
#include "providers/bitget/bitget_endpoints.h"

#include <cctype>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

std::shared_ptr<IProvider> make_bitget_futures(const provider_config& cfg)
{
    auto get = [&](const std::string& key) -> std::string {
        auto it = cfg.find(key);
        if (it == cfg.end()) return "";
        return it->second;
    };

    auto symbol = get("symbol");
    if (symbol.empty())
        throw std::runtime_error(
            "bitget-futures provider requires 'symbol' (e.g. BTCUSDT)");

    auto stream = get("stream");
    if (stream.empty())
        stream = "trade";

    const auto host_cfg = get("host");
    const auto port_cfg = get("port");

    // demo | testnet | paptrading → uta_demo (Bitget paptrading, not Binance testnet).
    const auto demo_cfg = get("demo");
    const auto testnet_cfg = get("testnet");
    const auto pap_cfg = get("paptrading");
    auto is_on = [](const std::string& v) {
        return v == "1" || v == "true" || v == "on" || v == "yes";
    };
    const bool want_demo =
        is_on(demo_cfg) || is_on(testnet_cfg) || is_on(pap_cfg);

    bitget::endpoints ep = want_demo
        ? bitget::uta_demo()
        : bitget::uta_mainnet();

    if (!host_cfg.empty())
    {
        ep.ws_public_host = host_cfg;
        ep.ws_private_host = host_cfg;
    }
    if (!port_cfg.empty())
        ep.ws_port = port_cfg;

    auto provider = std::make_shared<BitgetFuturesProvider>(
        symbol, stream,
        get("api_key"), get("api_secret"), get("api_passphrase"),
        ep.ws_public_host, ep.ws_port
    );
    provider->set_endpoints(ep);

    auto depth = get("depth_stream");
    if (!depth.empty())
        provider->set_depth_stream(depth);

    auto category = get("category");
    if (!category.empty())
        provider->set_category(category);

    auto surface = get("api_surface");
    if (!surface.empty())
    {
        // Only UTA is implemented (Phase 0–3). Classic mix/v2 = Phase 4.
        std::string lower;
        lower.reserve(surface.size());
        for (unsigned char c : surface)
            lower.push_back(static_cast<char>(std::tolower(c)));
        if (lower != "uta")
        {
            std::cerr << "bitget-futures: refusing api_surface='" << surface
                      << "' — only empty/'uta' is implemented "
                         "(classic mix/v2 is Phase 4 deferred).\n";
            throw std::runtime_error(
                "bitget-futures: api_surface='" + surface
                + "' is not implemented (only 'uta' or omit). "
                  "Classic mix/v2 is Phase 4 deferred.");
        }
        provider->set_api_surface(surface);
    }

    // Optional advisory inputs.
    auto margin_type = get("margin_type");
    if (!margin_type.empty())
        provider->set_expected_margin_type(margin_type);

    auto liq_pct_str = get("liquidation_warn_pct");
    if (!liq_pct_str.empty())
    {
        try
        {
            provider->set_liquidation_warn_pct(std::stod(liq_pct_str));
        }
        catch (...)
        {
            // Ignore malformed input; provider default applies.
        }
    }

    auto strict = get("margin_type_strict");
    if (strict == "1" || strict == "true")
        provider->set_margin_type_strict(true);

    // Position-based risk caps. Supplied malformed values are configuration
    // errors; silently disabling a requested live risk cap is fail-open.
    auto parse_double = [&](const char* key, void(BitgetFuturesProvider::*set)(double)) {
        auto raw = get(key);
        if (raw.empty()) return;
        std::size_t used = 0;
        double value = 0.0;
        try { value = std::stod(raw, &used); }
        catch (...) { throw std::runtime_error(std::string(key) + " must be a number"); }
        if (used != raw.size() || !std::isfinite(value) || value < 0.0)
            throw std::runtime_error(std::string(key) + " must be finite and non-negative");
        if (std::string_view(key) == "min_liquidation_distance_pct" && value > 1.0)
            throw std::runtime_error(std::string(key) + " must be in [0,1]");
        (provider.get()->*set)(value);
    };
    parse_double("max_notional_usdt",            &BitgetFuturesProvider::set_max_notional_usdt);
    parse_double("max_leverage",                 &BitgetFuturesProvider::set_max_leverage);
    parse_double("min_liquidation_distance_pct", &BitgetFuturesProvider::set_min_liquidation_distance_pct);
    parse_double("maintenance_margin_pct",       &BitgetFuturesProvider::set_maintenance_margin_pct);

    auto parse_int64 = [&](const char* key) -> std::optional<int64_t> {
        auto raw = get(key);
        if (raw.empty()) return std::nullopt;
        std::size_t used = 0;
        long long value = 0;
        try { value = std::stoll(raw, &used); }
        catch (...) { throw std::runtime_error(std::string(key) + " must be an integer"); }
        if (used != raw.size() || value < 0)
            throw std::runtime_error(std::string(key) + " must be non-negative");
        return static_cast<int64_t>(value);
    };
    auto countdown = parse_int64("dead_man_countdown_ms");
    auto heartbeat = parse_int64("dead_man_heartbeat_ms");
    if (heartbeat && *heartbeat > 0
        && (!countdown || *countdown == 0 || *heartbeat >= *countdown))
        throw std::runtime_error("dead_man_heartbeat_ms must be below a positive dead_man_countdown_ms");
    if (countdown) provider->set_dead_man_countdown_ms(*countdown);
    if (heartbeat) provider->set_dead_man_heartbeat_ms(*heartbeat);

    return provider;
}

} // namespace

REGISTER_PROVIDER("bitget-futures", make_bitget_futures);
REGISTER_PROVIDER("bitget", make_bitget_futures);

#endif // HAS_BITGET
