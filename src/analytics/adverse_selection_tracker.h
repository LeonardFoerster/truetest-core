#pragma once

#include "../core/event.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>

// markout_bps = (mark - fill) / fill * 1e4 * side_sign. Positive mean =
// favourable (spread capture); negative = adverse selection. Fills queue
// on on_fill; on_mark drains any whose horizon has elapsed. Oldest are
// dropped past max_pending.
class AdverseSelectionTracker
{
public:
    struct config
    {
        std::chrono::milliseconds horizon{10'000};
        std::size_t               max_pending{16'384};
    };

    AdverseSelectionTracker() = default;
    explicit AdverseSelectionTracker(config cfg) : cfg_(cfg) {}

    void on_fill(const fill_event& f);
    void on_mark(const std::string& symbol,
                 double mid,
                 std::chrono::system_clock::time_point ts);

    double      mean_bps()     const { return mean_bps_; }
    double      stdev_bps()    const;
    std::size_t sample_count() const { return sample_n_; }
    std::size_t pending_count() const { return pending_.size(); }
    std::size_t dropped_count() const { return dropped_; }

    void reset()
    {
        pending_.clear();
        sample_n_ = 0;
        mean_bps_ = 0.0;
        m2_bps_   = 0.0;
        dropped_  = 0;
    }

private:
    struct pending_fill
    {
        std::string                           symbol;
        double                                fill_price;
        int                                   side_sign;  // +1/-1
        std::chrono::system_clock::time_point ready_ts;
    };

    std::deque<pending_fill> pending_;
    std::size_t              sample_n_ = 0;
    double                   mean_bps_ = 0.0;
    double                   m2_bps_   = 0.0;
    std::size_t              dropped_  = 0;
    config                   cfg_;
};
