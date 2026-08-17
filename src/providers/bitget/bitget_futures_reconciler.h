#pragma once
#ifdef HAS_BITGET

// Startup gate for Bitget UTA USDT-M futures. Compares local cash vs venue
// available USDT (`GET /api/v3/account/assets`) and local signed qty vs venue
// position (`GET /api/v3/position/current-position`).
//
// Fail-closed: any HTTP / business-code / parse error → non-empty error string.
// No demo soft-pass (same philosophy as Binance futures).

#include "execution/live_safety.h"
#include "execution/portfolio.h"
#include "providers/bitget/bitget_parser.h"
#include "providers/bitget/bitget_rest_client.h"
#include "providers/recovery_payload.h"

#include <cmath>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

class BitgetFuturesReconciler : public IReconciler
{
public:
    using get_fn = std::function<BitgetRestClient::response(
        const std::string& endpoint, const std::string& query)>;

    BitgetFuturesReconciler(std::shared_ptr<BitgetRestClient> rest,
                            std::string symbol,
                            std::string category = "USDT-FUTURES",
                            bool is_demo = false)
        : rest_(std::move(rest))
        , symbol_(std::move(symbol))
        , category_(std::move(category))
        , is_demo_(is_demo)
    {
    }

    // Test seam: inject canned HTTP without a live REST client.
    BitgetFuturesReconciler(get_fn get,
                            std::string symbol,
                            std::string category = "USDT-FUTURES",
                            bool is_demo = false)
        : get_(std::move(get))
        , symbol_(std::move(symbol))
        , category_(std::move(category))
        , is_demo_(is_demo)
    {}

    std::string reconcile(const portfolio& local, double tolerance_bps) override
    {
        if (!rest_ && !get_)
            return "BitgetFuturesReconciler: no REST client";

        const auto expires_at = std::chrono::steady_clock::now()
                              + std::chrono::seconds(5);
        const auto request = [&](const std::string& endpoint,
                                 const std::string& query) {
            if (rest_)
            {
                const auto now = std::chrono::steady_clock::now();
                const auto remaining = now >= expires_at
                    ? std::chrono::milliseconds{0}
                    : std::chrono::duration_cast<std::chrono::milliseconds>(
                          expires_at - now);
                return rest_->safety_get(endpoint, query, remaining);
            }
            return get_(endpoint, query);
        };

        // Positions first (matches Binance futures order: position then cash).
        const std::string pos_q =
            "category=" + category_ + "&symbol=" + symbol_;
        auto pos_resp = request("/api/v3/position/current-position", pos_q);
        if (auto err = http_business_error(
                "current-position", pos_resp))
            return *err;

        auto acct_resp = request("/api/v3/account/assets", "");
        if (auto err = http_business_error("account/assets", acct_resp))
            return *err;

        double ex_available = 0.0;
        if (!extract_available_usdt(acct_resp.body, ex_available))
            return "BitgetFuturesReconciler: available USDT not found in "
                   "/api/v3/account/assets response";

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
            return "BitgetFuturesReconciler: position size not found in "
                   "/api/v3/position/current-position response";

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

    // Walk data.assets[] for coin==USDT; prefer `available`, then
    // `availableEquity`. First USDT match wins.
    static bool extract_available_usdt(std::string_view json, double& out)
    {
        std::string_view data;
        std::string_view arr;
        if (!provider_recovery::top_level_member(json, "data", data)
            || !provider_recovery::is_authoritative_object(data)
            || !provider_recovery::top_level_member(data, "assets", arr)
            || !provider_recovery::is_authoritative_object_array(arr))
            return false;

        bool found = false;
        const bool schema_ok = provider_recovery::every_top_level_object(
            arr, [&](std::string_view obj) {
            std::string_view coin;
            double parsed = 0.0;
            if (!provider_recovery::top_level_plain_string(
                    obj, "coin", coin)
                || !parse_available_field(obj, parsed))
                return false;
            if (coin != "USDT" && coin != "usdt") return true;
            if (found) return false;
            out = parsed;
            found = true;
            return true;
        });
        return schema_ok && found;
    }

    // Signed qty from current-position body. Empty list → 0.0 (flat is valid
    // when the venue filtered by symbol). Uses size/total + posSide
    // (long>0, short<0). Returns false on unparseable size fields OR when
    // want_symbol is set and a non-empty row list never matches that symbol
    // (fail-closed — do not treat "other symbols only" as flat).
    static bool extract_position_amt(std::string_view json, double& out,
                                     std::string_view want_symbol = {})
    {
        out = 0.0;
        std::string_view data;
        if (!provider_recovery::top_level_member(json, "data", data)
            || !provider_recovery::is_authoritative_object(data))
            return false;

        std::string_view arr;
        const auto list_state = provider_recovery::payload_parser(data)
            .inspect_top_level_member("list", arr);
        if (list_state
            == provider_recovery::payload_parser::member_result::missing)
            return parse_position_row(data, out, want_symbol);
        if (list_state
            != provider_recovery::payload_parser::member_result::unique)
            return false;
        if (!provider_recovery::is_authoritative_object_array(arr))
            return false;

        bool matched = false;
        bool parse_ok = true;
        int row_count = 0;
        const bool schema_ok = provider_recovery::every_top_level_object(
            arr, [&](std::string_view obj) {
            ++row_count;
            if (matched || !parse_ok) return false;
            double qty = 0.0;
            if (!parse_position_row(obj, qty, want_symbol))
            {
                parse_ok = false;
                return false;
            }
            out = qty;
            matched = true;
            return true;
        });

        if (!schema_ok || !parse_ok) return false;
        // Empty list → flat 0. Non-empty without a match for want_symbol → refuse
        // (symbol form mismatch or unexpected multi-symbol payload).
        return row_count == 0 || (row_count == 1 && matched);
    }

    bool is_demo() const { return is_demo_; }

private:
    static std::optional<std::string> http_business_error(
        const char* label, const BitgetRestClient::response& resp)
    {
        if (resp.status < 200 || resp.status >= 300)
        {
            const std::string body_s = bitget::truncate_for_log(resp.body, 160);
            char buf[320];
            std::snprintf(buf, sizeof(buf),
                "BitgetFuturesReconciler: %s failed (HTTP %d): %.160s",
                label, resp.status, body_s.c_str());
            return std::string(buf);
        }
        // Prefer explicit business_ok when set by RestClient; also re-check
        // envelope so injected test responses without business_ok still gate.
        if (!resp.business_ok && !bitget::is_business_success(resp.status, resp.body))
        {
            auto code = bitget::extract_business_code(resp.body);
            const std::string code_s =
                code.empty() ? std::string("<missing>") : std::string(code);
            const std::string body_s = bitget::truncate_for_log(resp.body, 160);
            char buf[320];
            std::snprintf(buf, sizeof(buf),
                "BitgetFuturesReconciler: %s business code %s: %.160s",
                label, code_s.c_str(), body_s.c_str());
            return std::string(buf);
        }
        return std::nullopt;
    }

    static bool parse_available_field(std::string_view obj, double& out)
    {
        std::string_view raw;
        auto state = provider_recovery::payload_parser(obj)
            .inspect_top_level_member("available", raw);
        if (state == provider_recovery::payload_parser::member_result::invalid_or_duplicate)
            return false;
        if (state == provider_recovery::payload_parser::member_result::missing)
        {
            state = provider_recovery::payload_parser(obj)
                .inspect_top_level_member("availableEquity", raw);
            if (state != provider_recovery::payload_parser::member_result::unique)
                return false;
            return provider_recovery::top_level_scalar_text(
                       obj, "availableEquity", raw)
                && bitget::parse_double_sv(raw, out);
        }
        return provider_recovery::top_level_scalar_text(
                   obj, "available", raw)
            && bitget::parse_double_sv(raw, out);
    }

    static bool parse_position_row(std::string_view obj, double& out,
                                   std::string_view want_symbol)
    {
        std::string_view sym;
        if (!provider_recovery::top_level_plain_string(obj, "symbol", sym)
            || (!want_symbol.empty() && sym != want_symbol))
            return false;

        auto optional_scalar = [&](std::string_view key,
                                   std::string_view& value) -> int {
            const auto state = provider_recovery::payload_parser(obj)
                .inspect_top_level_member(key, value);
            if (state == provider_recovery::payload_parser::member_result::missing)
                return 0;
            if (state != provider_recovery::payload_parser::member_result::unique
                || !provider_recovery::top_level_scalar_text(obj, key, value))
                return -1;
            return 1;
        };

        std::string_view size_sv;
        int size_state = optional_scalar("total", size_sv);
        if (size_state == 0) size_state = optional_scalar("size", size_sv);
        if (size_state == 0) size_state = optional_scalar("available", size_sv);
        if (size_state != 1) return false;

        double size = 0.0;
        if (!bitget::parse_double_sv(size_sv, size))
            return false;

        std::string_view pos_side;
        auto optional_string = [&](std::string_view key,
                                   std::string_view& value) -> int {
            const auto state = provider_recovery::payload_parser(obj)
                .inspect_top_level_member(key, value);
            if (state == provider_recovery::payload_parser::member_result::missing)
                return 0;
            if (state != provider_recovery::payload_parser::member_result::unique
                || !provider_recovery::top_level_plain_string(obj, key, value))
                return -1;
            return 1;
        };
        int side_state = optional_string("posSide", pos_side);
        if (side_state == 0) side_state = optional_string("holdSide", pos_side);
        if (side_state < 0) return false;

        const bool short_side =
            pos_side == "short" || pos_side == "SHORT"
            || pos_side == "sell" || pos_side == "SELL";
        const bool long_side =
            pos_side == "long" || pos_side == "LONG"
            || pos_side == "buy" || pos_side == "BUY";

        if (short_side)
            out = -std::abs(size);
        else if (long_side)
            out = std::abs(size);
        else
            out = size; // one-way may already be signed
        return true;
    }

    static bool within_tolerance(double a, double b, double tolerance_bps)
    {
        const double bound = std::max(std::abs(a), std::abs(b));
        if (bound < 1e-8) return true;
        const double rel = std::abs(a - b) / bound * 10000.0;
        return rel <= tolerance_bps;
    }

    std::shared_ptr<BitgetRestClient> rest_;
    get_fn get_;
    std::string symbol_;
    std::string category_ = "USDT-FUTURES";
    bool is_demo_ = false;
};

#endif // HAS_BITGET
