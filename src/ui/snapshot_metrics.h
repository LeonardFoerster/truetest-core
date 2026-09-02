#pragma once

#include "ui/dashboard_snapshot.h"

#include <chrono>
#include <optional>

namespace truetest::ui {

enum class snapshot_risk_level
{
    unknown,
    safe,
    caution,
    warning,
    danger,
    critical,
};

struct snapshot_risk_assessment
{
    snapshot_risk_level level = snapshot_risk_level::unknown;
    bool complete = false;
};

std::optional<double> available_metric(bool available, double value) noexcept;
std::optional<double> position_notional(
    const dashboard_snapshot::position_row& position) noexcept;
std::optional<double> position_unrealized_pct(
    const dashboard_snapshot::position_row& position) noexcept;
std::optional<std::chrono::milliseconds> snapshot_age(
    const dashboard_snapshot& snapshot,
    std::chrono::steady_clock::time_point now) noexcept;
bool snapshot_is_stale(
    const dashboard_snapshot* snapshot,
    std::chrono::steady_clock::time_point now,
    std::chrono::milliseconds max_age) noexcept;
snapshot_risk_assessment assess_snapshot_risk(
    const dashboard_snapshot::risk_view& risk) noexcept;

}  // namespace truetest::ui
