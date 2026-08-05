#ifdef HAS_IMGUI_DESK

#include "ui/desk/panels/research_panels.h"

#include "ui/desk/desk_theme.h"
#include "ui/desk/desk_window_names.h"
#include "ui/desk/format_scale.h"
#include "ui/desk/panels/panel_helpers.h"

#include "imgui.h"
#include "implot.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

namespace truetest::ui::desk::panels {
namespace {

DeskDataState state_of(const ResearchPresentation* research, ResearchSurface surface)
{
    return research ? research_surface_status(*research, surface).state
                    : DeskDataState::unavailable;
}

std::int64_t age_of(const ResearchPresentation* research, ResearchSurface surface)
{
    return research ? research_surface_status(*research, surface).age_ms : 0;
}

void draw_unavailable(const char* requirement)
{
    ImGui::Spacing();
    ImGui::TextColored(theme::tx_mid(), "Research view is not connected.");
    ImGui::TextWrapped("%s", requirement);
    ImGui::Spacing();
    ImGui::TextColored(theme::tx_faint(),
                       "Enable DEMO DATA in the global bar to inspect the presentation shell.");
}

void draw_demo_watermark()
{
    const char* label = "DEMO DATA";
    const ImVec2 size = ImGui::CalcTextSize(label);
    const ImVec2 min = ImGui::GetWindowPos();
    const ImVec2 max = ImVec2(min.x + ImGui::GetWindowWidth(), min.y + ImGui::GetWindowHeight());
    ImGui::GetWindowDrawList()->AddText(
        ImVec2(max.x - size.x - 18.0f, max.y - size.y - 14.0f),
        theme::u32(ImVec4(theme::warn().x, theme::warn().y, theme::warn().z, 0.35f)), label);
}

void panel_header(const char* title,
                  const DeskLinkContext& context,
                  const ResearchPresentation* research,
                  ResearchSurface surface)
{
    const auto* status = research ? &research_surface_status(*research, surface) : nullptr;
    char meta[240];
    std::snprintf(meta, sizeof(meta), "%s · %s · %s · LINK %c%s%s",
                  context.symbol.c_str(), context.venue.c_str(), context.interval.c_str(),
                  static_cast<char>('A' + std::min<std::uint8_t>(context.link_group, 3)),
                  status && !status->source.empty() ? " · " : "",
                  status && !status->source.empty() ? status->source.c_str() : "");
    const auto state = state_of(research, surface);
    theme::panel_meta(title, meta, state, age_of(research, surface));
    if (state == DeskDataState::demo)
        draw_demo_watermark();
}

ImU32 heat_color(float value)
{
    const float v = std::clamp(value, 0.0f, 1.0f);
    const ImVec4 low = theme::bg2();
    const ImVec4 high = v > 0.72f ? theme::warn() : theme::secondary();
    const float mix = std::pow(v, 0.75f);
    return theme::u32(ImVec4(
        low.x + (high.x - low.x) * mix,
        low.y + (high.y - low.y) * mix,
        low.z + (high.z - low.z) * mix,
        0.20f + 0.78f * mix));
}

struct FootprintDataBounds
{
    std::int64_t time_min_ms = 0;
    std::int64_t time_max_ms = 1;
    double price_min = 0.0;
    double price_max = 1.0;
    bool valid = false;
};

FootprintDataBounds compute_footprint_bounds(const std::vector<FootprintBarView>& bars)
{
    FootprintDataBounds b;
    if (bars.empty())
        return b;
    b.time_min_ms = bars.front().start_ms;
    b.time_max_ms = std::max(bars.back().end_ms, b.time_min_ms + 1);
    bool have_price = false;
    for (const auto& bar : bars)
    {
        if (bar.state == FootprintBarState::empty)
            continue; // no OHLC on a real-empty interval
        if (!have_price)
        {
            b.price_min = bar.low;
            b.price_max = bar.high;
            have_price = true;
        }
        else
        {
            b.price_min = std::min(b.price_min, bar.low);
            b.price_max = std::max(b.price_max, bar.high);
        }
    }
    if (!have_price)
    {
        b.price_min = 0.0;
        b.price_max = 1.0;
    }
    if (b.price_max <= b.price_min)
        b.price_max = b.price_min + 1.0;
    b.valid = true;
    return b;
}

const char* bar_state_label(FootprintBarState s)
{
    switch (s)
    {
    case FootprintBarState::forming:  return "FORMING";
    case FootprintBarState::complete: return "COMPLETE";
    case FootprintBarState::empty:    return "EMPTY";
    }
    return "?";
}

const char* footprint_status_label(truetest::footprint::data_status status)
{
    using truetest::footprint::data_status;
    switch (status)
    {
    case data_status::unavailable: return "UNAVAILABLE";
    case data_status::backfilling: return "BACKFILLING";
    case data_status::live:        return "LIVE";
    case data_status::recovering:  return "RECOVERING";
    case data_status::partial:     return "PARTIAL";
    case data_status::stale:       return "STALE";
    case data_status::replay:      return "REPLAY";
    }
    return "UNKNOWN";
}

theme::StatusTone footprint_status_tone(truetest::footprint::data_status status)
{
    using truetest::footprint::data_status;
    switch (status)
    {
    case data_status::live:        return theme::StatusTone::positive;
    case data_status::backfilling: return theme::StatusTone::info;
    case data_status::replay:      return theme::StatusTone::info;
    case data_status::recovering:  return theme::StatusTone::warning;
    case data_status::partial:     return theme::StatusTone::warning;
    case data_status::stale:       return theme::StatusTone::warning;
    case data_status::unavailable: return theme::StatusTone::neutral;
    }
    return theme::StatusTone::neutral;
}

// Compact toolbar: bar type/interval, volume threshold, tick grouping,
// base/quote units, imbalance minimum, CVD boundary, fit, follow, and
// data-status details (footprint.md §2.3). Returns true when an
// aggregation-affecting field changed - the caller reconfigures and
// republishes the underlying aggregator/presentation.
bool draw_footprint_toolbar(FootprintPanelSettings& settings,
                            FootprintCamera& camera,
                            const ResearchPresentation* research)
{
    const FootprintPanelSettings before = settings;
    const auto bounds = research ? compute_footprint_bounds(research->footprint) : FootprintDataBounds{};
    const auto latest_index = research
        ? static_cast<std::int64_t>(research->footprint.size()) - 1 : 0;

    ImGui::PushItemWidth(theme::dp(76.0f));
    static const char* bar_type_items[] = {"1s", "5s", "15s", "1m", "5m", "Volume"};
    int bar_type_idx = static_cast<int>(settings.bar_type);
    if (ImGui::Combo("##fp_bar_type", &bar_type_idx, bar_type_items, IM_ARRAYSIZE(bar_type_items)))
        settings.bar_type = static_cast<FootprintPanelSettings::BarType>(bar_type_idx);

    if (settings.bar_type == FootprintPanelSettings::BarType::volume)
    {
        ImGui::SameLine();
        auto threshold = static_cast<float>(settings.volume_threshold);
        if (ImGui::DragFloat("##fp_vol_threshold", &threshold, 50.0f, 1.0f, 10'000'000.0f, "Vol %.0f"))
            settings.volume_threshold = threshold;
    }

    ImGui::SameLine();
    static const int tick_options[] = {1, 2, 5, 10, 25};
    static const char* tick_labels[] = {"x1", "x2", "x5", "x10", "x25"};
    int tick_idx = 0;
    for (int i = 0; i < IM_ARRAYSIZE(tick_options); ++i)
        if (tick_options[i] == settings.tick_group) tick_idx = i;
    if (ImGui::Combo("##fp_tick_group", &tick_idx, tick_labels, IM_ARRAYSIZE(tick_labels)))
        settings.tick_group = tick_options[tick_idx];

    ImGui::SameLine(); ImGui::Checkbox("Quote##fp_units", &settings.quote_units);

    ImGui::SameLine();
    int min_vol = static_cast<int>(settings.imbalance_min_volume);
    if (ImGui::DragInt("##fp_imb_min", &min_vol, 1.0f, 0, 1'000'000, "Imb min %d"))
        settings.imbalance_min_volume = min_vol;

    ImGui::SameLine();
    ImGui::DragInt("##fp_cvd_hr", &settings.cvd_reset_hour_utc, 0.25f, 0, 23, "CVD reset %dh UTC");

    ImGui::SameLine(); ImGui::Checkbox("Collapse CVD##fp_cvd_collapse", &settings.cvd_collapsed);
    ImGui::PopItemWidth();

    ImGui::SameLine();
    if (ImGui::SmallButton("Fit") && bounds.valid)
        camera.fit(latest_index, bounds.time_min_ms, bounds.time_max_ms, bounds.price_min, bounds.price_max);

    ImGui::SameLine();
    if (camera.state() == FootprintCameraState::following)
        theme::status_badge("FOLLOWING", theme::StatusTone::positive);
    else
    {
        char label[40];
        std::snprintf(label, sizeof(label), "%lld NEW \xC2\xB7 GO LIVE",
                      static_cast<long long>(camera.unseen_bars()));
        if (ImGui::SmallButton(label) && bounds.valid)
            camera.go_live(latest_index);
    }

    ImGui::SameLine();
    const auto status = research ? research->footprint_status : truetest::footprint::data_status::unavailable;
    theme::status_badge(footprint_status_label(status), footprint_status_tone(status));

    return !before.aggregation_equal(settings);
}

// Retained camera (drag to pan, wheel/Ctrl+wheel to zoom, double-click to
// fit, Home/End, -/+), forming-bar/POC/imbalance/gap/empty visuals, CVD
// subplot, and a time/price/buy/sell/delta/total/POC crosshair
// (footprint.md §2.3). Publishes the visible range into `context` for the
// future heatmap camera-group link.
void draw_footprint_canvas(const ResearchPresentation& research,
                           DeskLinkContext& context,
                           FootprintCamera& camera,
                           const FootprintPanelSettings& settings)
{
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    avail.x = std::max(avail.x, theme::dp(220.0f));
    avail.y = std::max(avail.y, theme::dp(180.0f));
    const float cvd_fraction = settings.cvd_collapsed ? 0.0f : 0.18f;
    const float chart_height = avail.y * (1.0f - cvd_fraction);
    const float cvd_top = origin.y + chart_height + theme::dp(5.0f);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, ImVec2(origin.x + avail.x, origin.y + avail.y),
                        theme::u32(theme::bg0()));

    if (research.footprint.empty())
    {
        ImGui::Dummy(avail);
        return;
    }

    const auto bounds = compute_footprint_bounds(research.footprint);
    const auto latest_bar_index = static_cast<std::int64_t>(research.footprint.size()) - 1;
    camera.update_latest(latest_bar_index, bounds.time_min_ms, bounds.time_max_ms,
                         bounds.price_min, bounds.price_max);

    const std::int64_t span_t = std::max<std::int64_t>(1, camera.time_max_ms() - camera.time_min_ms());
    const double span_p = std::max(1e-9, camera.price_max() - camera.price_min());

    auto time_to_x = [&](std::int64_t t_ms) {
        return origin.x + static_cast<float>(
            static_cast<double>(t_ms - camera.time_min_ms()) / static_cast<double>(span_t)) * avail.x;
    };
    auto price_to_y = [&](double price) {
        return origin.y + chart_height
            - static_cast<float>((price - camera.price_min()) / span_p) * chart_height;
    };

    // Viewport culling (cyrex/00-architecture.md §7).
    std::vector<std::size_t> visible_indices;
    visible_indices.reserve(std::min<std::size_t>(research.footprint.size(), 512));
    for (std::size_t i = 0; i < research.footprint.size(); ++i)
    {
        const auto& bar = research.footprint[i];
        if (bar.end_ms < camera.time_min_ms() || bar.start_ms > camera.time_max_ms())
            continue;
        visible_indices.push_back(i);
    }

    const std::size_t cells_per_bar = visible_indices.empty() ? 1
        : std::max<std::size_t>(1, max_visible_research_cells / visible_indices.size());
    double max_total_all = 1.0;
    for (auto i : visible_indices)
        for (const auto& lv : research.footprint[i].levels)
            max_total_all = std::max(max_total_all, lv.buy_qty + lv.sell_qty + lv.unknown_qty);

    for (auto bar_index : visible_indices)
    {
        const auto& bar = research.footprint[bar_index];
        const float x0 = time_to_x(bar.start_ms);
        const float x1 = std::max(x0 + theme::dp(1.0f), time_to_x(bar.end_ms) - theme::dp(1.0f));
        const float bar_width = x1 - x0;

        if (bar.state == FootprintBarState::empty)
        {
            // A real interval with zero trades - distinct from a gap.
            draw->AddRectFilled(ImVec2(x0, origin.y), ImVec2(x1, origin.y + chart_height),
                                theme::u32(ImVec4(theme::tx_faint().x, theme::tx_faint().y,
                                                  theme::tx_faint().z, 0.07f)));
            continue;
        }
        if (bar.gap)
        {
            // Hatched reconciliation gap (§2.2) - diagonal stripes.
            const ImU32 hatch = theme::u32(
                ImVec4(theme::warn().x, theme::warn().y, theme::warn().z, 0.18f));
            for (float x = x0 - chart_height; x < x1 + chart_height; x += theme::dp(8.0f))
                draw->AddLine(ImVec2(x, origin.y), ImVec2(x + chart_height, origin.y + chart_height),
                              hatch, 1.0f);
        }

        const std::size_t level_stride = research_render_stride(bar.levels.size(), cells_per_bar);
        for (std::size_t li = 0; li < bar.levels.size(); li += level_stride)
        {
            const auto& level = bar.levels[li];
            const double total = level.buy_qty + level.sell_qty + level.unknown_qty;
            const float intensity = static_cast<float>(total / max_total_all);

            double step = 0.0;
            if (li + 1 < bar.levels.size()) step = bar.levels[li + 1].price - level.price;
            else if (li > 0) step = level.price - bar.levels[li - 1].price;
            if (step <= 0.0) step = span_p / 40.0;

            const float y1 = price_to_y(level.price);
            const float y0 = price_to_y(level.price + step);
            const bool draw_text = (y1 - y0) >= ImGui::GetFontSize() + theme::dp(2.0f)
                && bar_width >= theme::dp(46.0f);

            draw->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), heat_color(intensity * 0.75f));
            const auto split = static_cast<float>(total > 0.0 ? level.sell_qty / total : 0.5);
            draw->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + bar_width * split, y1),
                                theme::u32(ImVec4(theme::down().x, theme::down().y,
                                                  theme::down().z, 0.10f + intensity * 0.22f)));
            draw->AddRectFilled(ImVec2(x0 + bar_width * split, y0), ImVec2(x1, y1),
                                theme::u32(ImVec4(theme::up().x, theme::up().y,
                                                  theme::up().z, 0.10f + intensity * 0.22f)));

            if (level.is_poc)
                draw->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), theme::u32(theme::accent()),
                              0.0f, 0, theme::dp(1.5f));
            if (level.diagonal != FootprintImbalance::none)
            {
                const ImVec4 tint = level.diagonal == FootprintImbalance::buy ? theme::up() : theme::down();
                draw->AddRect(ImVec2(x0, y0), ImVec2(x1, y1),
                              theme::u32(ImVec4(tint.x, tint.y, tint.z, level.stacked ? 0.95f : 0.55f)),
                              0.0f, 0, level.stacked ? theme::dp(2.0f) : theme::dp(1.0f));
            }
            if (draw_text)
            {
                char cell[48];
                std::snprintf(cell, sizeof(cell), "%.0f|%.0f", level.sell_qty, level.buy_qty);
                draw->AddText(ImVec2(x0 + theme::dp(2.0f), y0), theme::u32(theme::tx_mid()), cell);
            }
        }

        if (bar.state == FootprintBarState::forming)
            draw->AddRect(ImVec2(x0, origin.y), ImVec2(x1, origin.y + chart_height),
                          theme::u32(theme::secondary()), 0.0f, 0, theme::dp(1.0f));
    }

    const double last_price = research.footprint.back().close;
    const float last_y = price_to_y(last_price);
    draw->AddLine(ImVec2(origin.x, last_y), ImVec2(origin.x + avail.x, last_y),
                  theme::u32(theme::accent_dim()), theme::dp(1.0f));

    if (!settings.cvd_collapsed && !visible_indices.empty())
    {
        draw->AddLine(ImVec2(origin.x, cvd_top), ImVec2(origin.x + avail.x, cvd_top),
                      theme::u32(theme::line_hi()));
        double min_cvd = research.footprint[visible_indices.front()].cvd;
        double max_cvd = min_cvd;
        for (auto i : visible_indices)
        {
            min_cvd = std::min(min_cvd, research.footprint[i].cvd);
            max_cvd = std::max(max_cvd, research.footprint[i].cvd);
        }
        const double cvd_span = std::max(max_cvd - min_cvd, 1.0);
        const float cvd_plot_h = avail.y - chart_height - theme::dp(10.0f);
        auto cvd_point = [&](std::size_t i) {
            const auto& bar = research.footprint[i];
            return ImVec2((time_to_x(bar.start_ms) + time_to_x(bar.end_ms)) * 0.5f,
                          cvd_top + theme::dp(5.0f) + cvd_plot_h
                              * (1.0f - static_cast<float>((bar.cvd - min_cvd) / cvd_span)));
        };
        for (std::size_t k = 1; k < visible_indices.size(); ++k)
            draw->AddLine(cvd_point(visible_indices[k - 1]), cvd_point(visible_indices[k]),
                          theme::u32(theme::secondary()), theme::dp(1.5f));
        draw->AddText(ImVec2(origin.x + theme::dp(6.0f), cvd_top + theme::dp(6.0f)),
                      theme::u32(theme::tx_lo()), "CVD · buy aggressor − sell aggressor");
    }

    char price_axis[64];
    std::snprintf(price_axis, sizeof(price_axis), "%.1f  LAST %.1f  %.1f",
                  camera.price_max(), last_price, camera.price_min());
    draw->AddText(ImVec2(origin.x + theme::dp(6.0f), origin.y + theme::dp(5.0f)),
                  theme::u32(theme::tx_lo()), price_axis);

    // --- Retained camera interaction (footprint.md §2.3) ---
    ImGui::InvisibleButton("##fp_canvas_input", avail, ImGuiButtonFlags_MouseButtonLeft);
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 mouse = ImGui::GetIO().MousePos;

    if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
    {
        const ImVec2 delta = ImGui::GetIO().MouseDelta;
        camera.pan(static_cast<double>(-delta.x) / std::max(1.0f, avail.x),
                  static_cast<double>(delta.y) / std::max(1.0f, chart_height));
    }
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        camera.fit(latest_bar_index, bounds.time_min_ms, bounds.time_max_ms,
                  bounds.price_min, bounds.price_max);
    if (hovered && ImGui::GetIO().MouseWheel != 0.0f)
    {
        const double anchor_x = std::clamp(
            static_cast<double>((mouse.x - origin.x) / std::max(1.0f, avail.x)), 0.0, 1.0);
        const double anchor_y = std::clamp(
            static_cast<double>(1.0f - (mouse.y - origin.y) / std::max(1.0f, chart_height)), 0.0, 1.0);
        const double factor = std::pow(1.15, ImGui::GetIO().MouseWheel);
        if (ImGui::GetIO().KeyCtrl)
            camera.zoom_price(factor, anchor_y);
        else
            camera.zoom_time(factor, anchor_x);
    }
    if (ImGui::IsWindowFocused())
    {
        if (ImGui::IsKeyPressed(ImGuiKey_Home)) camera.jump_to_start(bounds.time_min_ms);
        if (ImGui::IsKeyPressed(ImGuiKey_End)) camera.go_live(latest_bar_index);
        if (ImGui::IsKeyPressed(ImGuiKey_Minus) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract))
            camera.zoom_time(1.0 / 1.3, 0.5);
        if (ImGui::IsKeyPressed(ImGuiKey_Equal) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd))
            camera.zoom_time(1.3, 0.5);
    }

    // Crosshair: time, price, buy, sell, delta, total, POC (§2.3).
    if (hovered && mouse.y <= origin.y + chart_height)
    {
        const double hovered_price = camera.price_min()
            + static_cast<double>((origin.y + chart_height - mouse.y) / chart_height) * span_p;
        draw->AddLine(ImVec2(origin.x, mouse.y), ImVec2(origin.x + avail.x, mouse.y),
                      theme::u32(theme::line_hi()));
        draw->AddLine(ImVec2(mouse.x, origin.y), ImVec2(mouse.x, origin.y + chart_height),
                      theme::u32(theme::line_hi()));

        const FootprintBarView* hovered_bar = nullptr;
        for (auto i : visible_indices)
        {
            const auto& bar = research.footprint[i];
            if (mouse.x >= time_to_x(bar.start_ms) && mouse.x <= time_to_x(bar.end_ms))
            { hovered_bar = &bar; break; }
        }
        if (hovered_bar)
        {
            double buy = 0.0, sell = 0.0, total = 0.0, poc_price = 0.0;
            bool has_poc = false;
            for (const auto& lv : hovered_bar->levels)
            {
                buy += lv.buy_qty; sell += lv.sell_qty; total += lv.buy_qty + lv.sell_qty + lv.unknown_qty;
                if (lv.is_poc) { poc_price = lv.price; has_poc = true; }
            }
            char tip[256];
            if (has_poc)
                std::snprintf(tip, sizeof(tip),
                    "%s\nPrice %.2f\nBuy %.1f  Sell %.1f  Delta %+.1f\nTotal %.1f\nPOC %.2f",
                    bar_state_label(hovered_bar->state), hovered_price, buy, sell, buy - sell,
                    total, poc_price);
            else
                std::snprintf(tip, sizeof(tip),
                    "%s\nPrice %.2f\nBuy %.1f  Sell %.1f  Delta %+.1f\nTotal %.1f",
                    bar_state_label(hovered_bar->state), hovered_price, buy, sell, buy - sell, total);
            ImGui::SetTooltip("%s", tip);
        }
        else
        {
            ImGui::SetTooltip("Price %.2f", hovered_price);
        }
    }

    // Publish the visible range for the future heatmap camera-group link
    // (footprint.md §2.3 "link by symbol, visible time range, and price range").
    context.time_min_ms = camera.time_min_ms();
    context.time_max_ms = camera.time_max_ms();
    context.price_min = camera.price_min();
    context.price_max = camera.price_max();
    context.camera_initialized = camera.initialized();
}

void draw_profile_histogram(const ResearchPresentation& research, bool tpo)
{
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    avail.x = std::max(avail.x, theme::dp(160.0f));
    avail.y = std::max(avail.y, theme::dp(120.0f));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, ImVec2(origin.x + avail.x, origin.y + avail.y),
                        theme::u32(theme::bg0()));
    constexpr std::size_t max_profile_rows = 512;
    const std::size_t visible_rows = std::min(research.profile.size(), max_profile_rows);
    const std::size_t first_row = research.profile.size() - visible_rows;
    double max_volume = 1.0;
    for (std::size_t i = first_row; i < research.profile.size(); ++i)
    {
        const auto& row = research.profile[i];
        max_volume = std::max(max_volume, row.buy + row.sell);
    }
    const float row_h = visible_rows == 0 ? avail.y
        : avail.y / static_cast<float>(visible_rows);
    for (std::size_t i = 0; i < visible_rows; ++i)
    {
        const auto& row = research.profile[research.profile.size() - 1 - i];
        const float y0 = origin.y + static_cast<float>(i) * row_h;
        const float y1 = y0 + std::max(theme::dp(2.0f), row_h - theme::dp(1.0f));
        char price[32];
        std::snprintf(price, sizeof(price), "%.1f", row.price);
        draw->AddText(ImVec2(origin.x + theme::dp(4.0f), y0), theme::u32(theme::tx_lo()), price);
        if (tpo)
        {
            draw->AddText(ImVec2(origin.x + theme::dp(78.0f), y0), theme::u32(theme::secondary()),
                          row.tpo.c_str());
        }
        else
        {
            const float width = (avail.x - theme::dp(86.0f))
                * static_cast<float>((row.buy + row.sell) / max_volume);
            const float sell_fraction = static_cast<float>(
                (row.buy + row.sell) > 0.0 ? row.sell / (row.buy + row.sell) : 0.5);
            draw->AddRectFilled(ImVec2(origin.x + theme::dp(82.0f), y0 + theme::dp(1.0f)),
                                ImVec2(origin.x + theme::dp(82.0f) + width * sell_fraction, y1),
                                theme::u32(theme::down_dim()));
            draw->AddRectFilled(ImVec2(origin.x + theme::dp(82.0f) + width * sell_fraction,
                                       y0 + theme::dp(1.0f)),
                                ImVec2(origin.x + theme::dp(82.0f) + width, y1),
                                theme::u32(theme::up_dim()));
        }
    }
    ImGui::Dummy(avail);
}

} // namespace

void draw_watchlist_panel(const dashboard_snapshot* snap,
                          const ResearchPresentation* research,
                          DeskLinkContext& context,
                          DeskDensity density)
{
    if (!ImGui::Begin(desk_window_name(DeskPanel::watchlist))) { ImGui::End(); return; }
    panel_header("WATCHLIST", context, research, ResearchSurface::watchlist);
    const float row_height = theme::dp(desk_row_height(density));
    if (research && !research->watchlist.empty()
        && ImGui::BeginTable("watchlist_rows", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                             ImVec2(0, -1)))
    {
        ImGui::TableSetupColumn("SYMBOL"); ImGui::TableSetupColumn("LAST"); ImGui::TableSetupColumn("CHG");
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(clipper_count(research->watchlist.size()), row_height);
        while (clipper.Step())
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
        {
            const auto& row = research->watchlist[static_cast<std::size_t>(i)];
            ImGui::TableNextRow(ImGuiTableRowFlags_None, row_height);
            ImGui::TableNextColumn();
            if (ImGui::Selectable(row.symbol.c_str(), context.symbol == row.symbol,
                                  ImGuiSelectableFlags_SpanAllColumns))
                context.symbol = row.symbol;
            ImGui::TableNextColumn(); ImGui::Text("%.2f", row.last);
            ImGui::TableNextColumn(); ImGui::TextColored(theme::pnl_color(row.change_pct), "%+.2f%%", row.change_pct);
        }
        ImGui::EndTable();
    }
    else if (snap && snap->l2.source != dashboard_snapshot::l2_source::none)
    {
        ImGui::TextColored(theme::data_link(), "%s", snap->l2.symbol.c_str());
        ImGui::SameLine(); mono_text(fmt_px(snap->l2.mid).c_str());
        ImGui::TextColored(theme::tx_faint(), "Only the engine's active L2 symbol is available.");
    }
    else
        draw_unavailable("A future research source will provide the configured symbol universe.");
    ImGui::End();
}

bool draw_orderflow_canvas_panel(const ResearchPresentation* research,
                                 DeskLinkContext& context,
                                 FootprintCamera& camera,
                                 FootprintPanelSettings& settings)
{
    if (!ImGui::Begin(desk_window_name(DeskPanel::orderflow_canvas))) { ImGui::End(); return false; }
    panel_header("FOOTPRINT / ORDERFLOW", context, research, ResearchSurface::footprint);

    const bool needs_reaggregate = draw_footprint_toolbar(settings, camera, research);

    if (!research || research->footprint.empty())
        draw_unavailable("Requires normalized public trades, instrument tick size, and the cold ResearchStore.");
    else
        draw_footprint_canvas(*research, context, camera, settings);

    ImGui::End();
    return needs_reaggregate;
}

void draw_dom_panel(DeskPanel panel,
                    const dashboard_snapshot* snap,
                    const ResearchPresentation* research,
                    const DeskLinkContext& context,
                    DeskDensity density)
{
    if (!ImGui::Begin(desk_window_name(panel))) { ImGui::End(); return; }
    const bool actual_l2 = snap && snap->l2.source != dashboard_snapshot::l2_source::none;
    const DeskDataState state = research && !research->dom.empty()
        ? state_of(research, ResearchSurface::dom)
        : (actual_l2 ? DeskDataState::snapshot : DeskDataState::unavailable);
    char meta[128];
    std::snprintf(meta, sizeof(meta), "%s · tick ×1 · 5s tape", context.symbol.c_str());
    theme::panel_meta("DEPTH OF MARKET", meta, state,
                      age_of(research, ResearchSurface::dom));

    if (research && !research->dom.empty())
    {
        double reference_price = research->dom[research->dom.size() / 2].price;
        if (!research->footprint.empty())
            reference_price = research->footprint.back().close;
        if (actual_l2 && snap->l2.symbol == context.symbol)
            reference_price = snap->l2.mid;
        if (ImGui::BeginTable("dom_rows", 6,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY
                                  | ImGuiTableFlags_BordersInnerV,
                              ImVec2(0, -1)))
        {
            const char* columns[] = {"BID", "PRICE", "ASK", "BOUGHT", "SOLD", "DELTA"};
            for (const char* column : columns) ImGui::TableSetupColumn(column);
            ImGui::TableSetupScrollFreeze(0, 1); ImGui::TableHeadersRow();
            const float row_height = theme::dp(desk_row_height(density));
            ImGuiListClipper clipper;
            clipper.Begin(clipper_count(research->dom.size()), row_height);
            while (clipper.Step())
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
            {
                const auto& row = research->dom[static_cast<std::size_t>(i)];
                ImGui::TableNextRow(ImGuiTableRowFlags_None, row_height);
                ImGui::TableNextColumn(); ImGui::TextColored(theme::up(), row.bid_qty > 0.0 ? "%.2f" : "", row.bid_qty);
                ImGui::TableNextColumn(); ImGui::TextColored(std::abs(row.price - reference_price) < 0.01 ? theme::accent() : theme::tx_hi(), "%.1f", row.price);
                ImGui::TableNextColumn(); ImGui::TextColored(theme::down(), row.ask_qty > 0.0 ? "%.2f" : "", row.ask_qty);
                ImGui::TableNextColumn(); ImGui::Text("%.1f", row.bought);
                ImGui::TableNextColumn(); ImGui::Text("%.1f", row.sold);
                ImGui::TableNextColumn(); ImGui::TextColored(theme::pnl_color(row.bought - row.sold), "%+.1f", row.bought - row.sold);
            }
            ImGui::EndTable();
        }
    }
    else if (actual_l2)
    {
        ImGui::TextColored(theme::tx_lo(), "Resting depth from engine snapshot · aggression unavailable");
        if (ImGui::BeginTable("snapshot_dom", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                              ImVec2(0, -1)))
        {
            ImGui::TableSetupColumn("BID"); ImGui::TableSetupColumn("PRICE"); ImGui::TableSetupColumn("ASK");
            ImGui::TableHeadersRow();
            for (auto it = snap->l2.asks.rbegin(); it != snap->l2.asks.rend(); ++it)
            {
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted("—");
                ImGui::TableNextColumn(); ImGui::TextColored(theme::down(), "%.2f", it->price);
                ImGui::TableNextColumn(); ImGui::Text("%.4f", it->size);
            }
            ImGui::TableNextRow(); ImGui::TableNextColumn();
            ImGui::TableNextColumn(); ImGui::TextColored(theme::accent(), "MID %.2f", snap->l2.mid);
            for (const auto& level : snap->l2.bids)
            {
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::Text("%.4f", level.size);
                ImGui::TableNextColumn(); ImGui::TextColored(theme::up(), "%.2f", level.price);
                ImGui::TableNextColumn(); ImGui::TextUnformatted("—");
            }
            ImGui::EndTable();
        }
    }
    else
        draw_unavailable("Enable --depth-stream for resting depth. Public tape is required for Bought/Sold/Delta.");
    ImGui::End();
}

void draw_selected_context_panel(const dashboard_snapshot* snap,
                                 const ResearchPresentation* research,
                                 const DeskLinkContext& context)
{
    if (!ImGui::Begin(desk_window_name(DeskPanel::selected_context))) { ImGui::End(); return; }
    theme::panel_meta("SELECTED CONTEXT", context.symbol.c_str(),
                      snap ? DeskDataState::snapshot
                           : state_of(research, ResearchSurface::footprint),
                      age_of(research, ResearchSurface::footprint));
    if (snap && snap->l2.source != dashboard_snapshot::l2_source::none
        && snap->l2.symbol == context.symbol)
    {
        ImGui::TextColored(theme::tx_lo(), "MID"); ImGui::SameLine();
        ImGui::TextColored(theme::tx_hi(), "%s", fmt_px(snap->l2.mid).c_str());
        ImGui::Text("Bid %s   Ask %s", fmt_px(snap->l2.best_bid).c_str(), fmt_px(snap->l2.best_ask).c_str());
        ImGui::Text("Spread %s", fmt_bps(snap->l2.spread_bps).c_str());
    }
    else
        ImGui::TextColored(theme::tx_faint(), "No live quote in the engine snapshot");
    ImGui::Separator();
    ImGui::TextColored(theme::tx_lo(), "OPEN EXPOSURE");
    bool found = false;
    if (snap)
        for (const auto& position : snap->positions)
            if (position.symbol == context.symbol)
            {
                found = true;
                position_side_badge(position.qty); ImGui::SameLine();
                mono_text(fmt_qty(abs_qty(position.qty)).c_str());
                ImGui::Text("Entry %s   Mark %s", fmt_px(position.avg_entry).c_str(), fmt_px(position.mark).c_str());
                ImGui::Text("uPnL"); ImGui::SameLine(); text_pnl(position.unrealized);
            }
    if (!found) ImGui::TextColored(theme::tx_faint(), "No position for selected symbol");
    ImGui::TextColored(theme::tx_faint(), "Order entry is intentionally not part of research panels.");
    ImGui::End();
}

void draw_liquidity_panel(const ResearchPresentation* research,
                          const DeskLinkContext& context)
{
    if (!ImGui::Begin(desk_window_name(DeskPanel::liquidity_heatmap))) { ImGui::End(); return; }
    panel_header("LIQUIDITY HEATMAP", context, research, ResearchSurface::heatmap);
    ImGui::TextColored(theme::tx_lo(), "100ms columns · log quantity · follow mid · bounded 12k-cell render budget");
    if (!research || research->heatmap.empty())
    {
        draw_unavailable("Requires sampled L2 history from a cold BookHistoryStore.");
        ImGui::End(); return;
    }
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    avail.x = std::max(avail.x, theme::dp(220.0f));
    avail.y = std::max(avail.y, theme::dp(160.0f));
    const std::size_t cell_count = research->heatmap.size();
    const std::size_t stride = research_render_stride(cell_count);
    const float cw = avail.x / static_cast<float>(std::max<std::uint16_t>(1, research->heatmap_columns));
    const float rh = avail.y / static_cast<float>(std::max<std::uint16_t>(1, research->heatmap_rows));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, ImVec2(origin.x + avail.x, origin.y + avail.y), theme::u32(theme::bg0()));
    for (std::size_t i = 0; i < research->heatmap.size(); i += stride)
    {
        const auto& cell = research->heatmap[i];
        const ImVec2 p0(origin.x + static_cast<float>(cell.column) * cw,
                        origin.y + avail.y - static_cast<float>(cell.row + 1) * rh);
        draw->AddRectFilled(p0,
                            ImVec2(p0.x + std::max(theme::dp(1.0f), cw),
                                   p0.y + std::max(theme::dp(1.0f), rh)),
                            heat_color(cell.value));
    }
    constexpr std::size_t max_liquidation_markers = 2'048;
    const std::size_t first_liquidation = research->liquidations.size()
        > max_liquidation_markers ? research->liquidations.size() - max_liquidation_markers : 0;
    for (std::size_t i = first_liquidation; i < research->liquidations.size(); ++i)
    {
        const auto& liq = research->liquidations[i];
        const double tx = static_cast<double>(liq.ts_ms - research->heatmap_start_ms)
            / std::max<std::int64_t>(1, research->heatmap_end_ms - research->heatmap_start_ms);
        const double py = (liq.price - research->heatmap_min_price)
            / std::max(1.0, research->heatmap_max_price - research->heatmap_min_price);
        const ImVec2 p(origin.x + static_cast<float>(tx) * avail.x,
                       origin.y + avail.y - static_cast<float>(py) * avail.y);
        const float radius = theme::dp(2.0f)
            + static_cast<float>(std::log10(std::max(1.0, liq.notional)) - 3.0);
        draw->AddCircleFilled(p, std::max(radius, theme::dp(2.0f)),
                              theme::u32(liq.long_liquidated ? theme::down() : theme::up()));
    }
    char scale[96];
    std::snprintf(scale, sizeof(scale), "%.1f  LIQUIDITY  %.1f",
                  research->heatmap_max_price, research->heatmap_min_price);
    draw->AddText(ImVec2(origin.x + theme::dp(6.0f), origin.y + theme::dp(5.0f)),
                  theme::u32(theme::tx_mid()), scale);
    if (ImGui::IsWindowHovered())
    {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        if (mouse.x >= origin.x && mouse.x <= origin.x + avail.x
            && mouse.y >= origin.y && mouse.y <= origin.y + avail.y)
        {
            const double hovered_price = research->heatmap_max_price
                - static_cast<double>((mouse.y - origin.y) / avail.y)
                    * (research->heatmap_max_price - research->heatmap_min_price);
            draw->AddLine(ImVec2(origin.x, mouse.y), ImVec2(origin.x + avail.x, mouse.y),
                          theme::u32(theme::line_hi()), theme::dp(1.0f));
            ImGui::SetTooltip("Price %.2f · historical resting liquidity", hovered_price);
        }
    }
    ImGui::Dummy(avail);
    ImGui::End();
}

void draw_liquidations_panel(const ResearchPresentation* research,
                             const DeskLinkContext& context)
{
    if (!ImGui::Begin(desk_window_name(DeskPanel::liquidations))) { ImGui::End(); return; }
    panel_header("LIQUIDATION CLUSTERS", context, research, ResearchSurface::liquidations);
    if (!research || research->liquidations.empty())
        draw_unavailable("Requires normalized venue force-order events. Estimates will always be labeled MODEL.");
    else
    {
        double longs = 0.0, shorts = 0.0;
        constexpr std::size_t max_summary_events = 2'048;
        const std::size_t first = research->liquidations.size() > max_summary_events
            ? research->liquidations.size() - max_summary_events : 0;
        for (std::size_t i = first; i < research->liquidations.size(); ++i)
        {
            const auto& row = research->liquidations[i];
            (row.long_liquidated ? longs : shorts) += row.notional;
        }
        ImGui::TextColored(theme::down(), "Long liq  $%.0f", longs);
        ImGui::TextColored(theme::up(), "Short liq $%.0f", shorts);
        ImGui::Separator();
        ImGui::TextColored(theme::tx_faint(), "Leverage cascade bands are not shown without an explicit MODEL source.");
    }
    ImGui::End();
}

void draw_liquidity_tape_panel(const ResearchPresentation* research,
                               const DeskLinkContext& context,
                               DeskDensity density)
{
    if (!ImGui::Begin(desk_window_name(DeskPanel::liquidity_tape))) { ImGui::End(); return; }
    panel_header("LIQUIDATION TAPE", context, research, ResearchSurface::liquidations);
    if (!research || research->liquidations.empty())
        draw_unavailable("Real venue liquidation prints will appear here after provider wiring.");
    else if (ImGui::BeginTable("liquidation_tape", 4,
                               ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, -1)))
    {
        ImGui::TableSetupColumn("SIDE"); ImGui::TableSetupColumn("PRICE");
        ImGui::TableSetupColumn("NOTIONAL"); ImGui::TableSetupColumn("SOURCE");
        ImGui::TableHeadersRow();
        const float row_height = theme::dp(desk_row_height(density));
        ImGuiListClipper clipper;
        clipper.Begin(clipper_count(research->liquidations.size()), row_height);
        while (clipper.Step())
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
        {
            const auto& row = research->liquidations[static_cast<std::size_t>(i)];
            ImGui::TableNextRow(ImGuiTableRowFlags_None, row_height);
            ImGui::TableNextColumn(); theme::status_badge(row.long_liquidated ? "LONG LIQ" : "SHORT LIQ",
                row.long_liquidated ? theme::StatusTone::negative : theme::StatusTone::positive);
            ImGui::TableNextColumn(); ImGui::Text("%.1f", row.price);
            ImGui::TableNextColumn(); ImGui::Text("$%.0f", row.notional);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(
                research_surface_status(*research, ResearchSurface::liquidations).source.empty()
                    ? desk_data_state_text(state_of(research, ResearchSurface::liquidations))
                    : research_surface_status(*research, ResearchSurface::liquidations).source.c_str());
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

void draw_tpo_panel(const ResearchPresentation* research,
                    const DeskLinkContext& context)
{
    if (!ImGui::Begin(desk_window_name(DeskPanel::tpo_profile))) { ImGui::End(); return; }
    panel_header("MARKET PROFILE / TPO", context, research, ResearchSurface::profile);
    ImGui::TextColored(theme::tx_lo(), "30m brackets · UTC session · print mode · IB 60m");
    if (!research || research->profile.empty())
        draw_unavailable("Requires the shared session calendar and public print stream.");
    else
        draw_profile_histogram(*research, true);
    ImGui::End();
}

void draw_volume_profile_panel(const ResearchPresentation* research,
                               const DeskLinkContext& context)
{
    if (!ImGui::Begin(desk_window_name(DeskPanel::volume_profile))) { ImGui::End(); return; }
    panel_header("VOLUME PROFILE", context, research, ResearchSurface::profile);
    if (!research || research->profile.empty())
        draw_unavailable("Requires session public trades and the shared integer-tick PriceAxis.");
    else
        draw_profile_histogram(*research, false);
    ImGui::End();
}

void draw_session_context_panel(const ResearchPresentation* research,
                                const DeskLinkContext& context)
{
    if (!ImGui::Begin(desk_window_name(DeskPanel::session_context))) { ImGui::End(); return; }
    panel_header("SESSION CONTEXT", context, research, ResearchSurface::profile);
    ImGui::Text("Template        %s", context.session.c_str());
    ImGui::Text("TPO bracket     30m");
    ImGui::Text("Value area      70%%");
    ImGui::Text("Price grouping  tick ×1");
    ImGui::Separator();
    ImGui::TextColored(theme::tx_faint(), "POC / VA / IB values remain unavailable until the research model is wired.");
    ImGui::End();
}

void draw_funding_panel(const ResearchPresentation* research,
                        const DeskLinkContext& context,
                        DeskDensity density)
{
    if (!ImGui::Begin(desk_window_name(DeskPanel::funding))) { ImGui::End(); return; }
    panel_header("FUNDING INTELLIGENCE", context, research, ResearchSurface::funding);
    if (!research || research->funding.empty())
        draw_unavailable("Requires market funding quotes; portfolio funding cash events are intentionally not used.");
    else if (ImGui::BeginTable("funding_rows", 6,
                               ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY
                                   | ImGuiTableFlags_BordersInnerV, ImVec2(0, -1)))
    {
        const char* columns[] = {"SYMBOL", "VENUE", "RATE", "ANN.", "BASIS", "NEXT"};
        for (const char* column : columns) ImGui::TableSetupColumn(column);
        ImGui::TableHeadersRow();
        const float row_height = theme::dp(desk_row_height(density));
        ImGuiListClipper clipper;
        clipper.Begin(clipper_count(research->funding.size()), row_height);
        while (clipper.Step())
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
        {
            const auto& row = research->funding[static_cast<std::size_t>(i)];
            ImGui::TableNextRow(ImGuiTableRowFlags_None, row_height);
            ImGui::TableNextColumn(); ImGui::TextColored(theme::data_link(), "%s", row.symbol.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(row.venue.c_str());
            ImGui::TableNextColumn(); ImGui::TextColored(theme::pnl_color(-row.funding_rate), "%+.4f%%", row.funding_rate * 100.0);
            ImGui::TableNextColumn(); ImGui::Text("%+.2f%%", row.annualized * 100.0);
            ImGui::TableNextColumn(); ImGui::Text("%+.1f", row.basis_bps);
            ImGui::TableNextColumn(); ImGui::Text("%02lld:%02lld:%02lld",
                static_cast<long long>(row.next_seconds / 3600),
                static_cast<long long>((row.next_seconds / 60) % 60),
                static_cast<long long>(row.next_seconds % 60));
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

void draw_correlation_panel(const ResearchPresentation* research,
                            const DeskLinkContext& context)
{
    if (!ImGui::Begin(desk_window_name(DeskPanel::correlation))) { ImGui::End(); return; }
    panel_header("ROLLING CORRELATION", context, research, ResearchSurface::correlation);
    if (!research || research->correlation_symbols.empty())
    {
        draw_unavailable("Requires aligned multi-symbol mid-price returns sampled on the cold analytics cadence.");
        ImGui::End(); return;
    }
    const std::size_t published_n = research->correlation_symbols.size();
    if (published_n == 0 || published_n > research->correlation.size() / published_n)
    {
        ImGui::TextColored(theme::danger(), "Correlation publication is incomplete.");
        ImGui::End();
        return;
    }
    constexpr std::size_t max_correlation_symbols = 16;
    const std::size_t n = std::min(published_n, max_correlation_symbols);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float cell = std::max(theme::dp(42.0f), std::min(theme::dp(86.0f),
        ImGui::GetContentRegionAvail().x / static_cast<float>(n + 1)));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    for (std::size_t col = 0; col < n; ++col)
        draw->AddText(ImVec2(origin.x + static_cast<float>(col + 1) * cell + theme::dp(5.0f), origin.y),
                      theme::u32(theme::tx_mid()), research->correlation_symbols[col].c_str());
    for (std::size_t row = 0; row < n; ++row)
    {
        draw->AddText(ImVec2(origin.x + theme::dp(4.0f),
                            origin.y + static_cast<float>(row + 1) * cell + theme::dp(5.0f)),
                      theme::u32(theme::tx_mid()), research->correlation_symbols[row].c_str());
        for (std::size_t col = 0; col < n; ++col)
        {
            const double rho = research->correlation[row * published_n + col];
            const ImVec4 base = rho >= 0.0 ? theme::up() : theme::down();
            const ImVec2 p0(origin.x + static_cast<float>(col + 1) * cell,
                            origin.y + static_cast<float>(row + 1) * cell);
            draw->AddRectFilled(p0,
                ImVec2(p0.x + cell - theme::dp(2.0f), p0.y + cell - theme::dp(2.0f)),
                theme::u32(ImVec4(base.x, base.y, base.z, 0.10f + 0.45f * static_cast<float>(std::abs(rho)))));
            char label[16]; std::snprintf(label, sizeof(label), "%+.2f", rho);
            draw->AddText(ImVec2(p0.x + theme::dp(7.0f), p0.y + theme::dp(7.0f)),
                          theme::u32(theme::tx_hi()), label);
        }
    }
    ImGui::Dummy(ImVec2(static_cast<float>(n + 1) * cell, static_cast<float>(n + 1) * cell));
    ImGui::TextColored(theme::tx_faint(), "15m window · 5s returns · pairwise complete observations");
    ImGui::End();
}

void draw_market_detail_panel(const ResearchPresentation* research,
                              const DeskLinkContext& context)
{
    if (!ImGui::Begin(desk_window_name(DeskPanel::market_detail))) { ImGui::End(); return; }
    panel_header("PAIR / CARRY DETAIL", context, research, ResearchSurface::correlation);
    ImGui::TextColored(theme::tx_mid(), "Pair and carry detail is reserved for the future selection contract.");
    ImGui::TextColored(theme::tx_faint(), "Cross-venue carry is illustrative and never places orders.");
    ImGui::End();
}

} // namespace truetest::ui::desk::panels

#endif
