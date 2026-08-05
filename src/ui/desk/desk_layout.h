#pragma once

#ifdef HAS_IMGUI_DESK

#include "ui/desk/desk_layout_model.h"

namespace truetest::ui::desk {

void apply_desk_page_layout(unsigned int dockspace_id,
                            float width,
                            float height,
                            DeskPage page,
                            bool focus_primary = false);

void set_desk_layout_locked(unsigned int dockspace_id, bool locked);

} // namespace truetest::ui::desk

#endif // HAS_IMGUI_DESK
