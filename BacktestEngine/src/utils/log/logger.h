#pragma once


#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>
#include <string>

namespace tt_log {

enum class level : int { info = 0, warn = 1, error = 2 };

inline const char* level_str(level l)
{
    switch (l) {
    case level::info:  return "INFO";
    case level::warn:  return "WARN";
    case level::error: return "ERROR";
    }
    return "?";
}

class Logger
{
public:
    static Logger& instance()
    {
        static Logger inst;
        return inst;
    }

    void set_file(const std::string& path)
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (file_.is_open()) file_.close();
        if (!path.empty())
            file_.open(path, std::ios::out | std::ios::app);
    }

    void set_min_level(level l)
    {
        min_level_.store(static_cast<int>(l), std::memory_order_relaxed);
    }

    bool enabled(level l) const
    {
        return static_cast<int>(l) >= min_level_.load(std::memory_order_relaxed);
    }

    void log(level l, const char* component, const char* fmt, ...)
    {
        if (!enabled(l)) return;

        char msg[2048];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(msg, sizeof(msg), fmt, ap);
        va_end(ap);

        char ts[64];
        format_timestamp(ts, sizeof(ts));

        char line[2400];
        std::snprintf(line, sizeof(line),
                      "[%s] [%s] [%s] %s\n",
                      ts, level_str(l),
                      component ? component : "-",
                      msg);

        std::lock_guard<std::mutex> lk(mu_);
        if (file_.is_open()) {
            file_ << line;
            file_.flush();
        } else {
            std::fputs(line, stderr);
        }
    }

private:
    Logger() = default;
    ~Logger() { if (file_.is_open()) file_.close(); }
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    static void format_timestamp(char* buf, std::size_t n)
    {
        using namespace std::chrono;
        auto now = system_clock::now();
        auto us = duration_cast<microseconds>(now.time_since_epoch()).count();
        std::time_t t = static_cast<std::time_t>(us / 1000000);
        int ms = static_cast<int>((us / 1000) % 1000);
        std::tm tm{};
#if defined(_WIN32)
        gmtime_s(&tm, &t);
#else
        gmtime_r(&t, &tm);
#endif
        std::snprintf(buf, n, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                      tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                      tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
    }

    std::mutex mu_;
    std::ofstream file_;
    std::atomic<int> min_level_{static_cast<int>(level::info)};
};

} // namespace tt_log

#define LOG_INFO(component, ...)  ::tt_log::Logger::instance().log(::tt_log::level::info,  component, __VA_ARGS__)
#define LOG_WARN(component, ...)  ::tt_log::Logger::instance().log(::tt_log::level::warn,  component, __VA_ARGS__)
#define LOG_ERROR(component, ...) ::tt_log::Logger::instance().log(::tt_log::level::error, component, __VA_ARGS__)
