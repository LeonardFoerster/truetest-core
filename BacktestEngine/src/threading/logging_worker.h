#pragma once

#include "worker.h"
#include "../core/event_log.h"

#include <atomic>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

// Consumes events from the logging ring and writes them to configured sinks.
// Supports binary event logging (EventLogger) and/or structured text logging.
class LoggingWorker : public Worker
{
public:
    enum class log_sink { none, stdout_sink, file_sink };

    explicit LoggingWorker(const std::string& event_log_path = "",
                           log_sink text_sink = log_sink::none,
                           const std::string& text_log_path = "")
        : text_sink_(text_sink)
    {
        if (!event_log_path.empty())
            event_logger_ = std::make_unique<EventLogger>(event_log_path);

        if (text_sink_ == log_sink::file_sink && !text_log_path.empty())
            text_file_.open(text_log_path, std::ios::out | std::ios::trunc);
    }

    ~LoggingWorker()
    {
        flush_batch();
        if (event_logger_)
            event_logger_->flush();
    }

    void on_event(const event_pointer& ev) override
    {
        events_processed_.fetch_add(1, std::memory_order_relaxed);

        // Binary event log
        if (event_logger_)
            event_logger_->log(*ev);

        // Text logging with batching
        if (text_sink_ != log_sink::none)
        {
            batch_buffer_ << format_event(*ev) << '\n';
            ++batch_count_;
            if (batch_count_ >= BATCH_SIZE)
                flush_batch();
        }
    }

    std::size_t events_processed() const
    {
        return events_processed_.load(std::memory_order_relaxed);
    }

private:
    std::unique_ptr<EventLogger> event_logger_;

    log_sink text_sink_ = log_sink::none;
    std::ofstream text_file_;
    std::ostringstream batch_buffer_;
    std::size_t batch_count_ = 0;
    static constexpr std::size_t BATCH_SIZE = 100;

    std::atomic<std::size_t> events_processed_{0};

    void flush_batch()
    {
        if (batch_count_ == 0)
            return;

        auto s = batch_buffer_.str();
        if (text_sink_ == log_sink::stdout_sink)
            std::cout << s << std::flush;
        else if (text_sink_ == log_sink::file_sink && text_file_.is_open())
            text_file_ << s << std::flush;

        batch_buffer_.str("");
        batch_buffer_.clear();
        batch_count_ = 0;
    }

    static std::string format_event(const event& ev)
    {
        switch (ev.get_type())
        {
        case event_type::market: {
            auto& m = static_cast<const market_event&>(ev);
            return "[MKT] " + m.get_symbol() +
                   " O=" + std::to_string(m.get_open()) +
                   " H=" + std::to_string(m.get_high()) +
                   " L=" + std::to_string(m.get_low()) +
                   " C=" + std::to_string(m.get_close()) +
                   " V=" + std::to_string(m.get_volume());
        }
        case event_type::order: {
            auto& o = static_cast<const order_event&>(ev);
            return "[ORD] " + o.get_symbol() +
                   " id=" + std::to_string(o.get_order_id()) +
                   " side=" + (o.get_side() == order_side::buy ? "BUY" : "SELL") +
                   " qty=" + std::to_string(o.get_quantity()) +
                   " px=" + std::to_string(o.get_price());
        }
        case event_type::fill: {
            auto& f = static_cast<const fill_event&>(ev);
            return "[FIL] " + f.get_symbol() +
                   " id=" + std::to_string(f.get_order_id()) +
                   " qty=" + std::to_string(f.get_filled_quantity()) +
                   " px=" + std::to_string(f.get_fill_price());
        }
        case event_type::tick: {
            auto& t = static_cast<const tick_event&>(ev);
            return "[TCK] " + t.get_symbol() +
                   " px=" + std::to_string(t.get_price()) +
                   " qty=" + std::to_string(t.get_quantity());
        }
        default:
            return "[EVT] type=" + std::to_string(static_cast<int>(ev.get_type()));
        }
    }
};
