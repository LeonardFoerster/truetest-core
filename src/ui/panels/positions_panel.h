#pragma once
#ifdef HAS_RICH_TUI

#include "panel.h"

namespace truetest::ui {

class PositionsPanel : public IPanel
{
public:
    const char* title() const override { return "Positions"; }
    void draw(int body_y0, int width, int height,
              const ConsoleDashboard& data,
              const dashboard_snapshot* snap) override;

    bool supports_cursor() const override { return true; }
    void set_cursor_row(int row) override { cursor_row_ = std::max(0, row); }
    int  cursor_row() const override { return cursor_row_; }
    int  cursor_max() const override { return cursor_max_; }

    void set_filter(const std::string& needle) override { filter_ = needle; }

private:
    int cursor_row_ = 0;
    int cursor_max_ = 0;
    std::string filter_;
};

}

#endif // HAS_RICH_TUI
