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

#include <cmath>
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
        if (rest_)
        {
            get_ = [r = rest_](const std::string& ep, const std::string& q) {
                return r->get(ep, q);
            };
        }
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
        if (!get_)
            return "BitgetFuturesReconciler: no REST client";

        // Positions first (matches Binance futures order: position then cash).
        const std::string pos_q =
            "category=" + category_ + "&symbol=" + symbol_;
        auto pos_resp = get_("/api/v3/position/current-position", pos_q);
        if (auto err = http_business_error(
                "current-position", pos_resp))
            return *err;

        auto acct_resp = get_("/api/v3/account/assets", "");
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
        auto arr = bitget::detail::extract_array(json, "assets");
        if (arr.empty())
        {
            // Some envelopes nest under data already walked by needle scan;
            // also accept a top-level single-object with coin=USDT.
            auto coin = bitget::extract_sv_string(json, "coin");
            if (coin == "USDT" || coin == "usdt")
                return parse_available_field(json, out);
            return false;
        }

        bool found = false;
        bitget::detail::for_each_array_object(arr, [&](std::string_view obj) {
            if (found) return;
            auto coin = bitget::extract_sv_string(obj, "coin");
            if (coin != "USDT" && coin != "usdt") return;
            if (parse_available_field(obj, out))
                found = true;
        });
        return found;
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

        // Prefer data.list[]; fall back to data[] array of rows.
        auto arr = bitget::detail::extract_array(json, "list");
        if (arr.empty())
            arr = bitget::detail::extract_array(json, "data");

        if (arr.empty())
        {
            // Single object under data, or empty envelope (flat).
            auto data_obj = bitget::detail::extract_object(json, "data");
            if (data_obj.empty())
                return true; // empty / flat
            // data may be {} with no list — flat.
            if (bitget::extract_sv_string(data_obj, "symbol").empty()
                && bitget::extract_sv_string(data_obj, "size").empty()
                && bitget::extract_sv_string(data_obj, "total").empty()
                && bitget::extract_sv_number(data_obj, "size").empty()
                && bitget::extract_sv_number(data_obj, "total").empty())
                return true;
            // Wrong-symbol single object: parse_position_row returns false.
            return parse_position_row(data_obj, out, want_symbol);
        }

        bool matched = false;
        bool parse_ok = true;
        int row_count = 0;
        bitget::detail::for_each_array_object(arr, [&](std::string_view obj) {
            ++row_count;
            if (matched || !parse_ok) return;
            auto sym = bitget::extract_sv_string(obj, "symbol");
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
        // Empty list → flat 0. Non-empty without a match for want_symbol → refuse
        // (symbol form mismatch or unexpected multi-symbol payload).
        if (!matched && row_count > 0 && !want_symbol.empty())
            return false;
        return true;
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
        auto sv = bitget::extract_sv_string(obj, "available");
        if (sv.empty())
            sv = bitget::extract_sv_number(obj, "available");
        if (sv.empty())
            sv = bitget::extract_sv_string(obj, "availableEquity");
        if (sv.empty())
            sv = bitget::extract_sv_number(obj, "availableEquity");
        if (sv.empty()) return false;
        return bitget::parse_double_sv(sv, out);
    }

    static bool parse_position_row(std::string_view obj, double& out,
                                   std::string_view want_symbol)
    {
        if (!want_symbol.empty())
        {
            auto sym = bitget::extract_sv_string(obj, "symbol");
            if (!sym.empty() && sym != want_symbol)
                return false;
        }

        auto size_sv = bitget::extract_sv_string(obj, "total");
        if (size_sv.empty())
            size_sv = bitget::extract_sv_number(obj, "total");
        if (size_sv.empty())
            size_sv = bitget::extract_sv_string(obj, "size");
        if (size_sv.empty())
            size_sv = bitget::extract_sv_number(obj, "size");
        // available (position size free to reduce) as last resort
        if (size_sv.empty())
            size_sv = bitget::extract_sv_string(obj, "available");
        if (size_sv.empty())
            size_sv = bitget::extract_sv_number(obj, "available");

        if (size_sv.empty())
        {
            // Row present but no size fields → treat as flat.
            out = 0.0;
            return true;
        }

        double size = 0.0;
        if (!bitget::parse_double_sv(size_sv, size))
            return false;

        auto pos_side = bitget::extract_sv_string(obj, "posSide");
        if (pos_side.empty())
            pos_side = bitget::extract_sv_string(obj, "holdSide");

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
