#pragma once

// ============================================================
// LIVE-SAFETY SURFACE — Phase 1 freeze (see prod.md)
// Any edit requires explicit two-person CCB review + 4 h
// mainnet shadow run on engine_shadow before merge.
// Authoritative path list: scripts/check-live-safety-freeze.sh
// ============================================================

// Live-mode only (gated by config_.mode). Noop defaults are placeholders
// until venue providers (e.g. BinanceReconciler) override — not safe for
// real production.

#include "execution/portfolio.h"

#include <chrono>
#include <memory>
#include <string>

class IReconciler
{
public:
    virtual ~IReconciler() = default;

    // Empty = pass; non-empty error = engine refuses to start.
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

    // Returns false if it bailed past `deadline` — operator must intervene.
    virtual bool cancel_all_and_flatten(std::chrono::milliseconds deadline) = 0;
};

class NoopKillSwitch : public IKillSwitch
{
public:
    bool cancel_all_and_flatten(std::chrono::milliseconds) override { return true; }
};
