#pragma once

#include "../core/event.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Queue-position estimate for shadow-mode passive limits.
// TradeTapeShadowAdapter without this model fills any limit the moment a
// real trade crosses it - optimistic for makers, because real life would
// have served the volume sitting ahead of you first. The model snapshots
// the depth at our limit price at submit time, and the adapter only
// emits a fill once that queue has been consumed by subsequent prints.
class IQueuePositionModel
{
public:
    virtual ~IQueuePositionModel() = default;

    virtual void on_snapshot(
        const std::string& symbol,
        const std::vector<std::pair<double, double>>& bids,
        const std::vector<std::pair<double, double>>& asks) = 0;

    virtual void on_update(
        const std::string& symbol, order_side side,
        double price, double new_size) = 0;

    // Event-time variants preserve replay determinism. Implementations that
    // do not need timestamps remain source-compatible through these defaults.
    virtual void on_snapshot_at(
        const std::string& symbol,
        const std::vector<std::pair<double, double>>& bids,
        const std::vector<std::pair<double, double>>& asks,
        std::chrono::system_clock::time_point /*event_ts*/)
    {
        on_snapshot(symbol, bids, asks);
    }

    virtual void on_update_at(
        const std::string& symbol, order_side side,
        double price, double new_size,
        std::chrono::system_clock::time_point /*event_ts*/)
    {
        on_update(symbol, side, price, new_size);
    }

    // queue_ahead at our limit price, evaluated at submit_ts. Returns 0
    // when we improve the BBO or when no level matches our price. A stale or
    // absent L2 observation is represented by +inf, which blocks fills.
    virtual double queue_ahead(
        const std::string& symbol, order_side side,
        double limit_price,
        std::chrono::system_clock::time_point ts) const = 0;
};

// Default. Always returns 0 -> adapter falls through to legacy fill-on-
// cross. Used when --queue-model none (the default) is set, or when no
// L2 stream is available.
class NoQueueModel final : public IQueuePositionModel
{
public:
    void on_snapshot(const std::string&,
                     const std::vector<std::pair<double, double>>&,
                     const std::vector<std::pair<double, double>>&) override {}
    void on_update(const std::string&, order_side, double, double) override {}
    double queue_ahead(const std::string&, order_side, double,
                       std::chrono::system_clock::time_point) const override
    { return 0.0; }
};

// L2 snapshot model. Maintains the most recent bid/ask ladder per
// symbol from `on_snapshot` + `on_update`. queue_ahead(side, P) returns
// the size resting at level P on the corresponding side.
// Refuses (returns +inf) when the snapshot is older than max_staleness_;
// 100ms diff streams plus WS jitter put fresh snapshots typically under
// 200ms, so 1s is a wide tolerance that still catches stalls.
class L2SnapshotQueueModel final : public IQueuePositionModel
{
public:
    explicit L2SnapshotQueueModel(
        std::chrono::milliseconds max_staleness = std::chrono::seconds(1))
        : max_staleness_(max_staleness) {}

    void on_snapshot(
        const std::string& symbol,
        const std::vector<std::pair<double, double>>& bids,
        const std::vector<std::pair<double, double>>& asks) override
    {
        on_snapshot_at(symbol, bids, asks,
                       std::chrono::system_clock::time_point{});
    }

    void on_snapshot_at(
        const std::string& symbol,
        const std::vector<std::pair<double, double>>& bids,
        const std::vector<std::pair<double, double>>& asks,
        std::chrono::system_clock::time_point event_ts) override
    {
        auto& book = books_[symbol];
        book.bids.clear();
        book.asks.clear();
        for (const auto& [p, q] : bids)
            if (q > 0.0) book.bids[p] = q;
        for (const auto& [p, q] : asks)
            if (q > 0.0) book.asks[p] = q;
        book.last_update = event_ts;
        book.has_event_time = true;
    }

    void on_update(const std::string& symbol, order_side side,
                   double price, double new_size) override
    {
        on_update_at(symbol, side, price, new_size,
                     std::chrono::system_clock::time_point{});
    }

    void on_update_at(const std::string& symbol, order_side side,
                      double price, double new_size,
                      std::chrono::system_clock::time_point event_ts) override
    {
        auto& book = books_[symbol];
        auto& levels = (side == order_side::buy) ? book.bids : book.asks;
        if (new_size > 0.0)
            levels[price] = new_size;
        else
            levels.erase(price);
        book.last_update = event_ts;
        book.has_event_time = true;
    }

    double queue_ahead(
        const std::string& symbol, order_side side,
        double limit_price,
        std::chrono::system_clock::time_point ts) const override
    {
        auto it = books_.find(symbol);
        if (it == books_.end())
            return std::numeric_limits<double>::infinity();

        const auto& book = it->second;
        if (!book.has_event_time || ts < book.last_update
            || ts - book.last_update > max_staleness_)
            return std::numeric_limits<double>::infinity();

        const auto& levels = (side == order_side::buy) ? book.bids : book.asks;
        auto level_it = levels.find(limit_price);
        // No level at our price -> we're alone at this rung (either
        // improving the BBO or sitting deeper than any existing rest).
        // Either way queue_ahead = 0 and the legacy fill-on-cross path
        // takes over.
        if (level_it == levels.end())
            return 0.0;
        return level_it->second;
    }

private:
    struct book_t
    {
        // bids: sorted ascending; rbegin() is best bid.
        // asks: sorted ascending; begin()  is best ask.
        std::map<double, double> bids;
        std::map<double, double> asks;
        std::chrono::system_clock::time_point last_update{};
        bool has_event_time{false};
    };

    std::unordered_map<std::string, book_t> books_;
    std::chrono::milliseconds max_staleness_;
};
