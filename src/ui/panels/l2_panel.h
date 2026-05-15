#pragma once
#ifdef HAS_RICH_TUI

#include "panel.h"

namespace truetest::ui {

class L2Panel : public IPanel
{
public:
    const char* title() const override { return "L2"; }
    void draw(int body_y0, int width, int height,
              const ConsoleDashboard& data,
              const dashboard_snapshot* snap) override;
};

}

#endif // HAS_RICH_TUI
