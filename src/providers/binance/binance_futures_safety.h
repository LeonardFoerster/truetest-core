#pragma once
#ifdef HAS_BINANCE

#include "providers/binance/binance_parser.h"
#include "providers/recovery_payload.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace binance::futures {

// Operator-facing startup advisory. Not a refusal - startup proceeds
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
    const auto equals_ci = [](std::string_view lhs, std::string_view rhs) {
        if (lhs.size() != rhs.size()) return false;
        for (std::size_t i = 0; i < lhs.size(); ++i)
            if (ascii_upper(lhs[i]) != ascii_upper(rhs[i])) return false;
        return true;
    };
    if (equals_ci(s, "isolated")) return "ISOLATED";
    if (equals_ci(s, "cross") || equals_ci(s, "crossed")) return "CROSSED";
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
// array). `expected_margin_type` empty -> margin-mode check disabled.
// `liquidation_warn_pct` <= 0 -> liquidation-distance check disabled.
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

// Strict startup is an evidence gate, not an advisory.  When enabled, every
// way in which the scoped positionRisk response fails to prove the configured
// margin mode returns a refusal reason. Even a flat scoped account returns a
// symbol row, so an empty array is missing evidence rather than proof. Any row
// must be uniquely scoped and complete even when positionAmt is zero.
inline std::optional<std::string> strict_margin_probe_refusal(
    int http_status,
    std::string_view body,
    std::string_view target_symbol,
    std::string_view expected_margin_type,
    bool strict)
{
    if (!strict) return std::nullopt;

    const auto expected = detail::normalize_margin_type(expected_margin_type);
    if (expected != "ISOLATED" && expected != "CROSSED")
        return "strict margin mode requires ISOLATED or CROSSED expectation";
    if (http_status < 200 || http_status >= 300)
        return "positionRisk HTTP response did not prove strict margin mode";
    if (!provider_recovery::is_authoritative_object_array(body))
        return "positionRisk payload is not an authoritative object array";

    std::size_t rows = 0;
    std::string failure;
    const bool valid = provider_recovery::every_top_level_object(
        body, [&](std::string_view row) {
            ++rows;
            if (rows > 1)
            {
                failure = "positionRisk returned multiple rows for a scoped symbol";
                return false;
            }

            std::string_view symbol;
            std::string_view position_amt;
            std::string_view margin_type;
            double parsed_position = 0.0;
            if (!provider_recovery::top_level_plain_string(row, "symbol", symbol)
                || symbol != target_symbol)
            {
                failure = "positionRisk row is missing the configured symbol identity";
                return false;
            }
            if (!provider_recovery::top_level_scalar_text(
                    row, "positionAmt", position_amt)
                || !binance::parse_double_sv(position_amt, parsed_position)
                || !std::isfinite(parsed_position))
            {
                failure = "positionRisk row has no authoritative positionAmt";
                return false;
            }
            if (!provider_recovery::top_level_plain_string(
                    row, "marginType", margin_type))
            {
                failure = "positionRisk row has no authoritative marginType";
                return false;
            }
            const auto actual = detail::normalize_margin_type(margin_type);
            if (actual != "ISOLATED" && actual != "CROSSED")
            {
                failure = "positionRisk row contains an unsupported marginType";
                return false;
            }
            if (actual != expected)
            {
                failure = std::string(target_symbol) + " margin mode is "
                    + actual + ", operator configured " + expected;
                return false;
            }
            return true;
        });
    if (!valid)
        return failure.empty()
            ? std::optional<std::string>{
                  "positionRisk payload could not be validated"}
            : std::optional<std::string>{std::move(failure)};
    if (rows == 0)
        return "positionRisk returned no row for the configured symbol";
    return std::nullopt;
}

}

#endif // HAS_BINANCE
