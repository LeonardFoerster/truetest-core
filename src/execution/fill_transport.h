#pragma once

#include <functional>
#include <string_view>

class IFillTransport
{
public:
    enum class lifecycle
    {
        closed,
        connecting,
        open,
        degraded,
        error
    };

    using message_cb = std::function<void(std::string_view)>;
    using status_cb  = std::function<void(lifecycle, std::string_view)>;

    virtual ~IFillTransport() = default;

    virtual bool open() = 0;
    virtual void close() = 0;
    virtual lifecycle state() const = 0;

    virtual void set_on_message(message_cb cb) = 0;
    virtual void set_on_status(status_cb cb) = 0;
};
