#pragma once
#ifdef HAS_RICH_TUI

#include <string>

namespace truetest::ui {

// Persisted TUI user preferences (survives across runs).
// Stored as a tiny JSON-ish file under ~/.config/truetest/tui.json (or $XDG).
struct TuiPrefs {
    int active_tab = -1;  // last active panel index
    int theme      = -1;  // enum value of TabbedDashboard::theme
    int frozen     = -1;  // 1 if UI was frozen on exit
};

// Resolve the prefs file path (empty if HOME/XDG not usable).
std::string tui_prefs_path();

// Load (best-effort; leaves defaults on any error).
TuiPrefs load_tui_prefs();

// Save (best-effort; silently ignores failures).
void save_tui_prefs(const TuiPrefs& p);

}

#endif // HAS_RICH_TUI
