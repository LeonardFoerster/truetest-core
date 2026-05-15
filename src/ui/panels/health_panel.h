#pragma once
#ifdef HAS_RICH_TUI

#include "panel.h"

namespace truetest::ui {

class HealthPanel : public IPanel
{
public:
    const char* title() const override { return "Health"; }
    void draw(int body_y0, int width, int height,
              const ConsoleDashboard& data,
              const dashboard_snapshot* snap) override;

    // Health metrics (latency, throughput, ring drops) move slowly
    // enough that a 1 s refresh feels live without burning render time.
    std::chrono::milliseconds min_refresh_interval() const override
    {
        return std::chrono::milliseconds(1000);
    }
};

}

#endif // HAS_RICH_TUI
