#pragma once
#ifdef HAS_RICH_TUI

#include <cstddef>

namespace truetest::ui {

// Overlay drawing helpers for the rich tabbed dashboard.
// These are pure drawing functions; ownership of modal state stays with caller.

// Confirm overlay (y/n for destructive actions).
enum class ConfirmKind { none, flatten, kill };

void paint_confirm_overlay(int width, int height, ConfirmKind kind);

// Full-screen hotkey help overlay.
void paint_help_overlay(int width, int height, bool visible);

}

#endif // HAS_RICH_TUI
