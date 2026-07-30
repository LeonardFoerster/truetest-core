#ifdef HAS_RICH_TUI

#include "toast.h"

#include <ncurses.h>
#include <algorithm>
#include <cstring>
#include <chrono>

namespace truetest::ui {

void ToastStack::set_toast(const std::string& msg)
{
    const auto now_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();

    {
        std::lock_guard<std::mutex> lk(mu_);
        toasts_.insert(toasts_.begin(), Entry{msg, now_ms});
        if (toasts_.size() > kMaxToasts)
            toasts_.resize(kMaxToasts);
    }
    last_ms_.store(now_ms, std::memory_order_release);
}

long long ToastStack::last_toast_ms() const
{
    return last_ms_.load(std::memory_order_acquire);
}

int ToastStack::draw(int width, int height) const
{
    const auto now =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();

    std::vector<Entry> snap;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& t : toasts_)
            if ((now - t.ts_ms) <= 6000) snap.push_back(t);   // 6s ttl
    }
    if (snap.empty()) return 0;

    constexpr int kPairYellow = 3;
    constexpr int kPairWhite  = 5;

    // Stack from bottom-right upward, newest first.
    int yy = height - 2;     // one row above the footer hint
    int drawn = 0;
    for (std::size_t i = 0; i < snap.size() && yy >= 0; ++i, --yy)
    {
        const auto& t = snap[i];
        const auto age = now - t.ts_ms;
        const int x = std::max(1, width - static_cast<int>(t.msg.size()) - 4);
        if (i == 0)
        {
            // Newest: full color, reverse video.
            attron(COLOR_PAIR(kPairYellow) | A_REVERSE | A_BOLD);
            mvprintw(yy, x, " %s ", t.msg.c_str());
            attroff(COLOR_PAIR(kPairYellow) | A_REVERSE | A_BOLD);
        }
        else if (age < 4000)
        {
            // Mid-age: dimmer, no reverse.
            attron(COLOR_PAIR(kPairWhite) | A_DIM);
            mvprintw(yy, x, " %s ", t.msg.c_str());
            attroff(COLOR_PAIR(kPairWhite) | A_DIM);
        }
        else
        {
            // Old: very dim text only.
            attron(A_DIM);
            mvprintw(yy, x, " %s ", t.msg.c_str());
            attroff(A_DIM);
        }
        ++drawn;
    }
    return drawn;
}

}

#endif // HAS_RICH_TUI
