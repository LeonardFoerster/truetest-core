#include <gtest/gtest.h>

#ifdef HAS_RICH_TUI
#include "ui/tui_prefs.h"
#endif

#include <filesystem>
#include <fstream>

TEST(TuiPrefs, DefaultsWhenNoFile)
{
#ifndef HAS_RICH_TUI
    GTEST_SKIP() << "Rich TUI not enabled in this build";
#else
    // load_tui_prefs should always succeed and return defaults when no file
    auto p = truetest::ui::load_tui_prefs();
    EXPECT_EQ(p.active_tab, -1);
    EXPECT_EQ(p.theme, -1);
    EXPECT_EQ(p.frozen, -1);
#endif
}

TEST(TuiPrefs, Roundtrip)
{
#ifndef HAS_RICH_TUI
    GTEST_SKIP() << "Rich TUI not enabled in this build";
#else
    truetest::ui::TuiPrefs in;
    in.active_tab = 3;
    in.theme = 1;
    in.frozen = 1;

    truetest::ui::save_tui_prefs(in);
    auto out = truetest::ui::load_tui_prefs();

    EXPECT_EQ(out.active_tab, 3);
    EXPECT_EQ(out.theme, 1);
    EXPECT_EQ(out.frozen, 1);
#endif
}

TEST(TuiPrefs, PathIsResolvable)
{
#ifndef HAS_RICH_TUI
    GTEST_SKIP() << "Rich TUI not enabled in this build";
#else
    std::string p = truetest::ui::tui_prefs_path();
    // On most systems this will be non-empty (under $HOME or XDG)
    // We don't assert a specific value, just that the function doesn't crash
    (void)p;
#endif
}
