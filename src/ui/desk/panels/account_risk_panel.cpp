#ifdef HAS_IMGUI_DESK

#include "ui/desk/panels/account_risk_panel.h"

#include "ui/desk/desk_format.h"
#include "ui/desk/desk_theme.h"
#include "ui/desk/widgets/numeric_text.h"

#include "imgui.h"

namespace truetest::ui::desk::panels {
namespace {

void row(const char* label, const std::string& value, ImVec4 color = theme::text())
{
    ImGui::TextColored(theme::text_faint(), "%s", label);
    ImGui::SameLine(125.0f);
    widgets::mono_text(value.c_str(), color);
}

std::string configured_limit(double value)
{
    if (value <= 0.0) return "NO LIMIT CONFIGURED";
    return format_money(std::optional<double>{value});
}

}  // namespace

void draw_account_risk_panel(const dashboard_snapshot& snapshot, const AccountView& account)
{
    ImGui::TextColored(theme::text(), "ACCOUNT / RISK");
    ImGui::Separator();
    ImGui::TextColored(theme::text_faint(), "ACCOUNT");
    row("Cash", format_money(std::optional<double>{account.cash}));
    row("Equity", format_money(account.equity));
    row("Initial", format_money(std::optional<double>{account.initial_balance}));
    row("Total PnL", format_money(account.total_pnl, true),
        account.total_pnl ? widgets::pnl_color(*account.total_pnl) : theme::warning());
    row("Realized PnL", format_money(account.realized_pnl, true),
        account.realized_pnl ? widgets::pnl_color(*account.realized_pnl) : theme::warning());
    row("Unrealized", format_money(account.unrealized_pnl, true),
        account.unrealized_pnl ? widgets::pnl_color(*account.unrealized_pnl) : theme::warning());

    ImGui::Spacing();
    ImGui::TextColored(theme::text_faint(), "RISK");
    row("Gross exposure", format_money(account.gross_exposure));
    row("Exposure limit", configured_limit(snapshot.risk.exposure_limit));
    row("Current DD", format_percent(account.current_drawdown_pct));
    const std::string dd_limit =
        snapshot.risk.max_drawdown_limit > 0.0
            ? format_percent(std::optional<double>{snapshot.risk.max_drawdown_limit})
            : "NO LIMIT CONFIGURED";
    row("DD limit", dd_limit);
    row("Max DD", format_percent(account.max_drawdown_pct));
    row("Daily loss", snapshot.risk.daily_loss_available
                          ? format_money(std::optional<double>{snapshot.risk.daily_loss}, true)
                          : "UNAVAILABLE");
    row("Daily limit", configured_limit(snapshot.risk.daily_loss_limit));
    const std::string orders_limit = snapshot.risk.open_orders_limit > 0
                                         ? std::to_string(snapshot.risk.open_orders_limit)
                                         : "NO LIMIT CONFIGURED";
    row("Working orders", std::to_string(snapshot.risk.open_orders) + " / " + orders_limit);

    ImGui::Spacing();
    if (snapshot.risk.halted)
        ImGui::TextColored(theme::negative(), "HALTED — PROCESS RESTART REQUIRED");
    else
        ImGui::TextColored(theme::text_faint(),
                           "No direct venue or engine controls in this panel.");
}

}  // namespace truetest::ui::desk::panels

#endif  // HAS_IMGUI_DESK
