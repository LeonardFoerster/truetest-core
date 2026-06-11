#pragma once
#ifdef HAS_BINANCE

#include "execution/live_safety.h"
#include "execution/portfolio.h"
#include "providers/binance/binance_parser.h"
#include "providers/binance/binance_rest_client.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

// Startup gate: compares local cash/position against GET /api/v3/account.
// Catches the "we think we hold X, exchange thinks 0" drift that would
// wipe an account under stale local state. Not per-order.
class BinanceReconciler : public IReconciler
{
public:
    BinanceReconciler(std::shared_ptr<BinanceRestClient> rest,
                      std::string symbol,
                      std::string base_asset,
                      std::string quote_asset,
                      bool is_testnet = false)
        : rest_(std::move(rest))
        , symbol_(std::move(symbol))
        , base_asset_(std::move(base_asset))
        , quote_asset_(std::move(quote_asset))
        , is_testnet_(is_testnet)
    {}

    std::string reconcile(const portfolio& local, double tolerance_bps) override
    {
        if (!rest_) return "BinanceReconciler: no REST client";

        auto resp = rest_->get("/api/v3/account", "");
        if (resp.status < 200 || resp.status >= 300)
        {
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                "BinanceReconciler: /api/v3/account failed (HTTP %d): %.160s",
                resp.status, resp.body.c_str());
            return buf;
        }

        double ex_quote_free = 0.0, ex_quote_locked = 0.0;
        if (!extract_balance(resp.body, quote_asset_, ex_quote_free, ex_quote_locked))
        {
            return "BinanceReconciler: quote asset '" + quote_asset_
                   + "' not found in /api/v3/account response";
        }

        const double ex_quote_total = ex_quote_free + ex_quote_locked;
        const double local_cash = local.get_cash();

        if (!within_tolerance(local_cash, ex_quote_total, tolerance_bps))
        {
            // Testnet wipes balances ~monthly. Treat "venue ~zero, local non-zero"
            // as the reset signature and downgrade to a warning so startup can
            // proceed; subsequent fills will re-anchor local state.
            if (is_testnet_ && looks_like_reset(ex_quote_total, local_cash))
            {
                std::fprintf(stderr,
                    "  [TESTNET-RESET] venue cash=%.8f %s, local=%.8f %s - "
                    "treating as account reset, drift check skipped.\n",
                    ex_quote_total, quote_asset_.c_str(),
                    local_cash, quote_asset_.c_str());
                return {};
            }
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                "cash drift: local=%.8f %s exchange=%.8f %s (> %.2f bps)",
                local_cash, quote_asset_.c_str(),
                ex_quote_total, quote_asset_.c_str(),
                tolerance_bps);
            return buf;
        }

        const auto& positions = local.get_positions();
        auto it = positions.find(symbol_);
        if (it != positions.end() && std::abs(it->second.qty) > 1e-12)
        {
            double ex_base_free = 0.0, ex_base_locked = 0.0;
            if (!extract_balance(resp.body, base_asset_, ex_base_free, ex_base_locked))
            {
                return "BinanceReconciler: base asset '" + base_asset_
                       + "' not found in /api/v3/account response but local "
                         "position is non-zero";
            }
            const double ex_base_total = ex_base_free + ex_base_locked;
            if (!within_tolerance(it->second.qty, ex_base_total, tolerance_bps))
            {
                if (is_testnet_ && looks_like_reset(ex_base_total, it->second.qty))
                {
                    std::fprintf(stderr,
                        "  [TESTNET-RESET] venue %s=%.8f, local=%.8f - "
                        "treating as account reset, position drift skipped.\n",
                        base_asset_.c_str(), ex_base_total, it->second.qty);
                    return {};
                }
                char buf[256];
                std::snprintf(buf, sizeof(buf),
                    "position drift: local=%.8f %s exchange=%.8f %s (> %.2f bps)",
                    it->second.qty, base_asset_.c_str(),
                    ex_base_total, base_asset_.c_str(),
                    tolerance_bps);
                return buf;
            }
        }

        return {};
    }

    // Binance fixed key order: find "asset":"X", then read the nearest
    // "free"/"locked" after it. Avoids a full JSON parse.
    static bool extract_balance(std::string_view json,
                                std::string_view asset,
                                double& free_out,
                                double& locked_out)
    {
        std::string needle;
        needle.reserve(asset.size() + 12);
        needle += "\"asset\":\"";
        needle += asset;
        needle += "\"";

        auto hit = json.find(needle);
        if (hit == std::string_view::npos) return false;

        auto rest = json.substr(hit);
        auto free_sv   = binance::extract_sv_string(rest, "free");
        auto locked_sv = binance::extract_sv_string(rest, "locked");

        if (!binance::parse_double_sv(free_sv, free_out))   return false;
        if (!binance::parse_double_sv(locked_sv, locked_out)) return false;
        return true;
    }

private:
    static bool within_tolerance(double a, double b, double tolerance_bps)
    {
        // 0 vs 0 OK. 0 vs <1e-8 OK (below exchange precision).
        const double bound = std::max(std::abs(a), std::abs(b));
        if (bound < 1e-8) return true;
        const double rel = std::abs(a - b) / bound * 10000.0;
        return rel <= tolerance_bps;
    }

    // Testnet reset signature: venue near-zero while local has real magnitude.
    // The 1e-6 floor catches dust that survives some resets.
    static bool looks_like_reset(double venue, double local)
    {
        return std::abs(venue) < 1e-6 && std::abs(local) > 1e-6;
    }

    std::shared_ptr<BinanceRestClient> rest_;
    std::string symbol_;
    std::string base_asset_;
    std::string quote_asset_;
    bool is_testnet_ = false;
};

#endif // HAS_BINANCE
