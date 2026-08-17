#pragma once

#include "providers/transport.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/bind_allocator.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/error.hpp>

#include <array>
#include <cstddef>
#include <chrono>
#include <memory>
#include <new>
#include <string_view>

namespace provider_ws
{

class fixed_handler_arena
{
public:
    static constexpr std::size_t capacity = 128 * 1024;

    void* allocate(std::size_t bytes, std::size_t alignment)
    {
        void* candidate = storage_.data() + offset_;
        auto remaining = storage_.size() - offset_;
        void* aligned = std::align(alignment, bytes, candidate, remaining);
        if (!aligned) throw std::bad_alloc{};
        offset_ = static_cast<std::byte*>(aligned) - storage_.data() + bytes;
        ++outstanding_;
        return aligned;
    }

    void deallocate() noexcept
    {
        if (outstanding_ != 0 && --outstanding_ == 0) offset_ = 0;
    }

private:
    alignas(std::max_align_t) std::array<std::byte, capacity> storage_{};
    std::size_t offset_ = 0;
    std::size_t outstanding_ = 0;
};

template<class T>
class fixed_handler_allocator
{
public:
    using value_type = T;

    fixed_handler_allocator() noexcept = default;
    explicit fixed_handler_allocator(fixed_handler_arena* arena) noexcept
        : arena_(arena) {}

    template<class U>
    fixed_handler_allocator(
        const fixed_handler_allocator<U>& other) noexcept
        : arena_(other.arena()) {}

    T* allocate(std::size_t count)
    {
        if (!arena_) throw std::bad_alloc{};
        return static_cast<T*>(arena_->allocate(
            count * sizeof(T), alignof(T)));
    }

    void deallocate(T*, std::size_t) noexcept
    {
        if (arena_) arena_->deallocate();
    }

    fixed_handler_arena* arena() const noexcept { return arena_; }

    template<class U>
    bool operator==(const fixed_handler_allocator<U>& other) const noexcept
    {
        return arena_ == other.arena();
    }

private:
    fixed_handler_arena* arena_ = nullptr;
};

// One read owner, one outstanding websocket read. The composed async_read
// remains pending across idle ticks, so a partial TLS/WebSocket frame never
// turns the engine-thread deadline into a synchronous block.
template<class WebSocket>
class BoundedFrameReader
{
public:
    static constexpr std::size_t frame_capacity = 256 * 1024;

    BoundedFrameReader() : state_(std::make_shared<State>()) {}

    void reset()
    {
        if (state_->pending)
            state_ = std::make_shared<State>();
        else
            state_->reset();
    }

    transport_read_result read_until(
        const std::shared_ptr<boost::asio::io_context>& ioc,
        const std::shared_ptr<WebSocket>& websocket,
        std::string_view& out,
        std::chrono::steady_clock::time_point deadline) noexcept
    {
        if (!ioc || !websocket) return transport_read_result::terminal;
        const auto state = state_;
        try
        {
            if (!state->pending && !state->completed)
            {
                state->clear_buffer();
                state->pending = true;
                websocket->async_read(state->buffer,
                    boost::asio::bind_allocator(
                    fixed_handler_allocator<std::byte>{&state->handler_arena},
                    [state, websocket, ioc](boost::beast::error_code ec,
                                             std::size_t) noexcept {
                        state->error = ec;
                        state->pending = false;
                        state->completed = true;
                    }));
            }

            while (!state->completed)
            {
                if (std::chrono::steady_clock::now() >= deadline)
                    return transport_read_result::idle;
                ioc->restart();
                (void)ioc->run_until(deadline);
            }

            state->last_error = state->error;
            state->completed = false;
            if (state->last_error)
                return transport_read_result::terminal;

            const auto data = state->buffer.data();
            out = {static_cast<const char*>(data.data()), data.size()};
            return transport_read_result::frame;
        }
        catch (...)
        {
            // Do not clear pending here: run_until itself may throw while the
            // composed operation is still live. Shared state/websocket/ioc
            // ownership keeps a late completion safe.
            state->last_error = boost::asio::error::operation_aborted;
            return transport_read_result::terminal;
        }
    }

    bool drain_after_cancel(
        const std::shared_ptr<boost::asio::io_context>& ioc,
        std::chrono::steady_clock::time_point deadline) noexcept
    {
        if (!ioc) return false;
        const auto state = state_;
        try
        {
            while (state->pending
                   && std::chrono::steady_clock::now() < deadline)
            {
                ioc->restart();
                (void)ioc->run_until(deadline);
            }
        }
        catch (...) {}
        return !state->pending;
    }

    bool pending() const noexcept { return state_->pending; }
    boost::beast::error_code last_error() const noexcept
    {
        return state_->last_error;
    }

private:
    struct State
    {
        State() : buffer(frame_capacity) { buffer.reserve(frame_capacity); }

        void clear_buffer() noexcept
        {
            if (buffer.size() != 0) buffer.consume(buffer.size());
        }

        void reset()
        {
            clear_buffer();
            completed = false;
            error.clear();
            last_error.clear();
        }

        boost::beast::flat_buffer buffer;
        boost::beast::error_code error;
        boost::beast::error_code last_error;
        fixed_handler_arena handler_arena;
        bool pending = false;
        bool completed = false;
    };

    std::shared_ptr<State> state_;
};

} // namespace provider_ws
