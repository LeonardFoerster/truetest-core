#pragma once

#include <chrono>
#include <functional>

namespace truetest::ui {

// Operator-control hooks shared by the rich ncurses TUI and the ImGui desk.
// Wired from main.inc against public engine/provider APIs. Null hooks mean
// "action unavailable" — UI shows a toast / disabled control, never invents
// a control path.
//
// Safety: callers must confirm before flatten/kill. Halt remains terminal;
// nothing here clears halt_flag_.
struct operator_actions
{
    std::function<void()> pause_toggle;  // `p`
    std::function<bool()> pause_state;   // status indicator
    std::function<void()> flatten;       // `F` (confirm first)
    // `K` (confirm first). Returns false on timeout / failure.
    std::function<bool(std::chrono::milliseconds deadline)> kill;
};

} // namespace truetest::ui
