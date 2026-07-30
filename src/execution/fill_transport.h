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

    // Optional. When set, a fatal stream-loss event (network/handshake
    // error past the transport's own retry budget) fires this callback
    // with a short reason string and the transport stops - the engine's
    // halt path takes over. Default no-op preserves the legacy reconnect
    // behaviour for non-live tests.
    virtual void set_fatal_disconnect_callback(
        std::function<void(std::string_view reason)> /*cb*/) {}
};
