#pragma once
#ifdef HAS_BYBIT

// Startup gate for Bybit V5 linear USDT perpetuals. Compares local cash vs
// venue UNIFIED wallet available USDT and local signed qty vs venue position.
//
// REST:
//   GET /v5/position/list?category=linear&symbol=X
//   GET /v5/account/wallet-balance?accountType=UNIFIED&coin=USDT
//
// Fail-closed: any HTTP / retCode / parse error → non-empty error string.
// No demo soft-pass (same philosophy as Binance/Bitget futures).

#include "execution/live_safety.h"
#include "execution/portfolio.h"
#include "providers/bybit/bybit_endpoints.h"
#include "providers/bybit/bybit_parser.h"
#include "providers/bybit/bybit_rest_client.h"

#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

class BybitFuturesReconciler : public IReconciler
{
public:
    using get_fn = std::function<BybitRestClient::response(
        const std::string& endpoint, const std::string& query)>;

    BybitFuturesReconciler(std::shared_ptr<BybitRestClient> rest,
                           std::string symbol,
                           std::string category = "linear",
                           bool is_demo = false)
        : rest_(std::move(rest))
        , symbol_(std::move(symbol))
        , category_(std::move(category))
        , is_demo_(is_demo)
    {
        if (rest_)
        {
            get_ = [r = rest_](const std::string& ep, const std::string& q) {
                return r->get(ep, q);
            };
        }
    }

    // Test seam: inject canned HTTP without a live REST client.
    BybitFuturesReconciler(get_fn get,
                           std::string symbol,
                           std::string category = "linear",
                           bool is_demo = false)
        : get_(std::move(get))
        , symbol_(std::move(symbol))
        , category_(std::move(category))
        , is_demo_(is_demo)
    {}

    std::string reconcile(const portfolio& local, double tolerance_bps) override
    {
        if (!get_)
            return "BybitFuturesReconciler: no REST client";

        // Positions first (matches Binance/Bitget order: position then cash).
        const std::string pos_q =
            "category=" + category_ + "&symbol=" + symbol_;
        auto pos_resp = get_(bybit::paths::position_list, pos_q);
        if (auto err = http_business_error("position/list", pos_resp))
            return *err;

        const std::string wallet_q = "accountType=UNIFIED&coin=USDT";
        auto acct_resp = get_(bybit::paths::wallet_balance, wallet_q);
        if (auto err = http_business_error("wallet-balance", acct_resp))
            return *err;

        double ex_available = 0.0;
        if (!extract_available_usdt(acct_resp.body, ex_available))
            return "BybitFuturesReconciler: available USDT not found in "
                   "/v5/account/wallet-balance response";

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
            return "BybitFuturesReconciler: position size not found in "
                   "/v5/position/list response";

        const auto& positions = local.get_positions();
        auto it = positions.find(symbol_);
        const double local_qty =
            (it != positions.end()) ? it->second.qty : 0.0;

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

    // Prefer result.list[0].totalAvailableBalance (UNIFIED account summary).
    // Fall back to coin[] entry for USDT: availableToWithdraw, then
    // walletBalance. First USDT match wins.
    static bool extract_available_usdt(std::string_view json, double& out)
    {
        auto result = bybit::detail::extract_object(json, "result");
        const std::string_view root = result.empty() ? json : result;

        auto list = bybit::detail::extract_array(root, "list");
        if (!list.empty())
        {
            bool found = false;
            bybit::detail::for_each_array_object(list, [&](std::string_view acct) {
                if (found) return;
                // Account-level available (UNIFIED).
                auto tab = bybit::extract_sv_string(acct, "totalAvailableBalance");
                if (tab.empty())
                    tab = bybit::extract_sv_number(acct, "totalAvailableBalance");
                if (!tab.empty() && bybit::parse_double_sv(tab, out))
                {
                    found = true;
                    return;
                }
                // Per-coin fallback inside this account object.
                if (extract_usdt_from_coin_array(acct, out))
                    found = true;
            });
            if (found) return true;
        }

        // Single-object / flat body fallbacks.
        auto tab = bybit::extract_sv_string(root, "totalAvailableBalance");
        if (tab.empty())
            tab = bybit::extract_sv_number(root, "totalAvailableBalance");
        if (!tab.empty() && bybit::parse_double_sv(tab, out))
            return true;

        return extract_usdt_from_coin_array(root, out);
    }

    // Signed qty from position/list body. Empty list → 0.0 (flat is valid).
    // side Buy → +size, side Sell → -size; size already signed → use as-is.
    // Returns false on unparseable size OR when want_symbol is set and a
    // non-empty list never matches that symbol (fail-closed).
    static bool extract_position_amt(std::string_view json, double& out,
                                     std::string_view want_symbol = {})
    {
        out = 0.0;

        auto result = bybit::detail::extract_object(json, "result");
        const std::string_view root = result.empty() ? json : result;

        auto arr = bybit::detail::extract_array(root, "list");
        if (arr.empty())
            arr = bybit::detail::extract_array(root, "data");

        if (arr.empty())
        {
            // Single object under result/data, or empty envelope (flat).
            auto data_obj = bybit::detail::extract_object(root, "data");
            if (data_obj.empty() && result.empty())
                return true; // empty / flat
            if (!result.empty()
                && bybit::extract_sv_string(result, "symbol").empty()
                && bybit::extract_sv_string(result, "size").empty()
                && bybit::extract_sv_number(result, "size").empty())
                return true;
            const auto obj = !data_obj.empty() ? data_obj : result;
            if (obj.empty())
                return true;
            return parse_position_row(obj, out, want_symbol);
        }

        bool matched = false;
        bool parse_ok = true;
        int row_count = 0;
        bybit::detail::for_each_array_object(arr, [&](std::string_view obj) {
            ++row_count;
            if (matched || !parse_ok) return;
            auto sym = bybit::extract_sv_string(obj, "symbol");
            if (!want_symbol.empty() && !sym.empty() && sym != want_symbol)
                return;
            double qty = 0.0;
            if (!parse_position_row(obj, qty, /*want=*/{}))
            {
                parse_ok = false;
                return;
            }
            out = qty;
            matched = true;
        });

        if (!parse_ok) return false;
        if (!matched && row_count > 0 && !want_symbol.empty())
            return false;
        return true;
    }

    bool is_demo() const { return is_demo_; }

private:
    static std::optional<std::string> http_business_error(
        const char* label, const BybitRestClient::response& resp)
    {
        if (resp.status < 200 || resp.status >= 300)
        {
            const std::string body_s = bybit::truncate_for_log(resp.body, 160);
            char buf[320];
            std::snprintf(buf, sizeof(buf),
                "BybitFuturesReconciler: %s failed (HTTP %d): %.160s",
                label, resp.status, body_s.c_str());
            return std::string(buf);
        }
        // Prefer explicit business_ok when set by RestClient; also re-check
        // envelope so injected test responses without business_ok still gate.
        if (!resp.business_ok
            && !bybit::is_business_success(resp.status, resp.body))
        {
            auto code = bybit::extract_ret_code(resp.body);
            const std::string code_s =
                code.empty() ? std::string("<missing>") : std::string(code);
            const std::string body_s = bybit::truncate_for_log(resp.body, 160);
            char buf[320];
            std::snprintf(buf, sizeof(buf),
                "BybitFuturesReconciler: %s business retCode %s: %.160s",
                label, code_s.c_str(), body_s.c_str());
            return std::string(buf);
        }
        return std::nullopt;
    }

    static bool extract_usdt_from_coin_array(std::string_view obj, double& out)
    {
        auto coins = bybit::detail::extract_array(obj, "coin");
        if (coins.empty())
        {
            auto coin = bybit::extract_sv_string(obj, "coin");
            if (coin == "USDT" || coin == "usdt")
                return parse_available_field(obj, out);
            return false;
        }
        bool found = false;
        bybit::detail::for_each_array_object(coins, [&](std::string_view c) {
            if (found) return;
            auto coin = bybit::extract_sv_string(c, "coin");
            if (coin != "USDT" && coin != "usdt") return;
            if (parse_available_field(c, out))
                found = true;
        });
        return found;
    }

    static bool parse_available_field(std::string_view obj, double& out)
    {
        // Prefer free-to-trade style fields before raw wallet balance.
        auto sv = bybit::extract_sv_string(obj, "availableToWithdraw");
        if (sv.empty())
            sv = bybit::extract_sv_number(obj, "availableToWithdraw");
        if (sv.empty())
            sv = bybit::extract_sv_string(obj, "availableToBorrow");
        if (sv.empty())
            sv = bybit::extract_sv_number(obj, "availableToBorrow");
        if (sv.empty())
            sv = bybit::extract_sv_string(obj, "walletBalance");
        if (sv.empty())
            sv = bybit::extract_sv_number(obj, "walletBalance");
        if (sv.empty())
            sv = bybit::extract_sv_string(obj, "equity");
        if (sv.empty())
            sv = bybit::extract_sv_number(obj, "equity");
        if (sv.empty()) return false;
        return bybit::parse_double_sv(sv, out);
    }

    static bool parse_position_row(std::string_view obj, double& out,
                                   std::string_view want_symbol)
    {
        if (!want_symbol.empty())
        {
            auto sym = bybit::extract_sv_string(obj, "symbol");
            if (!sym.empty() && sym != want_symbol)
                return false;
        }

        auto size_sv = bybit::extract_sv_string(obj, "size");
        if (size_sv.empty())
            size_sv = bybit::extract_sv_number(obj, "size");
        if (size_sv.empty())
            size_sv = bybit::extract_sv_string(obj, "positionValue");
        if (size_sv.empty())
            size_sv = bybit::extract_sv_number(obj, "positionValue");

        if (size_sv.empty())
        {
            // Row present but no size fields → treat as flat.
            out = 0.0;
            return true;
        }

        double size = 0.0;
        if (!bybit::parse_double_sv(size_sv, size))
            return false;

        auto side = bybit::extract_sv_string(obj, "side");
        const bool short_side =
            side == "Sell" || side == "sell" || side == "SELL"
            || side == "Short" || side == "short";
        const bool long_side =
            side == "Buy" || side == "buy" || side == "BUY"
            || side == "Long" || side == "long";

        if (short_side)
            out = -std::abs(size);
        else if (long_side)
            out = std::abs(size);
        else
            out = size; // one-way may already be signed; None side + 0 size
        return true;
    }

    static bool within_tolerance(double a, double b, double tolerance_bps)
    {
        const double bound = std::max(std::abs(a), std::abs(b));
        if (bound < 1e-8) return true;
        const double rel = std::abs(a - b) / bound * 10000.0;
        return rel <= tolerance_bps;
    }

    std::shared_ptr<BybitRestClient> rest_;
    get_fn get_;
    std::string symbol_;
    std::string category_ = "linear";
    bool is_demo_ = false;
};

#endif // HAS_BYBIT
