#pragma once
#ifdef HAS_RICH_TUI

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

namespace truetest::ui {

// Simple bounded toast stack for transient operator feedback messages.
// Newest on top. TTL ~6s inside draw. Thread-safe for set_toast from input thread.
class ToastStack {
public:
    static constexpr std::size_t kMaxToasts = 6;

    void set_toast(const std::string& msg);

    // Draw up to visible toasts in bottom-right. Caller must be in ncurses context.
    // Returns the number of toasts actually drawn (for layout hints if needed).
    int draw(int width, int height) const;

    // Timestamp (ms since epoch) of the most recent toast, for frame-skip decisions.
    long long last_toast_ms() const;

private:
    struct Entry {
        std::string msg;
        long long   ts_ms = 0;
    };

    mutable std::mutex mu_;
    std::vector<Entry> toasts_;   // newest first
    std::atomic<long long> last_ms_{0};
};

}

#endif // HAS_RICH_TUI
