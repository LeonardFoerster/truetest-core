#pragma once
#ifdef HAS_GATE

// Startup advisories + dual_mode refuse helpers for Gate.io USDT-M futures
// (cold path only). dual_mode / hedge is a hard open() refusal (G4);
// margin mismatch and liquidation distance are advisories (strict margin
// optionally promoted to refusal by the provider).

#include "providers/gate/gate_parser.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gate::futures {

struct advisory
{
    enum class kind
    {
        margin_mode_mismatch,
        liquidation_close,
        dual_mode,
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

// Parse GET .../accounts body. Non-empty = refuse open() (G4).
// Checks `in_dual_mode` boolean and, when present, `position_mode`
// (must be single / one_way / empty).
inline std::optional<std::string> dual_mode_refusal(
    std::string_view accounts_json)
{
    if (accounts_json.empty())
        return std::string(
            "GateFutures: accounts body empty (cannot verify dual_mode)");

    // Error envelope without account fields → refuse (fail-closed).
    auto label = extract_sv_string(accounts_json, "label");
    if (!label.empty()
        && extract_sv_string(accounts_json, "available").empty()
        && extract_sv_number(accounts_json, "available").empty()
        && !extract_sv_optional_bool(accounts_json, "in_dual_mode").has_value())
    {
        std::string note = "GateFutures: accounts error label ";
        note.append(label);
        return note;
    }

    auto dual = extract_sv_optional_bool(accounts_json, "in_dual_mode");
    if (dual && *dual)
    {
        return std::string(
            "GateFutures: in_dual_mode=true (hedge/dual mode not supported "
            "in v1 — switch to single/one-way)");
    }

    auto mode = extract_sv_string(accounts_json, "position_mode");
    if (mode.empty())
        mode = extract_sv_string(accounts_json, "mode");
    if (!mode.empty()
        && mode != "single"
        && mode != "one_way"
        && mode != "oneway"
        && mode != "SINGLE"
        && mode != "ONE_WAY")
    {
        std::string note =
            "GateFutures: position_mode='";
        note.append(mode);
        note.append("' (only single/one-way supported in v1)");
        return note;
    }

    // Field missing: treat as single (historical Gate accounts omit it).
    // Operator still gets an advisory path via compute_advisories if needed.
    return std::nullopt;
}

// True when accounts body explicitly declares dual/hedge mode.
inline bool is_dual_mode(std::string_view accounts_json)
{
    auto dual = extract_sv_optional_bool(accounts_json, "in_dual_mode");
    return dual.has_value() && *dual;
}

// position_json: single object, array body, or list envelope.
// want_symbol empty → all rows. expected_margin empty → skip margin check.
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

    auto consider = [&](std::string_view obj) {
        auto sym = extract_sv_string(obj, "contract");
        if (sym.empty())
            sym = extract_sv_string(obj, "symbol");
        if (!want_symbol.empty() && !sym.empty()
            && normalize_contract_symbol(sym)
                   != normalize_contract_symbol(want_symbol))
            return;

        auto size_sv = extract_sv_string(obj, "size");
        if (size_sv.empty())
            size_sv = extract_sv_number(obj, "size");
        const double signed_qty = to_double_or_zero(size_sv);
        if (std::abs(signed_qty) < 1e-12)
            return; // flat — no advisory

        const std::string sym_s(
            sym.empty() ? want_symbol : sym);

        if (check_margin)
        {
            // Gate: margin_mode / marginType; leverage "0" historically
            // means cross on some payloads.
            auto mt = extract_sv_string(obj, "margin_mode");
            if (mt.empty())
                mt = extract_sv_string(obj, "marginMode");
            if (mt.empty())
                mt = extract_sv_string(obj, "marginType");
            if (mt.empty())
            {
                auto lev = extract_sv_string(obj, "leverage");
                if (lev.empty())
                    lev = extract_sv_number(obj, "leverage");
                if (lev == "0")
                    mt = "cross";
            }
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
            auto mark_sv = extract_sv_string(obj, "mark_price");
            if (mark_sv.empty())
                mark_sv = extract_sv_number(obj, "mark_price");
            if (mark_sv.empty())
                mark_sv = extract_sv_string(obj, "markPrice");
            if (mark_sv.empty())
                mark_sv = extract_sv_number(obj, "markPrice");

            auto liq_sv = extract_sv_string(obj, "liq_price");
            if (liq_sv.empty())
                liq_sv = extract_sv_number(obj, "liq_price");
            if (liq_sv.empty())
                liq_sv = extract_sv_string(obj, "liquidation_price");
            if (liq_sv.empty())
                liq_sv = extract_sv_number(obj, "liquidation_price");
            if (liq_sv.empty())
                liq_sv = extract_sv_string(obj, "liquidationPrice");
            if (liq_sv.empty())
                liq_sv = extract_sv_number(obj, "liquidationPrice");

            const double mark = to_double_or_zero(mark_sv);
            const double liq = to_double_or_zero(liq_sv);
            // Gate uses "0" liq when not applicable / cross unconstrained.
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

    if (!position_json.empty() && position_json.front() == '[')
    {
        json_util::for_each_array_object(position_json, consider);
        return out;
    }

    auto arr = json_util::extract_array(position_json, "positions");
    if (arr.empty())
        arr = json_util::extract_array(position_json, "data");
    if (!arr.empty())
    {
        json_util::for_each_array_object(arr, consider);
        return out;
    }

    // Single object (GET .../positions/{contract}).
    if (!position_json.empty() && position_json.front() == '{')
        consider(position_json);
    return out;
}

// Strict margin refusal helper (mirrors Binance/Bitget). dual_mode is always
// a hard refuse via dual_mode_refusal(); liq distance is advisory-only.
inline std::optional<std::string> first_strict_refusal(
    const std::vector<advisory>& advisories,
    bool margin_type_strict)
{
    for (const auto& a : advisories)
    {
        if (a.k == advisory::kind::dual_mode)
            return a.note;
        if (margin_type_strict
            && a.k == advisory::kind::margin_mode_mismatch)
            return a.note;
    }
    return std::nullopt;
}

} // namespace gate::futures

#endif // HAS_GATE
