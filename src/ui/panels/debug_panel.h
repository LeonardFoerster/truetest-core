#pragma once
#ifdef HAS_RICH_TUI

#include "panel.h"

namespace truetest::ui {

class DebugPanel : public IPanel
{
public:
    const char* title() const override { return "Debug"; }
    void draw(int body_y0, int width, int height,
              const ConsoleDashboard& data,
              const dashboard_snapshot* snap) override;

    // Debug introspection (engine state, ring HWM, memory map) is dev-
    // facing and slow-moving. 1 s refresh is plenty.
    std::chrono::milliseconds min_refresh_interval() const override
    {
        return std::chrono::milliseconds(1000);
    }
};

}

#endif // HAS_RICH_TUI
