#pragma once
#ifdef HAS_RICH_TUI

#include <chrono>

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

    // Minimum interval between paints of this panel. The render loop
    // throttles per-panel: when the active tab's last paint is more
    // recent than this, the loop sleeps and skips re-drawing it.
    // Default = 100 ms (matches the dashboard tick). Slow-moving panels
    // (Health, Debug) override to 1 s; fast panels (L2, Overview)
    // keep the default.
    virtual std::chrono::milliseconds min_refresh_interval() const
    {
        return std::chrono::milliseconds(100);
    }

    // Per-panel selection state — drives j/k cursor + Enter drill-in.
    // Default false: panel doesn't own a row cursor. Panels that opt
    // in (Brackets, Orders, Positions) set the cursor row externally
    // via set_cursor_row and read it back in draw().
    virtual bool supports_cursor() const { return false; }
    virtual void set_cursor_row(int /*row*/) {}
    virtual int  cursor_row() const { return -1; }
    virtual int  cursor_max() const { return 0; }   // # of selectable rows
    virtual void set_filter(const std::string& /*needle*/) {}
};

}

#endif // HAS_RICH_TUI
