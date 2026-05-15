#pragma once
#ifdef HAS_BINANCE

#include "providers/binance/binance_parser.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace binance::futures {

// Operator-facing startup advisory. Not a refusal — startup proceeds
// regardless. Surfaces conditions the engine cannot fix on its own
// (margin type vs operator expectation, position uncomfortably close
// to liquidation) so they don't get noticed for the first time when
// the venue forces the issue.
struct advisory
{
    enum class kind
    {
        margin_mode_mismatch,
        liquidation_close,
    };

    kind k;
    std::string symbol;
    std::string note;
};

namespace detail {

inline char ascii_upper(char c)
{
    return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
}

// Map Binance's "isolated" / "cross" / "crossed" / "ISOLATED" etc. to a
// single canonical form for comparison. Returns "" if input is empty
// (treat as "no opinion" so empty operator config disables the check).
inline std::string normalize_margin_type(std::string_view s)
{
    if (s.empty()) return {};
    char first = ascii_upper(s[0]);
    if (first == 'I') return "ISOLATED";
    if (first == 'C') return "CROSSED";
    return std::string(s);
}

// Walk a JSON array body, yielding each top-level `{...}` object body
// as a std::string. Brace-counter respects nested objects (positionRisk
// rows are flat in practice, but safer this way).
inline std::vector<std::string> split_objects(std::string_view body)
{
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < body.size())
    {
        auto open = body.find('{', i);
        if (open == std::string_view::npos) break;
        int depth = 0;
        std::size_t j = open;
        for (; j < body.size(); ++j)
        {
            if (body[j] == '{') ++depth;
            else if (body[j] == '}')
            {
                --depth;
                if (depth == 0) { ++j; break; }
            }
        }
        out.emplace_back(body.substr(open, j - open));
        i = j;
    }
    return out;
}

inline double to_double_or_zero(std::string_view sv)
{
    double out = 0.0;
    if (!binance::parse_double_sv(sv, out)) return 0.0;
    return out;
}

}

// `position_risk_json` is the body of GET /fapi/v2/positionRisk (an
// array). `expected_margin_type` empty → margin-mode check disabled.
// `liquidation_warn_pct` <= 0 → liquidation-distance check disabled.
inline std::vector<advisory> compute_advisories(
    std::string_view position_risk_json,
    std::string_view expected_margin_type,
    double liquidation_warn_pct)
{
    std::vector<advisory> out;
    const auto expected_norm = detail::normalize_margin_type(expected_margin_type);
    const bool check_margin = !expected_norm.empty();
    const bool check_liq    = liquidation_warn_pct > 0.0;

    if (!check_margin && !check_liq) return out;

    for (const auto& obj : detail::split_objects(position_risk_json))
    {
        const double pos_amt =
            detail::to_double_or_zero(binance::extract_sv_string(obj, "positionAmt"));
        if (std::abs(pos_amt) < 1e-12) continue;  // no live position

        const auto sym = std::string(binance::extract_sv_string(obj, "symbol"));

        if (check_margin)
        {
            const auto mt = binance::extract_sv_string(obj, "marginType");
            const auto mt_norm = detail::normalize_margin_type(mt);
            if (!mt_norm.empty() && mt_norm != expected_norm)
            {
                advisory a;
                a.k = advisory::kind::margin_mode_mismatch;
                a.symbol = sym;
                char buf[160];
                std::snprintf(buf, sizeof(buf),
                    "%s margin mode is %s, operator configured %s",
                    sym.c_str(), mt_norm.c_str(), expected_norm.c_str());
                a.note = buf;
                out.push_back(std::move(a));
            }
        }

        if (check_liq)
        {
            const double mark = detail::to_double_or_zero(
                binance::extract_sv_string(obj, "markPrice"));
            const double liq  = detail::to_double_or_zero(
                binance::extract_sv_string(obj, "liquidationPrice"));

            // Skip silently when either value is missing or zero.
            // Unfunded testnet accounts and just-opened positions both
            // legitimately surface zeros here; flagging them as warnings
            // would train operators to ignore the message.
            if (mark <= 0.0 || liq <= 0.0) continue;

            // Long: mark > liq, distance = (mark - liq) / mark.
            // Short: liq > mark, distance = (liq - mark) / mark.
            const double distance = (pos_amt > 0.0)
                ? (mark - liq) / mark
                : (liq - mark) / mark;

            if (distance < liquidation_warn_pct)
            {
                advisory a;
                a.k = advisory::kind::liquidation_close;
                a.symbol = sym;
                char buf[200];
                std::snprintf(buf, sizeof(buf),
                    "%s position is %.2f%% from liquidation "
                    "(mark=%.4f liq=%.4f threshold=%.2f%%)",
                    sym.c_str(), distance * 100.0, mark, liq,
                    liquidation_warn_pct * 100.0);
                a.note = buf;
                out.push_back(std::move(a));
            }
        }
    }
    return out;
}

// Operator-driven escalation. compute_advisories() always returns
// warnings; this helper decides whether one of them is severe enough
// (under the operator's strict flags) to refuse startup. Returns the
// note from the first advisory that should refuse, or nullopt to
// proceed. Pure for testability.
inline std::optional<std::string> first_strict_refusal(
    const std::vector<advisory>& advisories,
    bool margin_type_strict)
{
    if (margin_type_strict)
    {
        for (const auto& a : advisories)
        {
            if (a.k == advisory::kind::margin_mode_mismatch)
                return a.note;
        }
    }
    return std::nullopt;
}

}

#endif // HAS_BINANCE
