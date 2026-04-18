#pragma once
#ifdef HAS_DEBUG

#include "absl/log/log.h"
#include "absl/log/initialize.h"
#include "absl/log/log_sink.h"
#include "absl/log/log_sink_registry.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_cat.h"

#include <fstream>
#include <mutex>
#include <string>

namespace debug {


inline void init()
{
    absl::InitializeLog();
}


class FileSink : public absl::LogSink
{
public:
    explicit FileSink(const std::string& path)
        : file_(path, std::ios::out | std::ios::trunc) {}

    void Send(const absl::LogEntry& entry) override
    {
        if (!file_.is_open()) return;
        std::lock_guard<std::mutex> lock(mu_);
        file_ << entry.text_message_with_prefix_and_newline();
        if (++count_ % 100 == 0) file_.flush();
    }

    void Flush() override
    {
        std::lock_guard<std::mutex> lock(mu_);
        file_.flush();
    }

private:
    std::ofstream file_;
    std::mutex mu_;
    uint64_t count_ = 0;
};

} // namespace debug


#define DBG_INFO(...)  LOG(INFO) << absl::StrFormat(__VA_ARGS__)
#define DBG_WARN(...)  LOG(WARNING) << absl::StrFormat(__VA_ARGS__)
#define DBG_PERF(...)  LOG(INFO) << "[PERF] " << absl::StrFormat(__VA_ARGS__)
#define DBG_HW(...)    LOG(INFO) << "[HW] " << absl::StrFormat(__VA_ARGS__)
#define DBG_MEM(...)   LOG(INFO) << "[MEM] " << absl::StrFormat(__VA_ARGS__)
#define DBG_COPY(...)  LOG(INFO) << "[COPY] " << absl::StrFormat(__VA_ARGS__)
#define DBG_RING(...)  LOG(INFO) << "[RING] " << absl::StrFormat(__VA_ARGS__)
#define DBG_THR(...)   LOG(INFO) << "[THREAD] " << absl::StrFormat(__VA_ARGS__)

#else

#define DBG_INFO(...)  ((void)0)
#define DBG_WARN(...)  ((void)0)
#define DBG_PERF(...)  ((void)0)
#define DBG_HW(...)    ((void)0)
#define DBG_MEM(...)   ((void)0)
#define DBG_COPY(...)  ((void)0)
#define DBG_RING(...)  ((void)0)
#define DBG_THR(...)   ((void)0)

#endif // HAS_DEBUG
