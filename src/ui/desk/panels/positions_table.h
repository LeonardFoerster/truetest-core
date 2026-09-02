#pragma once

#ifdef HAS_IMGUI_DESK

#include "ui/desk/desk_state.h"
#include "ui/desk/desk_view_model.h"

namespace truetest::ui::desk::panels {

void draw_positions_table(const CommandCenterViewModel& view, DeskState& state);

}  // namespace truetest::ui::desk::panels

#endif  // HAS_IMGUI_DESK
