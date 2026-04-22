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

// Live-mode startup gate. Calls GET /api/v3/account, extracts the quote-asset
// free+locked total, and compares it to the local portfolio's cash. Also
// compares any open long position against its corresponding base-asset
// balance on the exchange. Returns "" on pass, a human-readable diff string
// on failure — the engine turns that into a startup exception.
//
// The reconciler does NOT attempt to match per-order state. Its purpose is
// to catch the "we think we hold 2 BTC + 10k USDT, the exchange thinks we
// hold 0" category of startup drift, which is the one that would wipe out
// an account when a strategy ran hot against stale local state.
class BinanceReconciler : public IReconciler
{
public:
    BinanceReconciler(std::shared_ptr<BinanceRestClient> rest,
                      std::string symbol,
                      std::string base_asset,
                      std::string quote_asset)
        : rest_(std::move(rest))
        , symbol_(std::move(symbol))
        , base_asset_(std::move(base_asset))
        , quote_asset_(std::move(quote_asset))
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

    // Scan the account-endpoint response for a specific asset entry and pull
    // out its free/locked amounts. Visible for tests.
    static bool extract_balance(std::string_view json,
                                std::string_view asset,
                                double& free_out,
                                double& locked_out)
    {
        // Balance entries look like: {"asset":"BTC","free":"0.1","locked":"0.0"}
        // We locate the "asset":"X" pair then read the nearest "free"/"locked"
        // after it. The extraction is scoped to the rest of the string; since
        // keys appear in a fixed order in Binance responses, this gives the
        // right entry without a full JSON parse.
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
        // Compare relative drift. Zero vs zero is always OK. Zero vs small
        // (< 1e-8) is also OK — below any meaningful exchange precision.
        const double bound = std::max(std::abs(a), std::abs(b));
        if (bound < 1e-8) return true;
        const double rel = std::abs(a - b) / bound * 10000.0; // bps
        return rel <= tolerance_bps;
    }

    std::shared_ptr<BinanceRestClient> rest_;
    std::string symbol_;
    std::string base_asset_;
    std::string quote_asset_;
};

#endif // HAS_BINANCE
