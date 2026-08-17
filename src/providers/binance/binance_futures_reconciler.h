#pragma once
#ifdef HAS_BINANCE

// ============================================================
// LIVE-SAFETY SURFACE — Phase 1 freeze (see prod.md)
// Any edit requires explicit two-person CCB review + 4 h
// mainnet shadow run on engine_shadow before merge.
// Authoritative path list: scripts/check-live-safety-freeze.sh
// ============================================================

#include "execution/live_safety.h"
#include "execution/portfolio.h"
#include "providers/binance/binance_parser.h"
#include "providers/binance/binance_rest_client.h"
#include "providers/recovery_payload.h"

#include <cmath>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

// Startup gate for USDT-M futures. Compares local cash vs venue
// `availableBalance` (top-level on /fapi/v2/account) and local
// per-symbol position quantity vs venue `positionAmt`
// (/fapi/v2/positionRisk?symbol=...).
// Differences from the spot reconciler:
//   - venue cash comes from a single `availableBalance` field, not a
//     per-asset `free + locked` walk over the balances array;
//   - position amount is signed (long > 0, short < 0); local
//     `position.qty` follows the same convention on futures-paying
//     strategies, so within_tolerance just works;
//   - no testnet-reset shortcut. The futures testnet does not wipe on
//     the same cadence as spot, and the spot heuristic ("venue near-zero,
//     local non-zero, treat as reset") would otherwise mask real drift.
//     Operators must explicitly clear local checkpoint files instead.
class BinanceFuturesReconciler : public IReconciler
{
public:
    using get_fn = std::function<BinanceRestClient::response(
        const std::string&, const std::string&)>;
    struct injected_get_t {};
    static constexpr injected_get_t injected_get{};

    BinanceFuturesReconciler(std::shared_ptr<BinanceRestClient> rest,
                             std::string symbol,
                             bool is_testnet = false)
        : rest_(std::move(rest))
        , symbol_(std::move(symbol))
        , is_testnet_(is_testnet)
    {}

    BinanceFuturesReconciler(injected_get_t, get_fn get,
                             std::string symbol,
                             bool is_testnet = false)
        : get_(std::move(get))
        , symbol_(std::move(symbol))
        , is_testnet_(is_testnet)
    {}

    std::string reconcile(const portfolio& local, double tolerance_bps) override
    {
        if (!rest_ && !get_)
            return "BinanceFuturesReconciler: no REST client";

        const auto expires_at = std::chrono::steady_clock::now()
                              + std::chrono::seconds(5);
        const auto remaining = [&]() {
            const auto now = std::chrono::steady_clock::now();
            if (now >= expires_at) return std::chrono::milliseconds{0};
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                expires_at - now);
        };

        // Both endpoints are signed USER_DATA on futures.
        const auto request = [&](const std::string& endpoint,
                                 const std::string& params) {
            return rest_ ? rest_->safety_get(endpoint, params, remaining())
                         : get_(endpoint, params);
        };
        auto pos_resp = request(
            "/fapi/v2/positionRisk", "symbol=" + binance::url_encode(symbol_));
        if (pos_resp.status < 200 || pos_resp.status >= 300)
        {
            const auto body = binance::redact_for_log(pos_resp.body);
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                "BinanceFuturesReconciler: /fapi/v2/positionRisk failed "
                "(HTTP %d): %.160s",
                pos_resp.status, body.c_str());
            return buf;
        }

        auto acct_resp = request("/fapi/v2/account", "");
        if (acct_resp.status < 200 || acct_resp.status >= 300)
        {
            const auto body = binance::redact_for_log(acct_resp.body);
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                "BinanceFuturesReconciler: /fapi/v2/account failed "
                "(HTTP %d): %.160s",
                acct_resp.status, body.c_str());
            return buf;
        }

        double ex_available = 0.0;
        if (!extract_available_balance(acct_resp.body, ex_available))
            return "BinanceFuturesReconciler: availableBalance not found in "
                   "/fapi/v2/account response";

        const double local_cash = local.get_cash();
        if (!within_tolerance(local_cash, ex_available, tolerance_bps))
        {
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                "cash drift: local=%.8f exchange=%.8f available "
                "(> %.2f bps)",
                local_cash, ex_available, tolerance_bps);
            return buf;
        }

        double ex_position_amt = 0.0;
        if (!extract_position_amt(pos_resp.body, ex_position_amt, symbol_))
            return "BinanceFuturesReconciler: positionAmt not found in "
                   "/fapi/v2/positionRisk response";

        const auto& positions = local.get_positions();
        auto it = positions.find(symbol_);
        const double local_qty = (it != positions.end()) ? it->second.qty : 0.0;

        if (!within_tolerance(local_qty, ex_position_amt, tolerance_bps))
        {
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                "position drift: local=%.8f exchange=%.8f (> %.2f bps)",
                local_qty, ex_position_amt, tolerance_bps);
            return buf;
        }

        return {};
    }

    // Top-level `availableBalance` is emitted before the `assets[]` array,
    // so the first match is the one we want. Tests inject canned bodies.
    static bool extract_available_balance(std::string_view json, double& out)
    {
        std::string_view value;
        if (!provider_recovery::top_level_member(
                json, "availableBalance", value))
            return false;
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
            value = value.substr(1, value.size() - 2);
        auto sv = value;
        return binance::parse_double_sv(sv, out);
    }

    // /fapi/v2/positionRisk?symbol=X is filtered server-side. In one-way
    // mode that response is a single-element array; we read the first
    // `positionAmt`. Hedge mode is gated out at provider open() — if it
    // somehow slipped through, the first entry's amount may be wrong, but
    // we don't try to merge LONG+SHORT here.
    static bool extract_position_amt(std::string_view json, double& out,
                                     std::string_view wanted_symbol = {})
    {
        bool found = false;
        std::size_t row_count = 0;
        const bool schema_ok = provider_recovery::every_top_level_object(
            json, [&](std::string_view row) {
                ++row_count;
                std::string_view symbol;
                std::string_view amount;
                if (!provider_recovery::top_level_plain_string(
                        row, "symbol", symbol)
                    || !provider_recovery::top_level_scalar_text(
                        row, "positionAmt", amount))
                    return false;
                double parsed = 0.0;
                if (!binance::parse_double_sv(amount, parsed))
                    return false;
                if (!wanted_symbol.empty() && symbol != wanted_symbol)
                    return false;
                if (found) return false;
                out = parsed;
                found = true;
                return true;
            });
        return schema_ok && row_count == 1 && found;
    }

    bool is_testnet() const { return is_testnet_; }

private:
    static bool within_tolerance(double a, double b, double tolerance_bps)
    {
        const double bound = std::max(std::abs(a), std::abs(b));
        if (bound < 1e-8) return true;
        const double rel = std::abs(a - b) / bound * 10000.0;
        return rel <= tolerance_bps;
    }

    std::shared_ptr<BinanceRestClient> rest_;
    get_fn get_;
    std::string symbol_;
    bool is_testnet_ = false;
};

#endif // HAS_BINANCE
