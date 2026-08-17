#include <gtest/gtest.h>

#include "bin/provider_open_policy.h"
#include "providers/thread_safe_callback.h"
#if defined(HAS_BINANCE) || defined(HAS_BITGET)
#include "providers/bounded_ws_open.h"
#include <boost/asio/ssl.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

class CapabilityProvider : public IProvider
{
public:
    CapabilityProvider(bool data_feed, bool execution, bool open_result = true)
        : data_feed_(data_feed)
        , execution_(execution)
        , open_result_(open_result)
    {
    }

    std::string name() const override { return "capability-provider"; }
    bool has_data_feed() const override { return data_feed_; }
    bool has_execution() const override { return execution_; }

    bool open() override
    {
        ++open_calls;
        return open_result_;
    }

    void close() override { ++close_calls; }

    std::shared_ptr<IDataTransport> get_transport() override { return {}; }
    std::shared_ptr<IExecutionAdapter> get_execution_adapter() override
    {
        return {};
    }

    int open_calls = 0;
    int close_calls = 0;

private:
    bool data_feed_;
    bool execution_;
    bool open_result_;
};

class StopSpy
{
public:
    void stop()
    {
        {
            std::lock_guard<std::mutex> lock(mu_);
            ++stop_calls_;
        }
        stopped_.notify_all();
    }

    bool wait_until_stopped(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(mu_);
        return stopped_.wait_for(lock, timeout,
            [this] { return stop_calls_ != 0; });
    }

    int stop_calls() const
    {
        std::lock_guard<std::mutex> lock(mu_);
        return stop_calls_;
    }

private:
    mutable std::mutex mu_;
    std::condition_variable stopped_;
    int stop_calls_ = 0;
};

class SignalDuringOpenProvider final : public CapabilityProvider
{
public:
    SignalDuringOpenProvider() : CapabilityProvider(true, false) {}
    bool open() override
    {
        const bool result = CapabilityProvider::open();
        truetest::bin::mark_shutdown_signal(SIGTERM);
        return result;
    }
};

} // namespace

TEST(ProviderOpenPolicy, ExecutionOnlyProviderIsOpened)
{
    truetest::bin::reset_shutdown_signal();
    auto provider = std::make_shared<CapabilityProvider>(false, true);
    LiveSafetySession session(provider, false, std::chrono::milliseconds{50});

    EXPECT_TRUE(truetest::bin::open_provider_if_required(*provider, session));
    EXPECT_EQ(provider->open_calls, 1);
    EXPECT_TRUE(session.is_open());
}

TEST(ProviderOpenPolicy, DataOnlyProviderIsOpened)
{
    truetest::bin::reset_shutdown_signal();
    auto provider = std::make_shared<CapabilityProvider>(true, false);
    LiveSafetySession session(provider, false, std::chrono::milliseconds{50});

    EXPECT_TRUE(truetest::bin::open_provider_if_required(*provider, session));
    EXPECT_EQ(provider->open_calls, 1);
    EXPECT_TRUE(session.is_open());
}

TEST(ProviderOpenPolicy, CapabilityLessProviderRemainsClosed)
{
    truetest::bin::reset_shutdown_signal();
    auto provider = std::make_shared<CapabilityProvider>(false, false);
    LiveSafetySession session(provider, false, std::chrono::milliseconds{50});

    EXPECT_TRUE(truetest::bin::open_provider_if_required(*provider, session));
    EXPECT_EQ(provider->open_calls, 0);
    EXPECT_FALSE(session.is_open());
}

TEST(ProviderOpenPolicy, ExecutionOnlyOpenFailureRefusesAndCloses)
{
    truetest::bin::reset_shutdown_signal();
    auto provider = std::make_shared<CapabilityProvider>(false, true, false);
    LiveSafetySession session(provider, false, std::chrono::milliseconds{50});

    EXPECT_FALSE(truetest::bin::open_provider_if_required(*provider, session));
    EXPECT_EQ(provider->open_calls, 1);
    EXPECT_EQ(provider->close_calls, 1);
    EXPECT_FALSE(session.is_open());
}

TEST(ProviderOpenPolicy, SignalDuringOpenPreventsEngineStartupAndClosesProvider)
{
    truetest::bin::reset_shutdown_signal();
    auto provider = std::make_shared<SignalDuringOpenProvider>();
    LiveSafetySession session(provider, false, std::chrono::milliseconds{50});

    EXPECT_FALSE(truetest::bin::open_provider_if_required(*provider, session));
    EXPECT_EQ(provider->open_calls, 1);
    EXPECT_EQ(provider->close_calls, 1);
    EXPECT_FALSE(session.is_open());
    truetest::bin::reset_shutdown_signal();
}

TEST(ProviderOpenPolicy, SignalHandlerOnlyNotifiesLifetimeOwnedBridgeMonitor)
{
    auto bridge = std::make_shared<StopSpy>();
    truetest::bin::reset_shutdown_signal();
    {
        truetest::bin::bridge_shutdown_monitor monitor(bridge);
        truetest::bin::mark_shutdown_signal(SIGTERM);
        ASSERT_TRUE(bridge->wait_until_stopped(std::chrono::milliseconds{250}));
    }
    EXPECT_EQ(bridge->stop_calls(), 1);
}

TEST(ProviderOpenPolicy, ExceptionDestroysBridgeMonitorBeforeFailClosedShutdown)
{
    auto provider = std::make_shared<CapabilityProvider>(true, false);
    LiveSafetySession session(provider, false, std::chrono::milliseconds{50});
    ASSERT_TRUE(session.open_provider());

    auto bridge = std::make_shared<StopSpy>();
    truetest::bin::reset_shutdown_signal();
    std::ostringstream errors;
    const int rc = truetest::bin::run_provider_session_guarded(
        session,
        [&] {
            truetest::bin::bridge_shutdown_monitor monitor(bridge);
            throw std::runtime_error("stream failed");
            return 0;
        },
        errors);

    EXPECT_EQ(rc, 1);
    EXPECT_EQ(bridge->stop_calls(), 0);
    EXPECT_EQ(provider->close_calls, 1);
}

TEST(ThreadSafeCallback, ConcurrentReplacementAndInvocationUseOwnedSnapshots)
{
    ThreadSafeCallback<void()> callback;
    std::atomic<int> calls{0};
    std::atomic<bool> start{false};

    callback.store([&] { calls.fetch_add(1, std::memory_order_relaxed); });
    auto old_snapshot = callback.load();
    ASSERT_TRUE(old_snapshot);

    std::thread writer([&] {
        while (!start.load(std::memory_order_acquire)) {}
        for (int i = 0; i < 10'000; ++i)
            callback.store([&] {
                calls.fetch_add(1, std::memory_order_relaxed);
            });
    });
    std::thread reader([&] {
        start.store(true, std::memory_order_release);
        for (int i = 0; i < 10'000; ++i)
            if (auto current = callback.load()) (*current)();
    });

    writer.join();
    reader.join();
    (*old_snapshot)();
    EXPECT_GT(calls.load(std::memory_order_relaxed), 0);
}

TEST(LatchedFailureCallback, DeliversPreRegistrationFailureExactlyOnce)
{
    LatchedFailureCallback callback;
    callback.publish("private stream lost before engine callback");

    int calls = 0;
    std::string reason;
    callback.store([&](std::string_view value) {
        ++calls;
        reason.assign(value);
    });
    callback.store([&](std::string_view) { ++calls; });

    EXPECT_EQ(calls, 1);
    EXPECT_EQ(reason, "private stream lost before engine callback");
}

TEST(LatchedFailureCallback, ThrowingConsumerCannotEscapeProviderThreadPath)
{
    LatchedFailureCallback callback;
    callback.store([](std::string_view) { throw std::runtime_error("boom"); });
    EXPECT_NO_THROW(callback.publish("terminal private stream failure"));
}

#if defined(HAS_BINANCE) || defined(HAS_BITGET)
TEST(BoundedWsOpen, CancellationCompletionIsDeadlineBounded)
{
    boost::asio::io_context ioc;
    boost::asio::steady_timer timer(ioc);
    timer.expires_after(std::chrono::seconds(10));
    const auto started = std::chrono::steady_clock::now();
    const bool ok = provider_ws::run_bounded(
        ioc, std::chrono::milliseconds(20),
        [&](auto done) {
            timer.async_wait([done](boost::system::error_code ec) mutable {
                done(ec);
            });
        },
        [&] { timer.cancel(); });

    EXPECT_FALSE(ok);
    EXPECT_LT(std::chrono::steady_clock::now() - started,
              std::chrono::milliseconds(500));
}

TEST(BoundedWsOpen, NonCompletingCancellationHasBoundedGraceAndSafeLateHandler)
{
    boost::asio::io_context ioc;
    boost::asio::steady_timer timer(ioc);
    timer.expires_after(std::chrono::seconds(10));
    const auto started = std::chrono::steady_clock::now();
    const bool ok = provider_ws::run_bounded(
        ioc, std::chrono::milliseconds(20),
        [&](auto done) {
            timer.async_wait([done](boost::system::error_code ec) mutable {
                done(ec);
            });
        },
        [] {});

    EXPECT_FALSE(ok);
    EXPECT_LT(std::chrono::steady_clock::now() - started,
              std::chrono::milliseconds(500));

    // The operation callback owns its completion state, so a late cancelled
    // handler cannot reference run_bounded's returned stack frame.
    timer.cancel();
    ioc.restart();
    EXPECT_NO_THROW(ioc.poll());
}

TEST(BoundedWsOpen, StalledTlsHandshakeReturnsWithinDeadline)
{
    namespace net = boost::asio;
    namespace beast = boost::beast;
    namespace websocket = beast::websocket;
    using tcp = net::ip::tcp;

    net::io_context server_ioc;
    tcp::acceptor acceptor(server_ioc,
        tcp::endpoint(net::ip::address_v4::loopback(), 0));
    auto accepted = std::make_shared<tcp::socket>(server_ioc);
    acceptor.async_accept(*accepted, [](boost::system::error_code) {});
    std::thread server([&] { server_ioc.run(); });

    net::io_context client_ioc;
    net::ssl::context tls(net::ssl::context::tls_client);
    tls.set_verify_mode(net::ssl::verify_none);
    websocket::stream<beast::ssl_stream<tcp::socket>> ws(client_ioc, tls);

    const auto started = std::chrono::steady_clock::now();
    const bool opened = provider_ws::open_tls_websocket(
        client_ioc, ws, "127.0.0.1",
        std::to_string(acceptor.local_endpoint().port()), "/",
        std::chrono::milliseconds(30),
        [](auto&) {}, [](auto&) {});
    const auto elapsed = std::chrono::steady_clock::now() - started;

    boost::system::error_code ignored;
    accepted->close(ignored);
    acceptor.close(ignored);
    server_ioc.stop();
    server.join();

    EXPECT_FALSE(opened);
    EXPECT_LT(elapsed, std::chrono::seconds(1));
}
#endif
