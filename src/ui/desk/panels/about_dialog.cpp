#ifdef HAS_IMGUI_DESK

#include "ui/desk/panels/about_dialog.h"

#include "core/tt_target.h"
#include "ui/desk/about_model.h"
#include "ui/desk/desk_theme.h"

#include "imgui.h"
#include "implot.h"
#include <tt/truetest_version.h>

#include <array>

namespace truetest::ui::desk {
namespace {

#ifdef HAS_RICH_TUI
constexpr bool has_rich_tui = true;
#else
constexpr bool has_rich_tui = false;
#endif
#ifdef HAS_BINANCE
constexpr bool has_binance = true;
#else
constexpr bool has_binance = false;
#endif
#ifdef HAS_BITGET
constexpr bool has_bitget = true;
#else
constexpr bool has_bitget = false;
#endif
#ifdef HAS_BITUNIX
constexpr bool has_bitunix = true;
#else
constexpr bool has_bitunix = false;
#endif
constexpr bool has_live_data = has_binance || has_bitget || has_bitunix;
#ifdef HAS_QUESTDB
constexpr bool has_questdb = true;
#else
constexpr bool has_questdb = false;
#endif
#ifdef HAS_WEB
constexpr bool has_web = true;
#else
constexpr bool has_web = false;
#endif
#ifdef HAS_DEBUG
constexpr bool has_debug = true;
#else
constexpr bool has_debug = false;
#endif

struct Feature
{
    const char* name;
    bool enabled;
};

constexpr std::array compiled_features = {
    Feature{"ImGui desk", true},
    Feature{"Rich TUI", has_rich_tui},
    Feature{"Venue live-data", has_live_data},
    Feature{"Binance", has_binance},
    Feature{"Bitget", has_bitget},
    Feature{"Bitunix", has_bitunix},
    Feature{"QuestDB", has_questdb},
    Feature{"Web API", has_web},
    Feature{"Debug samplers", has_debug},
};

DeskBuildInfo current_build_info() noexcept
{
    return {
        TRUETEST_VERSION,
        TRUETEST_GIT_SHA,
        TRUETEST_GIT_DIRTY,
        TRUETEST_BUILD_TIMESTAMP,
        TRUETEST_BUILD_TYPE,
        TRUETEST_CXX_COMPILER,
        truetest::target_name(),
        truetest::target_allows_live_orders(),
    };
}

void metadata_row(const char* label, std::string_view value)
{
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextColored(theme::tx_lo(), "%s", label);
    ImGui::TableNextColumn();
    const auto display = build_value_or_unknown(value);
    ImGui::TextUnformatted(display.data(), display.data() + display.size());
}

} // namespace

void draw_about_dialog(bool& open)
{
    if (!open)
        return;

    ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("About TrueTest", &open,
                      ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::End();
        return;
    }

    const auto info = current_build_info();
    ImGui::TextColored(theme::accent(), "TrueTest %.*s",
                       static_cast<int>(build_value_or_unknown(info.version).size()),
                       build_value_or_unknown(info.version).data());
    ImGui::TextColored(theme::tx_faint(),
                       "Build identity below was captured at CMake configure time.");

    if (ImGui::BeginTable("build_identity", 2,
                          ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed, 170.0f);
        ImGui::TableSetupColumn("Value");
        metadata_row("Version", info.version);
        metadata_row("Git SHA at configure", info.git_sha);
        metadata_row("Worktree at configure", info.git_state_at_configure);
        metadata_row("Configured at (UTC)", info.configured_at_utc);
        metadata_row("Build type", info.build_type);
        metadata_row("Compiler", info.compiler);
        metadata_row("Binary target", info.target);
        metadata_row("Live orders compiled",
                     info.live_orders_compiled ? "yes" : "no");
        metadata_row("Dear ImGui", IMGUI_VERSION);
        metadata_row("ImPlot", IMPLOT_VERSION);
        ImGui::EndTable();
    }

    ImGui::SeparatorText("Compiled features — not runtime readiness");
    for (const auto& feature : compiled_features)
    {
        ImGui::TextColored(feature.enabled ? theme::up() : theme::tx_faint(),
                           "%s  %s", feature.enabled ? "ON " : "OFF", feature.name);
    }

    ImGui::Separator();
    if (ImGui::Button("Copy build identity"))
    {
        const auto identity = format_build_identity(info);
        ImGui::SetClipboardText(identity.c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("Close"))
        open = false;

    ImGui::End();
}

} // namespace truetest::ui::desk

#endif // HAS_IMGUI_DESK
