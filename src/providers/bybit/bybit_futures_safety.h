#pragma once
#ifdef HAS_BYBIT

// Startup advisories for Bybit V5 linear futures (cold path).
// Input: GET /v5/position/list body (or equivalent list envelope).
// Warnings only — refusal is operator-driven via margin_type_strict
// (enforced in provider open when set).

#include "providers/bybit/bybit_parser.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bybit::futures {

struct advisory
{
    enum class kind
    {
        margin_mode_mismatch,
        liquidation_close,
        hedge_mode,
    };

    kind        k;
    std::string symbol;
    std::string note;
};

inline std::string normalize_margin_type(std::string_view s)
{
    if (s.empty())
        return {};
    char c0 = static_cast<char>(
        std::tolower(static_cast<unsigned char>(s[0])));
    if (c0 == 'i')
        return "ISOLATED";
    if (c0 == 'c')
        return "CROSSED";
    // Bybit tradeMode: 0 = cross, 1 = isolated (numeric string).
    if (s == "0")
        return "CROSSED";
    if (s == "1")
        return "ISOLATED";
    std::string up;
    up.reserve(s.size());
    for (unsigned char c : s)
        up.push_back(static_cast<char>(std::toupper(c)));
    return up;
}

inline double to_double_or_zero(std::string_view sv)
{
    double v = 0.0;
    if (!parse_double_sv(sv, v))
        return 0.0;
    return v;
}

// Return non-empty error if any row for want_symbol (or any row when empty)
// has positionIdx 1 or 2 (hedge legs). Empty list / idx 0 → ok.
inline std::string check_one_way_position_mode(
    std::string_view position_json,
    std::string_view want_symbol = {})
{
    auto result = detail::extract_object(position_json, "result");
    const std::string_view root = result.empty() ? position_json : result;
    auto arr = detail::extract_array(root, "list");
    if (arr.empty())
        arr = detail::extract_array(root, "data");

    std::string err;
    auto consider = [&](std::string_view obj) {
        if (!err.empty()) return;
        auto sym = extract_sv_string(obj, "symbol");
        if (!want_symbol.empty() && !sym.empty() && sym != want_symbol)
            return;
        auto idx = extract_sv_number(obj, "positionIdx");
        if (idx.empty())
            idx = extract_sv_string(obj, "positionIdx");
        if (idx == "1" || idx == "2")
        {
            err = "account appears to be in hedge mode (positionIdx=";
            err.append(idx);
            err.append("); TrueTest Bybit provider requires one-way "
                       "(positionIdx=0)");
        }
    };

    if (!arr.empty())
    {
        detail::for_each_array_object(arr, consider);
        return err;
    }
    auto data_obj = detail::extract_object(root, "data");
    if (!data_obj.empty())
        consider(data_obj);
    else if (!result.empty())
        consider(result);
    return err;
}

// position_json: full REST envelope or list body. Filters to want_symbol
// when non-empty. expected_margin empty → skip margin check.
// liquidation_warn_pct <= 0 → skip liq distance.
inline std::vector<advisory> compute_advisories(
    std::string_view position_json,
    std::string_view want_symbol,
    std::string_view expected_margin_type,
    double liquidation_warn_pct)
{
    std::vector<advisory> out;
    const auto expected_norm = normalize_margin_type(expected_margin_type);
    const bool check_margin = !expected_norm.empty();
    const bool check_liq = liquidation_warn_pct > 0.0;
    if (!check_margin && !check_liq)
        return out;

    auto result = detail::extract_object(position_json, "result");
    const std::string_view root = result.empty() ? position_json : result;

    auto arr = detail::extract_array(root, "list");
    if (arr.empty())
        arr = detail::extract_array(root, "data");

    auto consider = [&](std::string_view obj) {
        auto sym = extract_sv_string(obj, "symbol");
        if (!want_symbol.empty() && !sym.empty() && sym != want_symbol)
            return;

        auto size_sv = extract_sv_string(obj, "size");
        if (size_sv.empty())
            size_sv = extract_sv_number(obj, "size");
        const double size = std::abs(to_double_or_zero(size_sv));
        if (size < 1e-12)
            return;

        auto side = extract_sv_string(obj, "side");
        const bool is_short =
            side == "Sell" || side == "sell" || side == "SELL"
            || side == "Short" || side == "short";
        double signed_qty = is_short ? -size : size;
        if (side.empty())
        {
            double raw = to_double_or_zero(size_sv);
            if (raw < 0)
                signed_qty = raw;
        }

        const std::string sym_s(sym.empty() ? want_symbol : sym);

        if (check_margin)
        {
            auto mt = extract_sv_string(obj, "tradeMode");
            if (mt.empty())
                mt = extract_sv_number(obj, "tradeMode");
            if (mt.empty())
                mt = extract_sv_string(obj, "marginMode");
            if (mt.empty())
                mt = extract_sv_string(obj, "marginType");
            const auto mt_norm = normalize_margin_type(mt);
            if (!mt_norm.empty() && mt_norm != expected_norm)
            {
                advisory a;
                a.k = advisory::kind::margin_mode_mismatch;
                a.symbol = sym_s;
                char buf[192];
                std::snprintf(buf, sizeof(buf),
                    "%s margin mode is %s, operator configured %s",
                    sym_s.c_str(), mt_norm.c_str(), expected_norm.c_str());
                a.note = buf;
                out.push_back(std::move(a));
            }
        }

        if (check_liq)
        {
            auto mark_sv = extract_sv_string(obj, "markPrice");
            if (mark_sv.empty())
                mark_sv = extract_sv_number(obj, "markPrice");
            auto liq_sv = extract_sv_string(obj, "liqPrice");
            if (liq_sv.empty())
                liq_sv = extract_sv_number(obj, "liqPrice");
            if (liq_sv.empty())
                liq_sv = extract_sv_string(obj, "liquidationPrice");
            if (liq_sv.empty())
                liq_sv = extract_sv_number(obj, "liquidationPrice");

            const double mark = to_double_or_zero(mark_sv);
            const double liq = to_double_or_zero(liq_sv);
            if (mark <= 0.0 || liq <= 0.0)
                return;

            const double distance = (signed_qty > 0.0)
                ? (mark - liq) / mark
                : (liq - mark) / mark;
            if (distance < liquidation_warn_pct)
            {
                advisory a;
                a.k = advisory::kind::liquidation_close;
                a.symbol = sym_s;
                char buf[220];
                std::snprintf(buf, sizeof(buf),
                    "%s position is %.2f%% from liquidation "
                    "(mark=%.4f liq=%.4f threshold=%.2f%%)",
                    sym_s.c_str(), distance * 100.0, mark, liq,
                    liquidation_warn_pct * 100.0);
                a.note = buf;
                out.push_back(std::move(a));
            }
        }
    };

    if (!arr.empty())
    {
        detail::for_each_array_object(arr, consider);
        return out;
    }

    auto data_obj = detail::extract_object(root, "data");
    if (!data_obj.empty())
        consider(data_obj);
    return out;
}

// Strict margin refusal helper (mirrors Binance/Bitget). Liq distance is
// advisory-only.
inline std::optional<std::string> first_strict_refusal(
    const std::vector<advisory>& advisories,
    bool margin_type_strict)
{
    if (!margin_type_strict)
        return std::nullopt;
    for (const auto& a : advisories)
    {
        if (a.k == advisory::kind::margin_mode_mismatch)
            return a.note;
    }
    return std::nullopt;
}

} // namespace bybit::futures

#endif // HAS_BYBIT
