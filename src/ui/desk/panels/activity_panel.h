#pragma once

#ifdef HAS_IMGUI_DESK

#include "ui/dashboard_snapshot.h"
#include "ui/desk/desk_context.h"
#include "ui/desk/desk_window_names.h"

namespace truetest::ui::desk::panels {

void draw_activity_panel(DeskPanel panel,
                         const dashboard_snapshot* snap,
                         DeskDensity density,
                         const char* symbol_filter = nullptr);

} // namespace truetest::ui::desk::panels

#endif
