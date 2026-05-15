#pragma once
#ifdef HAS_RICH_TUI

#include "panel.h"

namespace truetest::ui {

class StrategyPanel : public IPanel
{
public:
    const char* title() const override { return "Strategy"; }
    void draw(int body_y0, int width, int height,
              const ConsoleDashboard& data,
              const dashboard_snapshot* snap) override;
};

}

#endif // HAS_RICH_TUI
