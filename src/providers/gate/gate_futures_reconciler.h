#pragma once
#ifdef HAS_GATE

// Startup gate for Gate.io USDT-M futures. Compares local cash vs venue
// `available` (GET /api/v4/futures/{settle}/accounts) and local signed qty
// vs venue `size` (GET /api/v4/futures/{settle}/positions/{contract}).
//
// Fail-closed: any HTTP / parse error → non-empty error string.
// No testnet soft-pass (same philosophy as Binance/Bitget futures).
// dual_mode is refused at open() via gate_futures_safety.h — not "fixed" here.

#include "execution/live_safety.h"
#include "execution/portfolio.h"
#include "providers/gate/gate_endpoints.h"
#include "providers/gate/gate_parser.h"
#include "providers/gate/gate_rest_client.h"

#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

class GateFuturesReconciler : public IReconciler
{
public:
    using get_fn = std::function<GateRestClient::response(
        const std::string& path, const std::string& query)>;

    GateFuturesReconciler(std::shared_ptr<GateRestClient> rest,
                          std::string symbol,
                          gate::endpoints ep = gate::usdt_mainnet())
        : rest_(std::move(rest))
        , symbol_(std::move(symbol))
        , endpoints_(std::move(ep))
    {
        if (rest_)
        {
            get_ = [r = rest_](const std::string& path, const std::string& q) {
                return r->get(path, q);
            };
        }
    }

    // Test seam: inject canned HTTP without a live REST client.
    GateFuturesReconciler(get_fn get,
                          std::string symbol,
                          gate::endpoints ep = gate::usdt_mainnet())
        : get_(std::move(get))
        , symbol_(std::move(symbol))
        , endpoints_(std::move(ep))
    {}

    std::string reconcile(const portfolio& local, double tolerance_bps) override
    {
        if (!get_)
            return "GateFuturesReconciler: no REST client";

        // Positions first (matches Binance/Bitget order: position then cash).
        const std::string pos_path =
            gate::futures_path(endpoints_,
                               std::string("/positions/") + symbol_);
        auto pos_resp = get_(pos_path, "");
        if (auto err = http_error("positions", pos_resp,
                                  /*allow_not_found=*/true))
            return *err;

        const std::string acct_path =
            gate::futures_path(endpoints_, "/accounts");
        auto acct_resp = get_(acct_path, "");
        if (auto err = http_error("accounts", acct_resp,
                                  /*allow_not_found=*/false))
            return *err;

        double ex_available = 0.0;
        if (!extract_available(acct_resp.body, ex_available))
            return "GateFuturesReconciler: available not found in "
                   "accounts response";

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
        // POSITION_NOT_FOUND (or empty body after allow_not_found) → flat 0.
        if (is_position_not_found(pos_resp))
        {
            ex_position_amt = 0.0;
        }
        else if (!extract_position_size(pos_resp.body, ex_position_amt,
                                        symbol_))
        {
            return "GateFuturesReconciler: position size not found in "
                   "positions response";
        }

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

    // Top-level `available` on GET .../accounts (string or number).
    // Prefer first match; Gate does not nest USDT under an assets array
    // for USDT-settle futures accounts.
    static bool extract_available(std::string_view json, double& out)
    {
        auto sv = gate::extract_sv_string(json, "available");
        if (sv.empty())
            sv = gate::extract_sv_number(json, "available");
        if (sv.empty()) return false;
        return gate::parse_double_sv(sv, out);
    }

    // Signed size from a single-position object, a positions array, or an
    // empty/flat envelope. Empty list / empty body → 0.0. Non-empty without
    // a matching contract when want_symbol is set → refuse (fail-closed).
    static bool extract_position_size(std::string_view json, double& out,
                                      std::string_view want_symbol = {})
    {
        out = 0.0;
        if (json.empty())
            return true; // flat

        // Error envelope without a position object.
        auto label = gate::extract_sv_string(json, "label");
        if (!label.empty()
            && gate::extract_sv_string(json, "contract").empty()
            && gate::extract_sv_string(json, "size").empty()
            && gate::extract_sv_number(json, "size").empty())
        {
            if (label == "POSITION_NOT_FOUND"
                || label == "POSITION_EMPTY")
            {
                out = 0.0;
                return true;
            }
            return false;
        }

        // Prefer top-level array body (list endpoint).
        if (!json.empty() && json.front() == '[')
        {
            return extract_from_array(json, out, want_symbol);
        }

        // Named array under common keys (defensive).
        auto arr = gate::json_util::extract_array(json, "positions");
        if (arr.empty())
            arr = gate::json_util::extract_array(json, "data");
        if (!arr.empty())
            return extract_from_array(arr, out, want_symbol);

        // Single object (GET .../positions/{contract}).
        return parse_position_row(json, out, want_symbol);
    }

    bool is_testnet() const { return endpoints_.is_testnet; }
    const std::string& symbol() const { return symbol_; }

private:
    static bool is_position_not_found(const GateRestClient::response& resp)
    {
        if (gate::is_http_success(resp.status))
            return false;
        // Gate returns 404 + label POSITION_NOT_FOUND when flat.
        auto label = gate::extract_sv_string(resp.body, "label");
        return label == "POSITION_NOT_FOUND" || label == "POSITION_EMPTY";
    }

    static std::optional<std::string> http_error(
        const char* label,
        const GateRestClient::response& resp,
        bool allow_not_found)
    {
        if (gate::is_http_success(resp.status))
            return std::nullopt;

        if (allow_not_found && is_position_not_found(resp))
            return std::nullopt;

        const std::string body_s = gate::redact_for_log(resp.body, 160);
        auto err_label = gate::extract_error_label(resp.body);
        char buf[360];
        if (!err_label.empty())
        {
            std::snprintf(buf, sizeof(buf),
                "GateFuturesReconciler: %s failed (HTTP %d label=%.*s): %.160s",
                label, resp.status,
                static_cast<int>(err_label.size()), err_label.data(),
                body_s.c_str());
        }
        else
        {
            std::snprintf(buf, sizeof(buf),
                "GateFuturesReconciler: %s failed (HTTP %d): %.160s",
                label, resp.status, body_s.c_str());
        }
        return std::string(buf);
    }

    static bool extract_from_array(std::string_view arr, double& out,
                                   std::string_view want_symbol)
    {
        out = 0.0;
        bool matched = false;
        bool parse_ok = true;
        int row_count = 0;
        gate::json_util::for_each_array_object(
            arr, [&](std::string_view obj) {
                ++row_count;
                if (matched || !parse_ok) return;
                auto sym = gate::extract_sv_string(obj, "contract");
                if (sym.empty())
                    sym = gate::extract_sv_string(obj, "symbol");
                if (!want_symbol.empty() && !sym.empty()
                    && gate::normalize_contract_symbol(sym)
                           != gate::normalize_contract_symbol(want_symbol))
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
        // Empty list → flat 0. Non-empty without want_symbol match → refuse.
        if (!matched && row_count > 0 && !want_symbol.empty())
            return false;
        return true;
    }

    static bool parse_position_row(std::string_view obj, double& out,
                                   std::string_view want_symbol)
    {
        if (!want_symbol.empty())
        {
            auto sym = gate::extract_sv_string(obj, "contract");
            if (sym.empty())
                sym = gate::extract_sv_string(obj, "symbol");
            if (!sym.empty()
                && gate::normalize_contract_symbol(sym)
                       != gate::normalize_contract_symbol(want_symbol))
                return false;
        }

        // Gate size is signed: >0 long/buy, <0 short/sell (G3).
        auto size_sv = gate::extract_sv_string(obj, "size");
        if (size_sv.empty())
            size_sv = gate::extract_sv_number(obj, "size");
        if (size_sv.empty())
        {
            // Row present but no size → treat as flat only if contract keys
            // look empty (defensive). Missing size on a real row → refuse.
            auto contract = gate::extract_sv_string(obj, "contract");
            if (contract.empty())
                contract = gate::extract_sv_string(obj, "symbol");
            if (contract.empty())
            {
                out = 0.0;
                return true;
            }
            return false;
        }

        double size = 0.0;
        if (!gate::parse_double_sv(size_sv, size))
            return false;
        out = size; // already signed in one-way mode
        return true;
    }

    static bool within_tolerance(double a, double b, double tolerance_bps)
    {
        const double bound = std::max(std::abs(a), std::abs(b));
        if (bound < 1e-8) return true;
        const double rel = std::abs(a - b) / bound * 10000.0;
        return rel <= tolerance_bps;
    }

    std::shared_ptr<GateRestClient> rest_;
    get_fn get_;
    std::string symbol_;
    gate::endpoints endpoints_;
};

#endif // HAS_GATE
