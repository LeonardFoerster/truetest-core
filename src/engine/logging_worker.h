#pragma once

#include "../threading/worker.h"
#include "../core/event_log.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>

struct durable_log_ack
{
    event_type type = event_type::market;
    std::uint64_t entity_id = 0;
    std::uint64_t completed_steady_ns = 0;
};

static_assert(std::is_trivially_copyable_v<durable_log_ack>);

// Reverse SPSC protocol for the reserved mainnet ledger.  LoggingWorker is
// the sole producer; the engine event loop is the sole consumer.  The exact
// tuple proves that a particular order record completed EventLogger::log(),
// whose reserved-ledger order path includes flush + fsync + inode/path checks.
class DurableLogProtocol final
{
private:
    struct State final
    {
        // One live order waits synchronously before the next can be admitted.
        // Capacity greater than one is defensive headroom, not batching.
        RingBuffer<durable_log_ack, 8, AssertFull> acknowledgements;
        alignas(64) std::atomic<bool> compromised{false};
    };

    [[nodiscard]] static State& require_state(
        const std::shared_ptr<State>& state) noexcept
    {
        if (!state) std::terminate();
        return *state;
    }

public:
    class ProducerEndpoint final
    {
        friend class DurableLogProtocol;

        explicit ProducerEndpoint(std::shared_ptr<State> state) noexcept
            : state_(std::move(state)) {}

    public:
        ProducerEndpoint(const ProducerEndpoint&) = delete;
        ProducerEndpoint& operator=(const ProducerEndpoint&) = delete;
        ProducerEndpoint(ProducerEndpoint&&) noexcept = default;
        ProducerEndpoint& operator=(ProducerEndpoint&&) noexcept = default;

        bool publish_ack(durable_log_ack ack) noexcept
        {
            auto& state = require_state(state_);
            if (state.compromised.load(std::memory_order_acquire) ||
                !state.acknowledgements.try_push(ack))
            {
                compromise();
                return false;
            }
            return true;
        }

        void compromise() noexcept
        {
            require_state(state_).compromised.store(
                true, std::memory_order_release);
        }

        [[nodiscard]] bool compromised() const noexcept
        {
            return require_state(state_).compromised.load(
                std::memory_order_acquire);
        }

    private:
        std::shared_ptr<State> state_;
    };

    class ConsumerEndpoint final
    {
        friend class DurableLogProtocol;

        explicit ConsumerEndpoint(std::shared_ptr<State> state) noexcept
            : state_(std::move(state)) {}

    public:
        ConsumerEndpoint(const ConsumerEndpoint&) = delete;
        ConsumerEndpoint& operator=(const ConsumerEndpoint&) = delete;
        ConsumerEndpoint(ConsumerEndpoint&&) noexcept = default;
        ConsumerEndpoint& operator=(ConsumerEndpoint&&) noexcept = default;

        bool try_take_ack(durable_log_ack& ack) noexcept
        {
            return require_state(state_).acknowledgements.try_pop(ack);
        }

        void compromise() noexcept
        {
            require_state(state_).compromised.store(
                true, std::memory_order_release);
        }

        [[nodiscard]] bool compromised() const noexcept
        {
            return require_state(state_).compromised.load(
                std::memory_order_acquire);
        }

        [[nodiscard]] bool acknowledgements_empty() const noexcept
        {
            return require_state(state_).acknowledgements.empty();
        }

    private:
        std::shared_ptr<State> state_;
    };

    struct EndpointPair final
    {
        ProducerEndpoint producer;
        ConsumerEndpoint consumer;
    };

    [[nodiscard]] static EndpointPair make_endpoints()
    {
        auto state = std::make_shared<State>();
        return {ProducerEndpoint{state}, ConsumerEndpoint{std::move(state)}};
    }

    DurableLogProtocol() = delete;
};

class LoggingWorker : public Worker
{
public:
    enum class log_sink { none, stdout_sink, file_sink };

    explicit LoggingWorker(const std::string& event_log_path = "",
                           log_sink text_sink = log_sink::none,
                           const std::string& text_log_path = "",
                           bool compress_log = true,
                           std::uint64_t max_bytes = 0,
                           int max_files = 5,
                           std::shared_ptr<DurableEventLogReservation>
                               event_log_reservation = {},
                           std::unique_ptr<EventLogger>
                               preopened_event_logger = {},
                           std::optional<DurableLogProtocol::ProducerEndpoint>
                               durable_log_producer = std::nullopt)
        : durable_log_producer_(std::move(durable_log_producer))
        , text_sink_(text_sink)
        , text_log_path_(text_log_path)
        , text_max_bytes_(max_bytes)
        , text_max_files_(max_files)
    {
        if (preopened_event_logger)
        {
            if (event_log_path.empty() ||
                preopened_event_logger->logical_path() != event_log_path)
                throw std::runtime_error(
                    "LoggingWorker: preopened event logger path mismatch");
            event_logger_ = std::move(preopened_event_logger);
        }
        else if (!event_log_path.empty())
            event_logger_ = std::make_unique<EventLogger>(
                event_log_path, compress_log, max_bytes, max_files,
                std::move(event_log_reservation));

        if (durable_log_producer_ &&
            (!event_logger_ || !event_logger_->has_durable_reservation()))
            throw std::runtime_error(
                "LoggingWorker: durable ACK protocol requires the reserved logger");

        if (text_sink_ == log_sink::file_sink && !text_log_path.empty())
            text_file_.open(text_log_path, std::ios::out | std::ios::trunc);
    }

    ~LoggingWorker() noexcept
    {
        // Binary finalization is always explicit: only engine shutdown can
        // prove that producers are quiesced and the inbound ring is empty.
        // If that proof was skipped, keep the prefix diagnostic-only.
        try { flush_batch(); }
        catch (...) {}
        if (event_logger_)
            event_logger_->abandon();
    }

    const char* worker_name() const override { return "logging"; }

    void on_event(const event_pointer& ev) override
    {
        try
        {
            if (!ev)
                throw std::runtime_error(
                    "LoggingWorker: null event in logging ring");

            if (event_logger_)
                event_logger_->log(*ev);

            if (text_sink_ != log_sink::none)
            {
                batch_buffer_ << format_event(*ev) << '\n';
                ++batch_count_;
                if (batch_count_ >= BATCH_SIZE)
                    flush_batch();
            }

            if (durable_log_producer_ &&
                ev->get_type() == event_type::order)
            {
                const auto* order = dynamic_cast<const order_event*>(ev.get());
                if (!order)
                    throw std::runtime_error(
                        "LoggingWorker: order tag/dynamic type mismatch");
                const auto completed_ns = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
                if (!durable_log_producer_->publish_ack(
                        {event_type::order, order->get_order_id(),
                         completed_ns}))
                    throw std::runtime_error(
                        "LoggingWorker: durable ACK ring overflow or compromise");
            }

            // "Processed" means binary/text work and any mandatory ACK all
            // succeeded; it is never advanced before persistence.
            events_processed_.fetch_add(1, std::memory_order_relaxed);
        }
        catch (...)
        {
            if (durable_log_producer_)
                durable_log_producer_->compromise();
            if (event_logger_)
                event_logger_->abandon();
            throw;
        }
    }

    std::size_t events_processed() const
    {
        return events_processed_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool has_binary_logger() const noexcept
    {
        return static_cast<bool>(event_logger_);
    }

    [[nodiscard]] bool has_reserved_logger() const noexcept
    {
        return event_logger_ && event_logger_->has_durable_reservation();
    }

    // Call after the worker thread has joined. Unlike destructor-only
    // finalization, this gives the engine a synchronous failure boundary for
    // the durable trailer/flush and lets it latch a terminal halt.
    void finalize()
    {
        try
        {
            if (durable_log_producer_ &&
                durable_log_producer_->compromised())
                throw std::runtime_error(
                    "LoggingWorker: refusing to finalize a compromised ledger");
            flush_batch();
            if (event_logger_)
                event_logger_->finalize();
        }
        catch (...)
        {
            if (durable_log_producer_)
                durable_log_producer_->compromise();
            if (event_logger_)
                event_logger_->abandon();
            throw;
        }
    }

    void abandon() noexcept
    {
        if (durable_log_producer_)
            durable_log_producer_->compromise();
        if (event_logger_)
            event_logger_->abandon();
    }

private:
    std::unique_ptr<EventLogger> event_logger_;
    std::optional<DurableLogProtocol::ProducerEndpoint>
        durable_log_producer_;

    log_sink text_sink_ = log_sink::none;
    std::string text_log_path_;
    std::uint64_t text_max_bytes_ = 0;
    int text_max_files_ = 5;
    std::ofstream text_file_;
    std::ostringstream batch_buffer_;
    std::size_t batch_count_ = 0;
    static constexpr std::size_t BATCH_SIZE = 100;

    std::atomic<std::size_t> events_processed_{0};

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
