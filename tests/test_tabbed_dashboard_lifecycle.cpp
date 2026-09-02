#include <gtest/gtest.h>

#ifdef HAS_RICH_TUI

#include "ui/console_dashboard.h"
#include "ui/dashboard_snapshot.h"
#include "ui/tabbed_dashboard.h"

#include <chrono>
#include <cerrno>
#include <fcntl.h>
#include <memory>
#include <pty.h>
#include <string>
#include <thread>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

struct child_result
{
    int status = -1;
    bool timed_out = false;
    std::chrono::milliseconds elapsed{};
    std::string terminal_output;
};

void drain_pty(int fd, std::string& output)
{
    char buffer[4096];
    for (;;)
    {
        const ssize_t n = ::read(fd, buffer, sizeof(buffer));
        if (n > 0)
        {
            output.append(buffer, static_cast<std::size_t>(n));
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        break;
    }
}

child_result run_dashboard_child()
{
    int master = -1;
    const auto started = std::chrono::steady_clock::now();
    const pid_t pid = ::forkpty(&master, nullptr, nullptr, nullptr);
    if (pid == 0)
    {
        ::setenv("TERM", "xterm-256color", 1);
        ::unsetenv("HOME");
        ::unsetenv("XDG_CONFIG_HOME");

        truetest::ui::dashboard_config config;
        config.mode = truetest::ui::output_mode::off;
        auto console = std::make_shared<truetest::ui::ConsoleDashboard>(config);
        const auto generated = std::chrono::steady_clock::now() -
                               std::chrono::seconds{5};
        truetest::ui::TabbedDashboard dashboard(
            std::move(console),
            [generated](truetest::ui::dashboard_snapshot& out) {
                out.generated_at = generated;
                out.generated_at_available = true;
                return true;
            },
            std::chrono::milliseconds{20});
        dashboard.start();
        std::this_thread::sleep_for(std::chrono::milliseconds{450});
        dashboard.stop();
        ::_exit(0);
    }

    child_result result;
    if (pid < 0) return result;

    const int old_flags = ::fcntl(master, F_GETFL, 0);
    if (old_flags >= 0) ::fcntl(master, F_SETFL, old_flags | O_NONBLOCK);

    // Let the first frame paint, then enter the filter prompt. stop() must
    // still synchronously join the prompt/render thread without detaching.
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    const char filter_key = '/';
    (void)::write(master, &filter_key, 1);

    const auto deadline = started + std::chrono::seconds{3};
    for (;;)
    {
        drain_pty(master, result.terminal_output);
        const pid_t waited = ::waitpid(pid, &result.status, WNOHANG);
        if (waited == pid) break;
        if (waited < 0) break;
        if (std::chrono::steady_clock::now() >= deadline)
        {
            result.timed_out = true;
            (void)::kill(pid, SIGKILL);
            (void)::waitpid(pid, &result.status, 0);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    drain_pty(master, result.terminal_output);
    ::close(master);
    result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    return result;
}

TEST(TabbedDashboardLifecycle, StopJoinsPromptThreadAndRendersProducerStaleness)
{
    const auto result = run_dashboard_child();
    ASSERT_FALSE(result.timed_out);
    ASSERT_TRUE(WIFEXITED(result.status));
    EXPECT_EQ(WEXITSTATUS(result.status), 0);
    EXPECT_LT(result.elapsed, std::chrono::milliseconds{1200})
        << "stop() appears to have waited for the former detach timeout";
    EXPECT_NE(result.terminal_output.find("stale"), std::string::npos)
        << "a repeatedly fetched five-second-old snapshot must remain stale";
}

}  // namespace

#endif  // HAS_RICH_TUI
