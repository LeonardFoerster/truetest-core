#pragma once

// Interfaces the engine consults in live mode BEFORE submitting any order and
// AFTER the run ends. Backtest and shadow modes skip these paths entirely —
// the engine gates on `config_.mode == engine_mode::live`.
//
// Implementations live with the venue provider (BinanceReconciler, etc.).
// The no-op defaults here let the engine run even when the venue hasn't
// wired them up yet: reconciliation passes trivially, kill-switch is a nop.
// That's a hand-wave for now — once the Binance impls land, the defaults are
// obviously unsafe in production and the provider should always override.

#include "execution/portfolio.h"

#include <chrono>
#include <memory>
#include <string>

class IReconciler
{
public:
    virtual ~IReconciler() = default;

    // Compare local state against what the exchange reports. Returns an empty
    // string on pass, a human-readable error on mismatch. The engine refuses
    // to start when a non-empty error comes back.
    //
    // `tolerance_bps` is the acceptable discrepancy between local and
    // exchange-reported balance / position (expressed in basis points).
    virtual std::string reconcile(const portfolio& local_view,
                                  double tolerance_bps) = 0;
};

class NoopReconciler : public IReconciler
{
public:
    std::string reconcile(const portfolio&, double) override { return {}; }
};

class IKillSwitch
{
public:
    virtual ~IKillSwitch() = default;

    // Cancel every open order, flatten every open position, then return.
    // Returns true if completed within `deadline`, false if it had to bail
    // (so the operator knows to manually intervene).
    virtual bool cancel_all_and_flatten(std::chrono::milliseconds deadline) = 0;
};

class NoopKillSwitch : public IKillSwitch
{
public:
    bool cancel_all_and_flatten(std::chrono::milliseconds) override { return true; }
};
