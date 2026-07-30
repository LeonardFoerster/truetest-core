#pragma once
#ifdef HAS_GATE

// Low-level Gate field extractors (Phase 0 scaffold).
// Full trades / order_book_update / candlesticks → provider::event mapping
// lands in Phase 1 (gate_combined_parser.h). Hand-rolled only — no nlohmann.

#include "providers/gate/gate_json_util.h"

#include <optional>
#include <string>
#include <string_view>

namespace gate {

inline std::string_view extract_sv_string(std::string_view json,
                                          std::string_view key)
{
    return json_util::extract(json, key);
}

inline std::string_view extract_sv_number(std::string_view json,
                                          std::string_view key)
{
    return json_util::extract(json, key);
}

inline bool extract_sv_bool(std::string_view json, std::string_view key)
{
    auto v = json_util::extract(json, key);
    return v == "true" || v == "1";
}

inline std::optional<bool> extract_sv_optional_bool(std::string_view json,
                                                    std::string_view key)
{
    auto colon = json_util::find_key(json, key);
    if (colon == std::string_view::npos) return std::nullopt;
    auto v = json_util::value_at_colon(json, colon);
    if (v == "true" || v == "1") return true;
    if (v == "false" || v == "0") return false;
    return std::nullopt;
}

// Gate contract symbols are underscore form: BTC_USDT (G2).
// Accept already-canonical or bare BTCUSDT → BTC_USDT (best-effort).
inline std::string normalize_contract_symbol(std::string_view raw)
{
    std::string out;
    out.reserve(raw.size() + 1);
    for (unsigned char c : raw)
        out.push_back(static_cast<char>(
            (c >= 'a' && c <= 'z') ? (c - 'a' + 'A') : c));

    if (out.find('_') != std::string::npos)
        return out;

    // Heuristic: insert '_' before common quote currencies.
    static constexpr const char* quotes[] = {
        "USDT", "USDC", "USD", "BTC", "ETH"
    };
    for (const char* q : quotes)
    {
        const std::size_t qlen = std::char_traits<char>::length(q);
        if (out.size() > qlen
            && out.compare(out.size() - qlen, qlen, q) == 0)
        {
            out.insert(out.size() - qlen, 1, '_');
            return out;
        }
    }
    return out;
}

} // namespace gate

#endif // HAS_GATE
