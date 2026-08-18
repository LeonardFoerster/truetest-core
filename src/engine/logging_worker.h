#pragma once

#include "../threading/worker.h"
#include "../core/event_log.h"

#include <atomic>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

class LoggingWorker : public Worker
{
public:
    enum class log_sink { none, stdout_sink, file_sink };

    explicit LoggingWorker(const std::string& event_log_path = "",
                           log_sink text_sink = log_sink::none,
                           const std::string& text_log_path = "",
                           bool compress_log = true,
                           std::uint64_t max_bytes = 0,
                           int max_files = 5)
        : text_sink_(text_sink)
        , text_log_path_(text_log_path)
        , text_max_bytes_(max_bytes)
        , text_max_files_(max_files)
    {
        if (!event_log_path.empty())
            event_logger_ = std::make_unique<EventLogger>(
                event_log_path, compress_log, max_bytes, max_files);

        if (text_sink_ == log_sink::file_sink && !text_log_path.empty())
            text_file_.open(text_log_path, std::ios::out | std::ios::trunc);
    }

    // Used only for the pre-reserved live ledger.  The engine creates this
    // worker before its initial private drain, so that drain is recorded by
    // the same logger that later consumes the logging ring—without a pathname
    // reopen or a second writer.
    explicit LoggingWorker(std::unique_ptr<EventLogger> authoritative_event_logger,
                           log_sink text_sink = log_sink::none,
                           const std::string& text_log_path = "",
                           std::uint64_t text_max_bytes = 0,
                           int text_max_files = 5)
        : event_logger_(std::move(authoritative_event_logger))
        , authoritative_event_log_(true)
        , text_sink_(text_sink)
        , text_log_path_(text_log_path)
        , text_max_bytes_(text_max_bytes)
        , text_max_files_(text_max_files)
    {
        if (!event_logger_)
            throw std::invalid_argument(
                "LoggingWorker requires an authoritative EventLogger");
        if (text_sink_ == log_sink::file_sink && !text_log_path.empty())
            text_file_.open(text_log_path, std::ios::out | std::ios::trunc);
    }

    ~LoggingWorker()
    {
        // Preserve the ordinary logger's existing destructor semantics.  A
        // secure ledger can deliberately be abandoned after a terminal
        // failure, where flush() correctly throws; suppress only that
        // teardown rethrow because engine::stop_workers already latched it.
        if (!authoritative_event_log_)
        {
            flush_batch();
            if (event_logger_)
                event_logger_->flush();
            return;
        }

        try
        {
            flush_batch();
            if (event_logger_)
                event_logger_->flush();
        }
        catch (...)
        {
            // Explicit live finalization is performed by engine::stop_workers
            // where failure can latch the terminal halt.  Destruction is only
            // a last-resort cleanup path and must not terminate the process.
        }
    }

    const char* worker_name() const override { return "logging"; }

    void on_event(const event_pointer& ev) override
    {
        write_event(*ev);
    }

    // Before the worker thread exists, a live engine can still need to drain
    // provider-private facts during construction.  Write those facts through
    // this exact secure logger, never through a second path-opened logger.
    // Once startup marks the worker active, all events must go through its
    // SPSC ring to preserve asynchronous hot-path behavior.
    void log_event_before_worker_start(const event& ev)
    {
        if (worker_started_.load(std::memory_order_acquire))
            throw std::logic_error(
                "LoggingWorker direct logging after worker startup");
        write_event(ev);
    }

    void mark_worker_started() noexcept
    {
        worker_started_.store(true, std::memory_order_release);
    }

    bool worker_started() const noexcept
    {
        return worker_started_.load(std::memory_order_acquire);
    }

    std::size_t events_processed() const
    {
        return events_processed_.load(std::memory_order_relaxed);
    }

    // Call only after the worker has been stopped and joined.  This preserves
    // the live ledger's ordering: final private accounting is enqueued before
    // worker shutdown, the worker drains its ring, then the one descriptor
    // owner writes the terminal index and atomically publishes the ledger.
    void finalize_event_log()
    {
        flush_batch();
        if (event_logger_)
            event_logger_->finalize();
    }

    // A dropped or failed authoritative logging record invalidates the
    // ledger's completeness proof.  It must remain a forensic `.partial`,
    // never be promoted merely because shutdown reached the normal boundary.
    void abandon_event_log() noexcept
    {
        if (event_logger_)
            event_logger_->abandon_unpublished();
    }

private:
    void write_event(const event& ev)
    {
        events_processed_.fetch_add(1, std::memory_order_relaxed);

        if (event_logger_)
            event_logger_->log(ev);

        if (text_sink_ != log_sink::none)
        {
            batch_buffer_ << format_event(ev) << '\n';
            ++batch_count_;
            if (batch_count_ >= BATCH_SIZE)
                flush_batch();
        }
    }
    std::unique_ptr<EventLogger> event_logger_;
    bool authoritative_event_log_ = false;

    log_sink text_sink_ = log_sink::none;
    std::string text_log_path_;
    std::uint64_t text_max_bytes_ = 0;
    int text_max_files_ = 5;
    std::ofstream text_file_;
    std::ostringstream batch_buffer_;
    std::size_t batch_count_ = 0;
    static constexpr std::size_t BATCH_SIZE = 100;

    std::atomic<std::size_t> events_processed_{0};
    std::atomic<bool> worker_started_{false};

    void rotate_text_if_needed()
    {
        if (text_max_bytes_ == 0 || text_log_path_.empty())
            return;
        if (!text_file_.is_open())
            return;
        auto pos = static_cast<std::uint64_t>(text_file_.tellp());
        if (pos < text_max_bytes_)
            return;

        text_file_.flush();
        text_file_.close();

        auto nth = [&](int i) { return text_log_path_ + "." + std::to_string(i); };
        if (text_max_files_ > 0) {
            std::remove(nth(text_max_files_).c_str());
            for (int i = text_max_files_ - 1; i >= 1; --i)
                std::rename(nth(i).c_str(), nth(i + 1).c_str());
            std::rename(text_log_path_.c_str(), nth(1).c_str());
        } else {
            std::remove(text_log_path_.c_str());
        }
        text_file_.open(text_log_path_, std::ios::out | std::ios::trunc);
    }

    void flush_batch()
    {
        if (batch_count_ == 0)
            return;

        auto s = batch_buffer_.str();
        if (text_sink_ == log_sink::stdout_sink)
            std::cout << s << std::flush;
        else if (text_sink_ == log_sink::file_sink && text_file_.is_open())
        {
            text_file_ << s << std::flush;
            rotate_text_if_needed();
        }

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
