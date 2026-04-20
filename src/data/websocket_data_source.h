#pragma once
#ifdef HAS_LIVE_DATA

#include "../core/event.h"
#include "../utils/retry.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

class WebSocketDataSource
{
public:
    using event_callback = std::function<void(const market_event&)>;

    struct config
    {
        std::string endpoint_url;
        std::string auth_token;
        bool binary_format = false;

        std::chrono::seconds initial_backoff{1};
        std::chrono::seconds max_backoff{30};

        std::chrono::seconds ping_interval{15};
    };

    explicit WebSocketDataSource(config cfg);
    ~WebSocketDataSource();

    void start(event_callback cb);

    void stop();

    bool is_connected() const { return connected_.load(std::memory_order_acquire); }
    uint64_t get_sequence_number() const { return last_seq_.load(std::memory_order_relaxed); }
    uint64_t get_gap_count() const { return gap_count_.load(std::memory_order_relaxed); }

private:
    void io_thread_main();
    void connect();
    void on_message(const std::string& payload);
    void schedule_reconnect();

    config config_;
    event_callback callback_;

    std::thread io_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<uint64_t> last_seq_{0};
    std::atomic<uint64_t> gap_count_{0};

    std::chrono::seconds current_backoff_;
};

#endif // HAS_LIVE_DATA
