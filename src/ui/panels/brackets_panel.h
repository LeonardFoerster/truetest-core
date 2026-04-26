#pragma once
#ifdef HAS_RICH_TUI

#include "panel.h"

namespace truetest::ui {

class BracketsPanel : public IPanel
{
public:
    const char* title() const override { return "Brackets"; }
    void draw(int body_y0, int width, int height,
              const ConsoleDashboard& data,
              const dashboard_snapshot* snap) override;

    // Cursor + filter — proof-of-concept; other panels can adopt the
    // same pattern.
    bool supports_cursor() const override { return true; }
    void set_cursor_row(int r) override { cursor_ = r; }
    int  cursor_row() const override    { return cursor_; }
    int  cursor_max() const override    { return cursor_max_; }
    void set_filter(const std::string& s) override { filter_ = s; }

private:
    int cursor_ = 0;
    int cursor_max_ = 0;       // updated in draw() after filtering
    std::string filter_;
};

} // namespace truetest::ui

#endif // HAS_RICH_TUI
