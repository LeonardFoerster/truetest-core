#pragma once

#include "providers/transport.h"

#include <chrono>
#include <atomic>
#include <charconv>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

class ReplayTransport : public IDataTransport
{
public:
    explicit ReplayTransport(const std::string& file_path, bool pace = false)
        : path_(file_path)
        , pace_(pace)
    {}

    bool open() override
    {
        std::lock_guard<std::mutex> file_lock(file_mu_);
        in_.open(path_, std::ios::in);
        if (!in_.is_open())
            return false;

        open_.store(true, std::memory_order_release);
        stop_requested_.store(false, std::memory_order_release);
        clean_eof_.store(false, std::memory_order_release);
        failed_.store(false, std::memory_order_release);
        first_record_ts_ = 0;
        last_record_ts_ = 0;
        replay_start_ = std::chrono::steady_clock::now();
        return true;
    }

    void close() override
    {
        open_.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> file_lock(file_mu_);
        if (in_.is_open())
            in_.close();
    }

    bool is_open() const override
    {
        return open_.load(std::memory_order_acquire);
    }

    std::optional<std::string> read_line() override
    {
        return read_next(false);
    }

    bool is_streaming() const override
    {
        return pace_;
    }

    std::optional<std::string> read_line_blocking() override
    {
        return read_next(pace_);
    }

    void request_stop() override
    {
        stop_requested_.store(true, std::memory_order_release);
        open_.store(false, std::memory_order_release);
        pace_cv_.notify_all();
    }

    transport_terminal_status terminal_status() const override
    {
        if (stop_requested_.load(std::memory_order_acquire))
            return transport_terminal_status::operator_stop;
        if (clean_eof_.load(std::memory_order_acquire))
            return transport_terminal_status::clean_eof;
        if (failed_.load(std::memory_order_acquire))
            return transport_terminal_status::failed;
        return transport_terminal_status::unknown;
    }

private:
    std::string path_;
    bool pace_;
    std::ifstream in_;
    std::atomic<bool> open_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> clean_eof_{false};
    std::atomic<bool> failed_{false};
    std::mutex file_mu_;
    std::mutex pace_mu_;
    std::condition_variable pace_cv_;

    int64_t first_record_ts_ = 0;
    int64_t last_record_ts_ = 0;
    std::chrono::steady_clock::time_point replay_start_;

    std::optional<std::string> read_next(bool apply_pacing)
    {
        if (!open_.load(std::memory_order_acquire)
            || stop_requested_.load(std::memory_order_acquire))
            return std::nullopt;

        std::string line;
        {
            std::lock_guard<std::mutex> file_lock(file_mu_);
            if (!in_.is_open() || !std::getline(in_, line))
            {
                open_.store(false, std::memory_order_release);
                clean_eof_.store(true, std::memory_order_release);
                return std::nullopt;
            }
        }

        if (line.empty())
            return std::nullopt;

        auto tab_pos = line.find('\t');
        if (tab_pos == std::string::npos)
            return line;

        const std::string_view timestamp_text{line.data(), tab_pos};
        int64_t ts_ms = 0;
        const auto [end, error] = std::from_chars(
            timestamp_text.data(),
            timestamp_text.data() + timestamp_text.size(), ts_ms);
        if (error != std::errc{}
            || end != timestamp_text.data() + timestamp_text.size()
            || ts_ms <= 0 || (last_record_ts_ != 0 && ts_ms < last_record_ts_))
        {
            failed_.store(true, std::memory_order_release);
            open_.store(false, std::memory_order_release);
            return std::nullopt;
        }
        last_record_ts_ = ts_ms;
        std::string payload = line.substr(tab_pos + 1);

        if (apply_pacing && ts_ms > 0)
        {
            if (first_record_ts_ == 0)
            {
                first_record_ts_ = ts_ms;
                replay_start_ = std::chrono::steady_clock::now();
            }

            auto offset_ms = ts_ms - first_record_ts_;
            auto target = replay_start_ + std::chrono::milliseconds(offset_ms);
            auto now = std::chrono::steady_clock::now();

            if (target > now)
            {
                std::unique_lock<std::mutex> pace_lock(pace_mu_);
                if (pace_cv_.wait_until(pace_lock, target, [this] {
                        return stop_requested_.load(std::memory_order_acquire);
                    }))
                    return std::nullopt;
            }
        }

        return payload;
    }
};
