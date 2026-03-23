#pragma once

#include "providers/transport.h"

#include <chrono>
#include <fstream>
#include <optional>
#include <string>
#include <thread>

// ReplayTransport: reads recorded WebSocket messages from a file and
// replays them as if they came from a live transport.
//
// File format (produced by RecordingTransport): <epoch_ms>\t<raw_json>\n
//
// In batch mode (default), is_streaming() returns false — all records are
// delivered immediately via read_line(). In paced mode, is_streaming()
// returns true and read_line_blocking() sleeps to simulate original timing.
class ReplayTransport : public IDataTransport
{
public:
    // pace: if true, simulate original arrival timing (streaming mode).
    //        if false, deliver all records immediately (batch mode for backtesting).
    explicit ReplayTransport(const std::string& file_path, bool pace = false)
        : path_(file_path)
        , pace_(pace)
    {}

    bool open() override
    {
        in_.open(path_, std::ios::in);
        if (!in_.is_open())
            return false;

        open_ = true;
        first_record_ts_ = 0;
        replay_start_ = std::chrono::steady_clock::now();
        return true;
    }

    void close() override
    {
        open_ = false;
        if (in_.is_open())
            in_.close();
    }

    bool is_open() const override
    {
        return open_;
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
        open_ = false;
        close();
    }

private:
    std::string path_;
    bool pace_;
    std::ifstream in_;
    bool open_ = false;

    int64_t first_record_ts_ = 0;
    std::chrono::steady_clock::time_point replay_start_;

    std::optional<std::string> read_next(bool apply_pacing)
    {
        if (!open_ || !in_.is_open())
            return std::nullopt;

        std::string line;
        if (!std::getline(in_, line))
        {
            open_ = false;
            return std::nullopt;
        }

        if (line.empty())
            return std::nullopt;

        // Parse: <epoch_ms>\t<raw_json>
        auto tab_pos = line.find('\t');
        if (tab_pos == std::string::npos)
            return line; // malformed, return raw

        int64_t ts_ms = std::stoll(line.substr(0, tab_pos));
        std::string payload = line.substr(tab_pos + 1);

        if (apply_pacing && ts_ms > 0)
        {
            if (first_record_ts_ == 0)
            {
                first_record_ts_ = ts_ms;
                replay_start_ = std::chrono::steady_clock::now();
            }

            // Calculate when this record should be delivered
            auto offset_ms = ts_ms - first_record_ts_;
            auto target = replay_start_ + std::chrono::milliseconds(offset_ms);
            auto now = std::chrono::steady_clock::now();

            if (target > now)
                std::this_thread::sleep_until(target);
        }

        return payload;
    }
};
