// Compile-only smoke tests for BinanceUserDataTransport. Full behavioural
// tests live in the integration suite against a mock exchange because this
// class owns real TLS WebSocket I/O and listenKey REST lifecycle.

#include <gtest/gtest.h>

#ifdef HAS_BINANCE

#include "providers/binance/binance_user_data_transport.h"
#include "providers/private_ws_lifecycle.h"

#include <memory>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

TEST(PrivateWsLifecycle, CallbacksDetachOnlyAfterWorkerJoin)
{
    int owner_token = 0;
    provider_ws::private_ws_lifecycle lifecycle(&owner_token);
    lifecycle.set_on_message([](std::string_view) {}, true);
    lifecycle.set_fatal_disconnect_callback(
        [](std::string_view) {}, true);

    const auto session = lifecycle.begin_session_locked("test");
    lifecycle.mark_workers_started();
    EXPECT_TRUE(lifecycle.session_running(session));
    EXPECT_FALSE(lifecycle.callbacks_detachable_locked(false));

    lifecycle.begin_close_request();
    lifecycle.request_stop([] {});
    lifecycle.clear_active_session();
    lifecycle.mark_workers_joined();
    lifecycle.mark_closed(true, true);
    lifecycle.finish_close_request(false);

    EXPECT_TRUE(lifecycle.producer_joined());
    EXPECT_TRUE(lifecycle.callbacks_detachable_locked(false));
    lifecycle.set_on_message({}, true);
    lifecycle.set_fatal_disconnect_callback({}, true);
    EXPECT_FALSE(lifecycle.message_callback_ready());
    EXPECT_FALSE(lifecycle.fatal_callback_ready());
}

TEST(PrivateWsLifecycle, ReadyStatusCallbackCanStopBeforeReadyIsObserved)
{
    int owner_token = 0;
    provider_ws::private_ws_lifecycle lifecycle(&owner_token);
    lifecycle.set_on_status(
        [&](IFillTransport::lifecycle state, std::string_view) {
            if (state == IFillTransport::lifecycle::open)
                lifecycle.request_stop([] {});
        },
        true);

    const auto session = lifecycle.begin_session_locked("test");
    lifecycle.mark_workers_started();
    ASSERT_TRUE(lifecycle.set_state_for_session(
        session, IFillTransport::lifecycle::open, "ready"));
    EXPECT_FALSE(lifecycle.wait_until_ready(
        session, std::chrono::milliseconds(1)));

    lifecycle.clear_active_session();
    lifecycle.mark_workers_joined();
}

TEST(PrivateWsLifecycle, MessageCallbackFailureIsNotSilentlyDropped)
{
    int owner_token = 0;
    provider_ws::private_ws_lifecycle lifecycle(&owner_token);
    lifecycle.set_on_message(
        [](std::string_view) {
            throw std::runtime_error("consumer rejected private frame");
        },
        true);

    // Reader loops own the terminal transition. The shared lifecycle must
    // preserve their exception boundary rather than lose authenticated truth.
    EXPECT_THROW(lifecycle.publish_message("private frame"), std::runtime_error);
}

TEST(BinanceUserDataTransport, ConstructDoesNotOpenConnections)
{
    BinanceUserDataTransport tx(nullptr);
    EXPECT_EQ(tx.state(), IFillTransport::lifecycle::closed);
    EXPECT_TRUE(tx.listen_key().empty());
}

TEST(BinanceUserDataTransport, OpenWithoutRestReportsError)
{
    BinanceUserDataTransport tx(nullptr);
    tx.set_on_message([](std::string_view) {});
    tx.set_fatal_disconnect_callback([](std::string_view) {});
    IFillTransport::lifecycle seen = IFillTransport::lifecycle::closed;
    tx.set_on_status([&](IFillTransport::lifecycle s, std::string_view) {
        seen = s;
    });
    EXPECT_FALSE(tx.open());
    EXPECT_EQ(tx.state(), IFillTransport::lifecycle::error);
    EXPECT_EQ(seen,       IFillTransport::lifecycle::error);
}

TEST(BinanceUserDataTransport, OpenRefusesBeforeListenKeyWhenFatalCallbackMissing)
{
    std::atomic<int> creates{0};
    BinanceUserDataTransport tx(
        [&] {
            creates.fetch_add(1, std::memory_order_relaxed);
            BinanceRestClient::response r;
            r.status = 200;
            r.body = R"({"listenKey":"test-key"})";
            return r;
        },
        [](const std::string&) {},
        [](std::atomic<bool>&) {
            return BinanceUserDataTransport::run_result::stopped;
        });
    tx.set_on_message([](std::string_view) {});

    EXPECT_FALSE(tx.open());
    EXPECT_EQ(creates.load(std::memory_order_relaxed), 0);
    EXPECT_EQ(tx.state(), IFillTransport::lifecycle::error);
}

TEST(BinanceUserDataTransport, OpenRefusesBeforeListenKeyWhenMessageCallbackMissing)
{
    std::atomic<int> creates{0};
    BinanceUserDataTransport tx(
        [&] {
            creates.fetch_add(1, std::memory_order_relaxed);
            BinanceRestClient::response r;
            r.status = 200;
            r.body = R"({"listenKey":"test-key"})";
            return r;
        },
        [](const std::string&) {},
        [](std::atomic<bool>&) {
            return BinanceUserDataTransport::run_result::stopped;
        });
    tx.set_fatal_disconnect_callback([](std::string_view) {});

    EXPECT_FALSE(tx.open());
    EXPECT_EQ(creates.load(std::memory_order_relaxed), 0);
    EXPECT_EQ(tx.state(), IFillTransport::lifecycle::error);
}

TEST(BinanceUserDataTransport, DestructorWithNoOpenIsClean)
{
    // just making sure the destructor doesn't deadlock when open() was never
    // successful - regressing this would hang the whole test binary.
    BinanceUserDataTransport tx(nullptr);
    (void)tx.open();
}

TEST(BinanceUserDataTransport, InitialHandshakeFailureRefusesReadyAndCleansKey)
{
    std::atomic<int> deletes{0};
    BinanceUserDataTransport tx(
        [] {
            BinanceRestClient::response r;
            r.status = 200;
            r.body = R"({"listenKey":"test-key"})";
            return r;
        },
        [&](const std::string& key) {
            EXPECT_EQ(key, "test-key");
            deletes.fetch_add(1, std::memory_order_relaxed);
        },
        [](std::atomic<bool>&) {
            return BinanceUserDataTransport::run_result::handshake_error;
        });

    tx.set_on_message([](std::string_view) {});
    tx.set_fatal_disconnect_callback([](std::string_view) {});

    bool saw_error = false;
    tx.set_on_status([&](IFillTransport::lifecycle s, std::string_view) {
        if (s == IFillTransport::lifecycle::error) saw_error = true;
    });

    EXPECT_FALSE(tx.open());
    EXPECT_TRUE(saw_error);
    EXPECT_EQ(deletes.load(std::memory_order_relaxed), 1);
    EXPECT_TRUE(tx.listen_key().empty());
}

TEST(BinanceUserDataTransport, ListenKeyRequiresAuthoritativeUniqueEnvelope)
{
    const std::string bodies[] = {
        "{}",
        R"({"listenKey":"first","listenKey":"second"})",
        "{\"listenKey\":\"key\"} trailing",
        R"({"listenKey":null})",
        R"({"nested":{"listenKey":"key"}})",
    };
    for (const auto& body : bodies)
    {
        BinanceUserDataTransport tx(
            [body] {
                BinanceRestClient::response r;
                r.status = 200;
                r.body = body;
                return r;
            },
            [](const std::string&) {},
            [](std::atomic<bool>&) {
                return BinanceUserDataTransport::run_result::stopped;
            });
        tx.set_on_message([](std::string_view) {});
        tx.set_fatal_disconnect_callback([](std::string_view) {});
        EXPECT_FALSE(tx.open()) << body;
    }

    EXPECT_EQ(binance_keepalive_detail::authoritative_listen_key(
                  R"({"listenKey":"rotated"})"),
              "rotated");
    EXPECT_TRUE(binance_keepalive_detail::authoritative_listen_key(
                    R"({"listenKey":"a","listenKey":"b"})")
                    .empty());
    EXPECT_TRUE(binance_keepalive_detail::authoritative_listen_key(
                    "{\"listenKey\":\"a\"} trailing")
                    .empty());
}

TEST(BinanceUserDataTransport, CloseWinsReadyGateAndReclaimsItsListenKey)
{
    std::atomic<bool> reader_entered{false};
    std::atomic<int> deletes{0};
    BinanceUserDataTransport tx(
        [] {
            BinanceRestClient::response r;
            r.status = 200;
            r.body = R"({"listenKey":"test-key"})";
            return r;
        },
        [&](const std::string&) {
            deletes.fetch_add(1, std::memory_order_relaxed);
        },
        [&](std::atomic<bool>& stop) {
            reader_entered.store(true, std::memory_order_release);
            reader_entered.notify_all();
            stop.wait(false, std::memory_order_acquire);
            return BinanceUserDataTransport::run_result::stopped;
        });
    tx.set_on_message([](std::string_view) {});
    tx.set_fatal_disconnect_callback([](std::string_view) {});

    bool opened = true;
    std::thread opening([&] { opened = tx.open(); });
    reader_entered.wait(false, std::memory_order_acquire);

    const auto started = std::chrono::steady_clock::now();
    tx.close();
    EXPECT_LT(std::chrono::steady_clock::now() - started,
              std::chrono::seconds(2));
    opening.join();

    EXPECT_FALSE(opened);
    EXPECT_EQ(deletes.load(std::memory_order_relaxed), 1);
    EXPECT_TRUE(tx.listen_key().empty());
    EXPECT_EQ(tx.state(), IFillTransport::lifecycle::closed);
    EXPECT_TRUE(tx.private_execution_producer_joined());
}

TEST(BinanceUserDataTransport, ReaderStatusCloseIsJoinedWithoutDeadlock)
{
    std::atomic<int> open_status_calls{0};
    BinanceUserDataTransport tx(
        [] {
            BinanceRestClient::response r;
            r.status = 200;
            r.body = R"({"listenKey":"test-key"})";
            return r;
        },
        [](const std::string&) {},
        [](std::atomic<bool>& stop) {
            stop.wait(false, std::memory_order_acquire);
            return BinanceUserDataTransport::run_result::stopped;
        },
        {}, {}, true);
    tx.set_on_message([](std::string_view) {});
    tx.set_fatal_disconnect_callback([](std::string_view) {});
    tx.set_on_status([&](IFillTransport::lifecycle state, std::string_view) {
        if (state == IFillTransport::lifecycle::open)
        {
            open_status_calls.fetch_add(1, std::memory_order_relaxed);
            tx.close();
        }
    });

    const auto started = std::chrono::steady_clock::now();
    EXPECT_FALSE(tx.open());
    EXPECT_LT(std::chrono::steady_clock::now() - started,
              std::chrono::seconds(2));
    EXPECT_EQ(open_status_calls.load(std::memory_order_relaxed), 1);
    EXPECT_TRUE(tx.private_execution_producer_joined());
}

TEST(BinanceUserDataTransport, ConnectingStatusCallbackCanCloseWithoutControlMutexDeadlock)
{
    std::atomic<int> connecting_calls{0};
    BinanceUserDataTransport tx(
        [] {
            BinanceRestClient::response r;
            r.status = 200;
            r.body = R"({"listenKey":"test-key"})";
            return r;
        },
        [](const std::string&) {},
        [](std::atomic<bool>& stop) {
            stop.wait(false, std::memory_order_acquire);
            return BinanceUserDataTransport::run_result::stopped;
        });
    tx.set_on_message([](std::string_view) {});
    tx.set_fatal_disconnect_callback([](std::string_view) {});
    tx.set_on_status([&](IFillTransport::lifecycle state, std::string_view) {
        if (state == IFillTransport::lifecycle::connecting)
        {
            connecting_calls.fetch_add(1, std::memory_order_relaxed);
            tx.close();
        }
    });

    const auto started = std::chrono::steady_clock::now();
    EXPECT_FALSE(tx.open());
    EXPECT_LT(std::chrono::steady_clock::now() - started,
              std::chrono::seconds(2));
    EXPECT_EQ(connecting_calls.load(std::memory_order_relaxed), 1);
    EXPECT_TRUE(tx.private_execution_producer_joined());
}

TEST(BinanceUserDataTransport, CallbackClearIsRefusedWhileLiveThenAllowedAfterJoin)
{
    BinanceUserDataTransport tx(
        [] {
            BinanceRestClient::response r;
            r.status = 200;
            r.body = R"({"listenKey":"test-key"})";
            return r;
        },
        [](const std::string&) {},
        [](std::atomic<bool>& stop) {
            stop.wait(false, std::memory_order_acquire);
            return BinanceUserDataTransport::run_result::stopped;
        },
        {}, {}, true);
    tx.set_on_message([](std::string_view) {});
    tx.set_on_status([](IFillTransport::lifecycle, std::string_view) {});
    tx.set_fatal_disconnect_callback([](std::string_view) {});

    ASSERT_TRUE(tx.open());

    // Clearing any callback while the reader/keepalive generation is live is
    // refused. A successful reopen proves the required delivery/fatal routes
    // remained installed.
    tx.set_on_message({});
    tx.set_on_status({});
    tx.set_fatal_disconnect_callback({});
    tx.close();
    ASSERT_TRUE(tx.open());
    tx.close();
    ASSERT_TRUE(tx.private_execution_producer_joined());

    // Once close() has established joined proof, the bridge can release all
    // callbacks before it destroys its own state. The next open fails before
    // any venue resource is created because the required routes are gone.
    tx.set_on_message({});
    tx.set_on_status({});
    tx.set_fatal_disconnect_callback({});
    EXPECT_FALSE(tx.open());
}

TEST(BinanceUserDataTransport, ConcurrentOpenDuringReadyGateCannotRestartAfterClose)
{
    std::atomic<int> creates{0};
    std::atomic<int> readers{0};
    std::atomic<bool> reader_entered{false};
    std::atomic<int> deletes{0};
    BinanceUserDataTransport tx(
        [&] {
            creates.fetch_add(1, std::memory_order_relaxed);
            BinanceRestClient::response r;
            r.status = 200;
            r.body = R"({"listenKey":"test-key"})";
            return r;
        },
        [&](const std::string&) {
            deletes.fetch_add(1, std::memory_order_relaxed);
        },
        [&](std::atomic<bool>& stop) {
            readers.fetch_add(1, std::memory_order_relaxed);
            reader_entered.store(true, std::memory_order_release);
            reader_entered.notify_all();
            stop.wait(false, std::memory_order_acquire);
            return BinanceUserDataTransport::run_result::stopped;
        });
    tx.set_on_message([](std::string_view) {});
    tx.set_fatal_disconnect_callback([](std::string_view) {});

    bool first = true;
    std::thread first_open([&] { first = tx.open(); });
    reader_entered.wait(false, std::memory_order_acquire);

    // The first invocation owns the ready gate.  A second open must refuse
    // rather than queue and turn into an accidental post-close reopen.
    EXPECT_FALSE(tx.open());

    tx.close();
    first_open.join();

    EXPECT_FALSE(first);
    EXPECT_EQ(creates.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(readers.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(deletes.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(tx.state(), IFillTransport::lifecycle::closed);
    EXPECT_TRUE(tx.private_execution_producer_joined());
}

TEST(BinanceUserDataTransport, ReopenStartsOnlyAfterPriorWorkersWereJoined)
{
    std::atomic<int> creates{0};
    std::atomic<int> reader_starts{0};
    std::atomic<int> deletes{0};
    BinanceUserDataTransport tx(
        [&] {
            creates.fetch_add(1, std::memory_order_relaxed);
            BinanceRestClient::response r;
            r.status = 200;
            r.body = R"({"listenKey":"test-key"})";
            return r;
        },
        [&](const std::string&) {
            deletes.fetch_add(1, std::memory_order_relaxed);
        },
        [&](std::atomic<bool>& stop) {
            reader_starts.fetch_add(1, std::memory_order_release);
            reader_starts.notify_all();
            stop.wait(false, std::memory_order_acquire);
            return BinanceUserDataTransport::run_result::stopped;
        },
        {}, {}, true);
    tx.set_on_message([](std::string_view) {});
    tx.set_fatal_disconnect_callback([](std::string_view) {});

    EXPECT_FALSE(tx.private_execution_producer_joined());
    ASSERT_TRUE(tx.open());
    int expected = 0;
    while (reader_starts.load(std::memory_order_acquire) == expected)
        reader_starts.wait(expected, std::memory_order_acquire);
    EXPECT_FALSE(tx.private_execution_producer_joined());
    tx.close();
    EXPECT_TRUE(tx.private_execution_producer_joined());

    ASSERT_TRUE(tx.open());
    expected = 1;
    while (reader_starts.load(std::memory_order_acquire) == expected)
        reader_starts.wait(expected, std::memory_order_acquire);
    EXPECT_EQ(reader_starts.load(std::memory_order_acquire), 2);
    EXPECT_EQ(creates.load(std::memory_order_relaxed), 2);
    EXPECT_FALSE(tx.private_execution_producer_joined());
    tx.close();
    EXPECT_EQ(deletes.load(std::memory_order_relaxed), 2);
    EXPECT_TRUE(tx.private_execution_producer_joined());
}

TEST(BinanceUserDataTransport, TerminalKeepaliveWakesReaderAndPublishesFatal)
{
    for (const bool throw_from_keepalive : {false, true})
    {
        std::atomic<bool> reader_entered{false};
        std::atomic<bool> allow_keepalive{false};
        std::atomic<int> fatal_calls{0};
        std::atomic<int> deletes{0};
        binance_keepalive_policy policy;
        policy.interval = std::chrono::seconds{0};
        policy.retry_delay = std::chrono::seconds{0};
        policy.max_retries = 1;

        BinanceUserDataTransport tx(
            [] {
                BinanceRestClient::response r;
                r.status = 200;
                r.body = R"({"listenKey":"test-key"})";
                return r;
            },
            [&](const std::string&) {
                deletes.fetch_add(1, std::memory_order_release);
            },
            [&](std::atomic<bool>& stop) {
                reader_entered.store(true, std::memory_order_release);
                reader_entered.notify_all();
                stop.wait(false, std::memory_order_acquire);
                return BinanceUserDataTransport::run_result::stopped;
            },
            [&, throw_from_keepalive]()
                -> binance_keepalive_detail::tick_result {
                allow_keepalive.wait(false, std::memory_order_acquire);
                if (throw_from_keepalive) throw 7;
                binance_keepalive_detail::tick_result r;
                r.k = binance_keepalive_detail::tick_result::kind::error;
                r.note = "forced keepalive failure";
                return r;
            },
            policy,
            true);
        tx.set_on_message([](std::string_view) {});
        tx.set_fatal_disconnect_callback([&](std::string_view) {
            fatal_calls.fetch_add(1, std::memory_order_release);
            fatal_calls.notify_all();
        });

        ASSERT_TRUE(tx.open());
        reader_entered.wait(false, std::memory_order_acquire);
        allow_keepalive.store(true, std::memory_order_release);
        allow_keepalive.notify_all();
        int expected = 0;
        while (fatal_calls.load(std::memory_order_acquire) == 0)
            fatal_calls.wait(expected, std::memory_order_acquire);
        const auto start = std::chrono::steady_clock::now();
        tx.close();
        const auto elapsed = std::chrono::steady_clock::now() - start;
        EXPECT_LT(elapsed, std::chrono::milliseconds(500));
        EXPECT_EQ(fatal_calls.load(std::memory_order_acquire), 1);
        EXPECT_EQ(deletes.load(std::memory_order_acquire), 1);
        EXPECT_EQ(tx.state(), IFillTransport::lifecycle::closed);
    }
}

TEST(BinanceUserDataTransport, RotatedListenKeyIsTerminalWithoutMutatingLiveReaderKey)
{
    std::atomic<bool> reader_entered{false};
    std::atomic<bool> allow_keepalive{false};
    std::atomic<int> fatal_calls{0};
    binance_keepalive_policy policy;
    policy.interval = std::chrono::seconds{0};
    policy.retry_delay = std::chrono::seconds{0};
    policy.max_retries = 1;

    BinanceUserDataTransport tx(
        [] {
            BinanceRestClient::response r;
            r.status = 200;
            r.body = R"({"listenKey":"test-key"})";
            return r;
        },
        [](const std::string&) {},
        [&](std::atomic<bool>& stop) {
            reader_entered.store(true, std::memory_order_release);
            reader_entered.notify_all();
            stop.wait(false, std::memory_order_acquire);
            return BinanceUserDataTransport::run_result::stopped;
        },
        [&] {
            allow_keepalive.wait(false, std::memory_order_acquire);
            binance_keepalive_detail::tick_result r;
            r.k = binance_keepalive_detail::tick_result::kind::rotated;
            r.new_key = "rotated-key";
            r.note = "forced listenKey rotation";
            return r;
        },
        policy,
        true);
    tx.set_on_message([](std::string_view) {});
    tx.set_fatal_disconnect_callback([&](std::string_view) {
        fatal_calls.fetch_add(1, std::memory_order_release);
        fatal_calls.notify_all();
    });

    ASSERT_TRUE(tx.open());
    reader_entered.wait(false, std::memory_order_acquire);
    allow_keepalive.store(true, std::memory_order_release);
    allow_keepalive.notify_all();
    int expected = 0;
    while (fatal_calls.load(std::memory_order_acquire) == 0)
        fatal_calls.wait(expected, std::memory_order_acquire);

    EXPECT_EQ(tx.state(), IFillTransport::lifecycle::error);
    EXPECT_EQ(tx.listen_key(), "test-key");
    tx.close();
    EXPECT_EQ(fatal_calls.load(std::memory_order_acquire), 1);
}

#endif // HAS_BINANCE
