#pragma once

#include "exits/exit_intent.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace truetest::exits {

// Opaque handle returned by an adapter so the engine can later cancel
// or modify a venue-resting bracket. Plain-old-data — no ownership.
struct bracket_handles
{
    // Per-leg exchange order ids (e.g. Binance orderId for the SL and
    // the TP legs of an OCO). Empty = leg not placed (e.g. intent only
    // had a stop_loss).
    std::optional<std::string> sl_exchange_id;
    std::optional<std::string> tp_exchange_id;

    // Group/list id for venues that bind legs together atomically
    // (Binance OCO listClientOrderId / orderListId).
    std::optional<std::string> oco_list_id;

    // Symbol the brackets were placed against (uppercase). Required by
    // venues that scope cancel by symbol+orderId (Binance futures
    // /fapi/v1/order DELETE) — without this, the adapter would have to
    // stash a single symbol on construction, which doesn't generalize
    // to multi-symbol providers. Empty when handles haven't been
    // populated by place() / list_open() yet.
    std::string symbol;

    bool empty() const
    {
        return !sl_exchange_id && !tp_exchange_id && !oco_list_id;
    }
};

// Capability advertisement. ExitManager queries once and decides how
// to translate intents into venue-side state.
struct bracket_caps
{
    bool stop_market   = false;
    bool stop_limit    = false;
    bool oco           = false;   // SL+TP atomic pair
    bool trailing_stop = false;   // venue-side trailing
};

// Translates ExitManager intents into venue-resting orders. One adapter
// per provider; nullptr provider means engine-side eval is the only path
// (current behavior, used by backtest and shadow).
// Defense-in-depth contract: the in-process armed intent in ExitManager
// stays armed even after place() succeeds. Both paths watching the same
// price stream is intentional — if the venue lags or the WS gaps, the
// engine still fires. The duplicate close arrives at a flat lot and is
// rejected harmlessly by the portfolio.
class IBracketAdapter
{
public:
    virtual ~IBracketAdapter() = default;

    virtual bracket_caps capabilities() const = 0;

    // Called once after the opener fills. Returns handles describing
    // what was placed; empty handles means "could not place — engine
    // remains the sole enforcer" (logged as a warning; not fatal).
    // opener_fill_price is provided so adapters that need a price
    // anchor (rounding to tick size, sanity bounds) have it without
    // re-deriving from the intent. The intent's stop_loss/take_profit
    // are absolute prices, already computed.
    virtual bracket_handles place(std::uint64_t opener_order_id,
                                  const exit_intent& intent,
                                  double opener_fill_price) = 0;

    // Cancel the venue-resting bracket. Idempotent — called on every
    // teardown path (price-trigger fire, signal-close cancel, manual
    // cancel, lot net-flat). Adapters must tolerate "already gone".
    virtual void cancel(std::uint64_t opener_order_id,
                        const bracket_handles& handles) = 0;

    // Optional: trailing-stop adjustment or strategy-driven SL move.
    // Default no-op — adapters whose caps.trailing_stop is false ignore
    // this safely; ExitManager keeps its in-process trail running.
    virtual void modify(std::uint64_t /*opener_order_id*/,
                        const bracket_handles& /*handles*/,
                        std::optional<double> /*new_sl*/,
                        std::optional<double> /*new_tp*/) {}

    // Restart safety: enumerate brackets the venue still has resting
    // from a previous run. Called once at startup AFTER the regular
    // reconciler so the venue's view of orders is trusted. Returns
    // empty if the adapter doesn't model restart recovery (default).
    // strategy_name is best-effort — venues like Binance OCO don't
    // round-trip it through the listClientOrderId, so adapters that
    // can't recover it return "" and the engine carries on without
    // per-strategy attribution for these brackets.
    struct recovered_bracket
    {
        std::uint64_t opener_order_id = 0;
        std::string   strategy_name;
        std::string   symbol;
        order_side    close_side = order_side::sell;
        double        qty = 0.0;
        double        entry_price = 0.0;  // approximated from venue trigger
        std::optional<double> stop_loss;
        std::optional<double> take_profit;
        bracket_handles handles;
    };
    virtual std::vector<recovered_bracket> list_open() { return {}; }
};

// Default adapter for venues without resting-stop support (or for
// backtest/shadow). Reports zero capabilities; place() returns empty
// handles. Acts as a safe sentinel so callers can avoid null checks.
class NullBracketAdapter : public IBracketAdapter
{
public:
    bracket_caps capabilities() const override { return {}; }

    bracket_handles place(std::uint64_t, const exit_intent&, double) override
    {
        return {};
    }

    void cancel(std::uint64_t, const bracket_handles&) override {}

    std::vector<recovered_bracket> list_open() override { return {}; }
};

}
