#include <gtest/gtest.h>

#ifdef HAS_RICH_TUI
#include "ui/overlays.h"
#endif

TEST(Overlays, ConfirmKindValues)
{
#ifndef HAS_RICH_TUI
    GTEST_SKIP() << "Rich TUI not enabled in this build";
#else
    // Just verify the enum values are stable (used for casting in TabbedDashboard)
    EXPECT_EQ(static_cast<int>(truetest::ui::ConfirmKind::none), 0);
    EXPECT_EQ(static_cast<int>(truetest::ui::ConfirmKind::flatten), 1);
    EXPECT_EQ(static_cast<int>(truetest::ui::ConfirmKind::kill), 2);
#endif
}

TEST(Overlays, PaintFunctionsDoNotCrashWhenInvisible)
{
#ifndef HAS_RICH_TUI
    GTEST_SKIP() << "Rich TUI not enabled in this build";
#else
    // These functions are pure drawing. Calling them with "nothing to draw"
    // should be safe (they early-return).
    // We cannot easily assert ncurses output here without a real terminal,
    // so we just ensure they don't segfault or throw.
    EXPECT_NO_THROW(truetest::ui::paint_confirm_overlay(80, 24, truetest::ui::ConfirmKind::none));
    EXPECT_NO_THROW(truetest::ui::paint_help_overlay(80, 24, false));
#endif
}
