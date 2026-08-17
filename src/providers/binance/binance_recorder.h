#pragma once

#include "providers/transport.h"

#include <chrono>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

class RecordingTransport : public IDataTransport
{
public:
    RecordingTransport(std::shared_ptr<IDataTransport> inner, const std::string& output_path)
        : inner_(std::move(inner))
        , out_(output_path, std::ios::out | std::ios::trunc)
    {
        if (!out_.is_open())
            throw std::runtime_error("RecordingTransport: cannot open " + output_path);
    }

    ~RecordingTransport()
    {
        if (out_.is_open())
            out_.close();
    }

    bool open() override
    {
        return inner_->open();
    }

    void close() override
    {
        inner_->close();
        std::lock_guard<std::mutex> lk(mu_);
        if (out_.is_open())
            out_.flush();
    }

    bool is_open() const override
    {
        return inner_->is_open();
    }

    std::optional<std::string> read_line() override
    {
        auto line = inner_->read_line();
        if (line)
            record(*line);
        return line;
    }

    bool is_streaming() const override
    {
        return inner_->is_streaming();
    }

    transport_terminal_status terminal_status() const override
    {
        return inner_->terminal_status();
    }

    std::optional<std::string> read_line_blocking() override
    {
        auto line = inner_->read_line_blocking();
        if (line)
            record(*line);
        return line;
    }

    void request_stop() override
    {
        inner_->request_stop();
    }

    bool supports_bounded_idle_read() const override
    {
        return inner_->supports_bounded_idle_read();
    }

    transport_read_result read_frame_until(
        std::string_view& out,
        std::chrono::steady_clock::time_point deadline) override
    {
        const auto result = inner_->read_frame_until(out, deadline);
        if (result == transport_read_result::frame)
            record(out);
        return result;
    }

private:
    std::shared_ptr<IDataTransport> inner_;
    std::ofstream out_;
    std::mutex mu_;

    void record(std::string_view msg)
    {
        auto now = std::chrono::system_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();

        std::lock_guard<std::mutex> lk(mu_);
        out_ << ms << '\t' << msg << '\n';
        out_.flush();
    }
};
