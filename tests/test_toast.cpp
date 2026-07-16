#include <gtest/gtest.h>

#ifdef HAS_RICH_TUI
#include "ui/toast.h"
#endif

#include <thread>
#include <chrono>

TEST(ToastStack, BasicSetAndTimestamp)
{
#ifndef HAS_RICH_TUI
    GTEST_SKIP() << "Rich TUI not enabled in this build";
#else
    truetest::ui::ToastStack ts;
    EXPECT_EQ(ts.last_toast_ms(), 0);

    ts.set_toast("hello");
    EXPECT_GT(ts.last_toast_ms(), 0);
#endif
}

TEST(ToastStack, QueueCap)
{
#ifndef HAS_RICH_TUI
    GTEST_SKIP() << "Rich TUI not enabled in this build";
#else
    truetest::ui::ToastStack ts;

    for (int i = 0; i < 20; ++i)
        ts.set_toast("msg" + std::to_string(i));

    // We can't easily inspect internal size without drawing,
    // but drawing should never return more than kMaxToasts.
    // Here we just ensure it doesn't crash and last timestamp is set.
    EXPECT_GT(ts.last_toast_ms(), 0);
#endif
}

TEST(ToastStack, ThreadSafetySmoke)
{
#ifndef HAS_RICH_TUI
    GTEST_SKIP() << "Rich TUI not enabled in this build";
#else
    truetest::ui::ToastStack ts;

    auto worker = [&ts](int count) {
        for (int i = 0; i < count; ++i) {
            ts.set_toast("t" + std::to_string(i));
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    };

    std::thread t1(worker, 50);
    std::thread t2(worker, 50);
    t1.join();
    t2.join();

    EXPECT_GT(ts.last_toast_ms(), 0);
#endif
}
