#include "analytics/adverse_selection_tracker.h"

#include <cmath>

void AdverseSelectionTracker::on_fill(const fill_event& f)
{
    if (cfg_.max_pending > 0 && pending_.size() >= cfg_.max_pending)
    {
        // Bounded memory over perfect attribution when on_mark is behind.
        pending_.pop_front();
        ++dropped_;
    }

    const int side_sign = (f.get_side() == order_side::buy) ? +1 : -1;
    pending_.push_back({
        f.get_symbol(),
        f.get_fill_price(),
        side_sign,
        f.get_timestamp() + cfg_.horizon,
    });
}

void AdverseSelectionTracker::on_mark(
    const std::string& symbol,
    double mid,
    std::chrono::system_clock::time_point ts)
{
    if (!(mid > 0.0)) return;

    // Can't just drain front: symbol-B fills must stay queued until a
    // B-mark arrives even while A-fills elapse. Bounded queue keeps O(N).
    std::deque<pending_fill> survivors;
    for (auto& pf : pending_)
    {
        if (pf.ready_ts > ts)
        {
            survivors.push_back(std::move(pf));
            continue;
        }
        if (pf.symbol != symbol)
        {
            survivors.push_back(std::move(pf));
            continue;
        }

        const double markout_bps =
            (mid - pf.fill_price) / pf.fill_price * 1.0e4 * pf.side_sign;

        ++sample_n_;
        const double delta  = markout_bps - mean_bps_;
        mean_bps_          += delta / static_cast<double>(sample_n_);
        const double delta2 = markout_bps - mean_bps_;
        m2_bps_            += delta * delta2;
    }
    pending_ = std::move(survivors);
}

double AdverseSelectionTracker::stdev_bps() const
{
    if (sample_n_ < 2) return 0.0;
    return std::sqrt(m2_bps_ / static_cast<double>(sample_n_ - 1));
}
