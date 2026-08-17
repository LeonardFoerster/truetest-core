#pragma once

#include <atomic>
#include <array>
#include <algorithm>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

template <typename Signature>
class ThreadSafeCallback
{
public:
    using function_type = std::function<Signature>;

    void store(function_type callback)
    {
        std::shared_ptr<const function_type> published;
        if (callback)
            published = std::make_shared<const function_type>(
                std::move(callback));
        callback_.store(std::move(published), std::memory_order_release);
    }

    std::shared_ptr<const function_type> load() const noexcept
    {
        return callback_.load(std::memory_order_acquire);
    }

private:
    std::atomic<std::shared_ptr<const function_type>> callback_{nullptr};
};

class LatchedFailureCallback
{
public:
    using function_type = std::function<void(std::string_view)>;

    void store(function_type callback)
    {
        std::array<char, 192> pending{};
        std::size_t pending_size = 0;
        std::shared_ptr<const function_type> snapshot;
        {
            std::lock_guard<std::mutex> lock(mu_);
            callback_.store(std::move(callback));
            snapshot = callback_.load();
            if (snapshot && pending_size_ != 0)
            {
                pending_size = pending_size_;
                std::copy_n(pending_reason_.data(), pending_size,
                            pending.data());
                pending_size_ = 0;
            }
        }
        if (snapshot && pending_size != 0)
        {
            try { (*snapshot)(std::string_view(pending.data(), pending_size)); }
            catch (...) {}
        }
    }

    void publish(std::string_view reason) noexcept
    {
        try
        {
            std::shared_ptr<const function_type> snapshot;
            {
                std::lock_guard<std::mutex> lock(mu_);
                snapshot = callback_.load();
                if (!snapshot && pending_size_ == 0)
                {
                    pending_size_ = std::min(reason.size(), pending_reason_.size());
                    std::copy_n(
                        reason.data(), pending_size_, pending_reason_.data());
                }
            }
            if (snapshot)
            {
                try { (*snapshot)(reason); }
                catch (...) {}
            }
        }
        catch (...) {}
    }

private:
    std::mutex mu_;
    std::array<char, 192> pending_reason_{};
    std::size_t pending_size_ = 0;
    ThreadSafeCallback<void(std::string_view)> callback_;
};
