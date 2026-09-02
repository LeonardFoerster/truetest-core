#pragma once

#ifdef HAS_IMGUI_DESK

namespace truetest::ui::desk::widgets {

enum class ConfirmKind
{
    none,
    flatten,
    kill
};
enum class ConfirmResult
{
    none,
    confirmed,
    cancelled
};

ConfirmResult draw_confirm_modal(ConfirmKind kind);

}  // namespace truetest::ui::desk::widgets

#endif  // HAS_IMGUI_DESK
