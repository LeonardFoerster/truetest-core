#include "ui/snapshot_metrics.h"

#include <algorithm>
#include <cmath>

namespace truetest::ui {
namespace {

snapshot_risk_level level_for_fraction(double fraction) noexcept
{
    if (fraction >= 0.9) return snapshot_risk_level::critical;
    if (fraction >= 0.7) return snapshot_risk_level::danger;
    if (fraction >= 0.5) return snapshot_risk_level::warning;
    if (fraction >= 0.3) return snapshot_risk_level::caution;
    return snapshot_risk_level::safe;
}

}  // namespace

std::optional<double> available_metric(bool available, double value) noexcept
{
    return available && std::isfinite(value)
        ? std::optional<double>{value}
        : std::nullopt;
}

std::optional<double> position_notional(
    const dashboard_snapshot::position_row& position) noexcept
{
    const auto mark = available_metric(position.mark_available, position.mark);
    if (!mark || !std::isfinite(position.qty)) return std::nullopt;
    return std::abs(position.qty) * *mark;
}

std::optional<double> position_unrealized_pct(
    const dashboard_snapshot::position_row& position) noexcept
{
    const auto unrealized =
        available_metric(position.unrealized_available, position.unrealized);
    const double basis = std::abs(position.qty * position.avg_entry);
    if (!unrealized || !std::isfinite(basis) || basis <= 0.0)
        return std::nullopt;
    return *unrealized / basis * 100.0;
}

std::optional<std::chrono::milliseconds> snapshot_age(
    const dashboard_snapshot& snapshot,
    std::chrono::steady_clock::time_point now) noexcept
{
    if (!snapshot.generated_at_available || snapshot.generated_at > now)
        return std::nullopt;
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        now - snapshot.generated_at);
}

bool snapshot_is_stale(
    const dashboard_snapshot* snapshot,
    std::chrono::steady_clock::time_point now,
    std::chrono::milliseconds max_age) noexcept
{
    if (!snapshot) return true;
    const auto age = snapshot_age(*snapshot, now);
    return !age || *age > max_age;
}

snapshot_risk_assessment assess_snapshot_risk(
    const dashboard_snapshot::risk_view& risk) noexcept
{
    if (risk.halted)
        return {snapshot_risk_level::critical, true};

    snapshot_risk_level loudest = snapshot_risk_level::safe;
    bool complete = true;
    bool configured = false;
    const auto include = [&](bool available, double current, double limit) {
        if (!(std::isfinite(limit) && limit > 0.0)) return;
        configured = true;
        if (!(available && std::isfinite(current)))
        {
            complete = false;
            return;
        }
        const auto level = level_for_fraction(std::abs(current) / limit);
        if (level > loudest) loudest = level;
    };

    include(risk.daily_loss_available, risk.daily_loss, risk.daily_loss_limit);
    include(risk.max_drawdown_available, risk.max_drawdown_pct,
            risk.max_drawdown_limit);
    include(risk.exposure_available, risk.exposure, risk.exposure_limit);
    if (risk.open_orders_limit > 0)
    {
        configured = true;
        const double fraction = static_cast<double>(risk.open_orders) /
                                static_cast<double>(risk.open_orders_limit);
        const auto level = level_for_fraction(fraction);
        if (level > loudest) loudest = level;
    }

    if (!configured || (!complete && loudest == snapshot_risk_level::safe))
        return {snapshot_risk_level::unknown, complete};
    return {loudest, complete};
}

}  // namespace truetest::ui
