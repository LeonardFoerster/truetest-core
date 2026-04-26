#pragma once
#ifdef HAS_RICH_TUI

namespace truetest::ui {

class ConsoleDashboard;
struct dashboard_snapshot;

// One tab. draw() is called from the render thread at the dashboard tick;
// implementations must read from the supplied data without mutating it.
// `body_y0` is the first writable row inside the tab body (below the
// chrome); `height` is the body height in rows.
class IPanel
{
public:
    virtual ~IPanel() = default;

    virtual const char* title() const = 0;

    virtual void draw(int body_y0, int width, int height,
                      const ConsoleDashboard& data,
                      const dashboard_snapshot* snap) = 0;
};

} // namespace truetest::ui

#endif // HAS_RICH_TUI
