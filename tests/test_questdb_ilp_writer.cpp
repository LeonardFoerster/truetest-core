#ifdef HAS_QUESTDB

#include "data/questdb/ilp_writer.h"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <streambuf>
#include <vector>

using truetest::questdb::IIlpTransport;
using truetest::questdb::IlpWriter;
using truetest::questdb::IlpWriteOutcome;
using truetest::questdb::LineBuilder;

namespace {

class FakeTransport : public IIlpTransport
{
public:
    bool connect(const std::string& /*host*/, std::uint16_t /*port*/) override
    {
        connect_calls++;
        if (fail_connect_n_times > 0)
        {
            fail_connect_n_times--;
            connected_ = false;
            return false;
        }
        connected_ = true;
        return true;
    }
    bool write_all(std::string_view data) override
    {
        return do_write(data) == IlpWriteOutcome::complete;
    }
    IlpWriteOutcome write_attempt(std::string_view data) override
    {
        return do_write(data);
    }
    void close() override { connected_ = false; }
    bool is_connected() const override { return connected_; }

    std::vector<std::string> writes;
    std::vector<std::string> attempts;
    int connect_calls = 0;
    int write_calls = 0;
    int fail_connect_n_times = 0;
    int fail_write_n_times = 0;
    int ambiguous_write_n_times = 0;

private:
    IlpWriteOutcome do_write(std::string_view data)
    {
        write_calls++;
        attempts.emplace_back(data);
        if (ambiguous_write_n_times > 0)
        {
            ambiguous_write_n_times--;
            return IlpWriteOutcome::delivery_ambiguous;
        }
        if (fail_write_n_times > 0)
        {
            fail_write_n_times--;
            return IlpWriteOutcome::no_bytes_sent;
        }
        writes.emplace_back(data);
        return IlpWriteOutcome::complete;
    }

    bool connected_ = false;
};

class LegacyBoolOnlyTransport final : public IIlpTransport
{
public:
    bool connect(const std::string& /*host*/, std::uint16_t /*port*/) override
    {
        connected_ = true;
        ++connect_calls;
        return true;
    }
    bool write_all(std::string_view /*data*/) override
    {
        ++write_calls;
        return false;
    }
    void close() override { connected_ = false; }
    bool is_connected() const override { return connected_; }

    int connect_calls = 0;
    int write_calls = 0;

private:
    bool connected_ = false;
};

class ThrowingStream final : public std::ostream
{
private:
    class ThrowingBuffer final : public std::streambuf
    {
    protected:
        std::streamsize xsputn(const char*, std::streamsize) override
        {
            throw std::runtime_error("fallback stream failure");
        }
    } buffer_;

public:
    ThrowingStream()
        : std::ostream(&buffer_)
    {
        exceptions(std::ios::badbit);
    }
};

}

// ── LineBuilder ──────────────────────────────────────────────────────────────

TEST(QuestdbLineBuilder, Plain)
{
    LineBuilder b("t");
    b.add_tag("a", "1").add_field_long("b", 2);
    EXPECT_EQ(b.finish(1000), "t,a=1 b=2i 1000\n");
}

TEST(QuestdbLineBuilder, EscapeTagComma)
{
    LineBuilder b("t");
    b.add_tag("k", "a,b").add_field_long("x", 1);
    EXPECT_EQ(b.finish(0), "t,k=a\\,b x=1i 0\n");
}

TEST(QuestdbLineBuilder, EscapeTagSpace)
{
    LineBuilder b("t");
    b.add_tag("k", "a b").add_field_long("x", 1);
    EXPECT_EQ(b.finish(0), "t,k=a\\ b x=1i 0\n");
}

TEST(QuestdbLineBuilder, EscapeStringQuote)
{
    LineBuilder b("t");
    b.add_field_str("k", "a\"b");
    EXPECT_EQ(b.finish(0), "t k=\"a\\\"b\" 0\n");
}

TEST(QuestdbLineBuilder, MixedFieldTypes)
{
    LineBuilder b("t");
    b.add_tag("sym", "BTC")
     .add_field_long("n", 7)
     .add_field_double("px", 1.5)
     .add_field_str("note", "hi")
     .add_field_bool("active", true);
    const std::string out = b.finish(42);
    EXPECT_EQ(out,
        "t,sym=BTC n=7i,px=1.5,note=\"hi\",active=t 42\n");
}

TEST(QuestdbLineBuilder, DoublePrecision)
{
    LineBuilder b("t");
    b.add_field_double("x", 1.234567890123);
    const std::string out = b.finish(0);
    // Round-trip the printed value.
    const auto eq = out.find('=');
    const auto sp = out.find(' ', eq);
    const std::string num_str = out.substr(eq + 1, sp - eq - 1);
    const double back = std::stod(num_str);
    EXPECT_NEAR(back, 1.234567890123, 1e-12);
}

TEST(QuestdbLineBuilder, TimestampNs)
{
    LineBuilder b("t");
    b.add_field_long("x", 1);
    EXPECT_EQ(b.finish(1714000000000000000LL),
        "t x=1i 1714000000000000000\n");
}

// ── IlpWriter ────────────────────────────────────────────────────────────────

TEST(QuestdbIlpWriter, BuffersUntilThreshold)
{
    auto fake = std::make_unique<FakeTransport>();
    auto* fake_raw = fake.get();
    IlpWriter w("h", 9009, std::move(fake), /*flush_every_n_lines=*/3);
    ASSERT_TRUE(w.connect());

    w.enqueue("a\n");
    w.enqueue("b\n");
    EXPECT_EQ(fake_raw->writes.size(), 0u);
    EXPECT_EQ(w.pending_lines(), 2u);

    w.enqueue("c\n"); // hits threshold, triggers flush
    ASSERT_EQ(fake_raw->writes.size(), 1u);
    EXPECT_EQ(fake_raw->writes[0], "a\nb\nc\n");
    EXPECT_EQ(w.pending_lines(), 0u);
}

TEST(QuestdbIlpWriter, ExplicitFlush)
{
    auto fake = std::make_unique<FakeTransport>();
    auto* fake_raw = fake.get();
    IlpWriter w("h", 9009, std::move(fake), /*flush_every_n_lines=*/1000);
    ASSERT_TRUE(w.connect());

    w.enqueue("x\n");
    w.enqueue("y\n");
    w.enqueue("z\n");
    EXPECT_EQ(fake_raw->writes.size(), 0u);
    EXPECT_TRUE(w.flush());
    ASSERT_EQ(fake_raw->writes.size(), 1u);
    EXPECT_EQ(fake_raw->writes[0], "x\ny\nz\n");
}

TEST(QuestdbIlpWriter, FlushFailureRetainsBuffer)
{
    auto fake = std::make_unique<FakeTransport>();
    auto* fake_raw = fake.get();
    fake_raw->fail_write_n_times = 1;
    IlpWriter w("h", 9009, std::move(fake), /*flush_every_n_lines=*/1000);
    ASSERT_TRUE(w.connect());

    w.enqueue("a\n");
    w.enqueue("b\n");
    EXPECT_FALSE(w.flush());
    EXPECT_TRUE(w.failure_latched());
    EXPECT_EQ(w.pending_lines(), 2u);
    EXPECT_EQ(fake_raw->writes.size(), 0u);
}

TEST(QuestdbIlpWriter, ReconnectAfterFailure)
{
    auto fake = std::make_unique<FakeTransport>();
    auto* fake_raw = fake.get();
    fake_raw->fail_write_n_times = 1;
    IlpWriter w("h", 9009, std::move(fake), /*flush_every_n_lines=*/1000);
    ASSERT_TRUE(w.connect());

    w.enqueue("hello\n");
    EXPECT_FALSE(w.flush());
    // After failure: socket was closed; next flush reconnects + writes.
    EXPECT_TRUE(w.flush());
    EXPECT_TRUE(w.failure_latched());
    ASSERT_EQ(fake_raw->writes.size(), 1u);
    EXPECT_EQ(fake_raw->writes[0], "hello\n");
    EXPECT_GE(fake_raw->connect_calls, 2);
}

TEST(QuestdbIlpWriter, FailedBufferIsBoundedAndRetainedLinesRecoverInFifoOrder)
{
    auto fake = std::make_unique<FakeTransport>();
    auto* fake_raw = fake.get();
    IlpWriter w("h", 9009, std::move(fake),
                /*flush_every_n_lines=*/1000,
                std::chrono::hours{1},
                /*max_pending_bytes=*/8);
    std::size_t callbacks = 0;
    w.set_failure_callback([&] { ++callbacks; });
    ASSERT_TRUE(w.connect());

    w.enqueue("a\n");
    w.enqueue("b\n");
    w.enqueue("c\n");
    w.enqueue("d\n");
    ASSERT_EQ(w.pending_bytes(), 8u);

    fake_raw->fail_write_n_times = 1;
    EXPECT_FALSE(w.flush());
    EXPECT_TRUE(w.failure_latched());

    for (int i = 0; i < 96; ++i)
    {
        w.enqueue("x\n");
        EXPECT_LE(w.pending_bytes(), 8u);
    }

    EXPECT_EQ(w.pending_lines(), 4u);
    EXPECT_EQ(w.pending_bytes(), 8u);
    EXPECT_EQ(w.dropped_lines(), 96u);
    EXPECT_EQ(callbacks, 1u);
    EXPECT_EQ(fake_raw->connect_calls, 1);
    EXPECT_TRUE(fake_raw->writes.empty());

    EXPECT_TRUE(w.flush());
    ASSERT_EQ(fake_raw->writes.size(), 1u);
    EXPECT_EQ(fake_raw->writes[0], "a\nb\nc\nd\n");
    EXPECT_EQ(w.pending_lines(), 0u);
    EXPECT_EQ(w.pending_bytes(), 0u);
    EXPECT_EQ(w.dropped_lines(), 96u);
    EXPECT_EQ(callbacks, 1u);
}

TEST(QuestdbIlpWriter, FullBufferUsesHealthyFallbackBeforeDroppingIncomingLine)
{
    auto fake = std::make_unique<FakeTransport>();
    auto* fake_raw = fake.get();
    IlpWriter w("h", 9009, std::move(fake),
                /*flush_every_n_lines=*/1000,
                std::chrono::hours{1},
                /*max_pending_bytes=*/8);
    std::size_t callbacks = 0;
    w.set_failure_callback([&] { ++callbacks; });
    ASSERT_TRUE(w.connect());

    auto fallback = std::make_unique<std::ostringstream>();
    auto* fallback_raw = fallback.get();
    w.enable_fallback(std::move(fallback));

    w.enqueue("a\n");
    w.enqueue("b\n");
    w.enqueue("c\n");
    w.enqueue("d\n");
    fake_raw->fail_write_n_times = 1;
    w.enqueue("e\n");

    EXPECT_EQ(fallback_raw->str(), "a\nb\nc\nd\n");
    EXPECT_EQ(w.fallback_lines(), 4u);
    EXPECT_EQ(w.pending_lines(), 1u);
    EXPECT_EQ(w.pending_bytes(), 2u);
    EXPECT_EQ(w.dropped_lines(), 0u);
    EXPECT_TRUE(w.failure_latched());
    EXPECT_EQ(callbacks, 1u);

    EXPECT_TRUE(w.flush());
    ASSERT_EQ(fake_raw->writes.size(), 1u);
    EXPECT_EQ(fake_raw->writes[0], "e\n");
}

TEST(QuestdbIlpWriter, AmbiguousWriteQuarantinesTheBatchWithoutReplay)
{
    auto fake = std::make_unique<FakeTransport>();
    auto* fake_raw = fake.get();
    IlpWriter w("h", 9009, std::move(fake),
                /*flush_every_n_lines=*/1000,
                std::chrono::milliseconds{0},
                /*max_pending_bytes=*/8);
    ASSERT_TRUE(w.connect());
    auto fallback = std::make_unique<std::ostringstream>();
    auto* fallback_raw = fallback.get();
    w.enable_fallback(std::move(fallback));
    w.enqueue("a\n");
    w.enqueue("b\n");
    w.enqueue("c\n");
    w.enqueue("d\n");

    fake_raw->ambiguous_write_n_times = 1;
    EXPECT_FALSE(w.flush());
    EXPECT_TRUE(w.failure_latched());
    EXPECT_TRUE(w.delivery_ambiguous());
    EXPECT_FALSE(w.is_connected());
    EXPECT_EQ(w.pending_lines(), 4u);
    EXPECT_EQ(w.pending_bytes(), 8u);
    EXPECT_EQ(fallback_raw->str(), "");
    ASSERT_EQ(fake_raw->attempts.size(), 1u);
    EXPECT_EQ(fake_raw->attempts[0], "a\nb\nc\nd\n");

    EXPECT_FALSE(w.flush());
    w.maybe_time_flush();
    EXPECT_FALSE(w.enqueue("e\n"));
    EXPECT_EQ(w.dropped_lines(), 1u);
    EXPECT_EQ(w.pending_lines(), 4u);
    EXPECT_EQ(fake_raw->connect_calls, 1);
    EXPECT_EQ(fake_raw->attempts.size(), 1u);
}

TEST(QuestdbIlpWriter, LegacyBoolTransportFailureIsQuarantinedConservatively)
{
    auto legacy = std::make_unique<LegacyBoolOnlyTransport>();
    auto* legacy_raw = legacy.get();
    IlpWriter w("h", 9009, std::move(legacy),
                /*flush_every_n_lines=*/1000,
                std::chrono::hours{1},
                /*max_pending_bytes=*/8);
    ASSERT_TRUE(w.connect());
    auto fallback = std::make_unique<std::ostringstream>();
    auto* fallback_raw = fallback.get();
    w.enable_fallback(std::move(fallback));
    ASSERT_TRUE(w.enqueue("a\n"));

    EXPECT_FALSE(w.flush());
    EXPECT_TRUE(w.failure_latched());
    EXPECT_TRUE(w.delivery_ambiguous());
    EXPECT_EQ(w.pending_lines(), 1u);
    EXPECT_EQ(w.pending_bytes(), 2u);
    EXPECT_EQ(legacy_raw->connect_calls, 1);
    EXPECT_EQ(legacy_raw->write_calls, 1);
    EXPECT_EQ(fallback_raw->str(), "");

    EXPECT_FALSE(w.flush());
    EXPECT_FALSE(w.enqueue("b\n"));
    EXPECT_EQ(w.dropped_lines(), 1u);
    EXPECT_EQ(legacy_raw->connect_calls, 1);
    EXPECT_EQ(legacy_raw->write_calls, 1);
}

TEST(QuestdbIlpWriter, OversizedLineIsRejectedWithoutGrowingTheBuffer)
{
    auto fake = std::make_unique<FakeTransport>();
    auto* fake_raw = fake.get();
    IlpWriter w("h", 9009, std::move(fake),
                /*flush_every_n_lines=*/1000,
                std::chrono::hours{1},
                /*max_pending_bytes=*/4);
    std::size_t callbacks = 0;
    w.set_failure_callback([&] { ++callbacks; });
    ASSERT_TRUE(w.connect());

    w.enqueue("oversized\n");

    EXPECT_EQ(w.pending_lines(), 0u);
    EXPECT_EQ(w.pending_bytes(), 0u);
    EXPECT_EQ(w.dropped_lines(), 1u);
    EXPECT_TRUE(w.failure_latched());
    EXPECT_EQ(callbacks, 1u);
    EXPECT_TRUE(fake_raw->writes.empty());
}

TEST(QuestdbIlpWriter, ThrowingFailureCallbackDoesNotSkipCloseOrFallback)
{
    auto fake = std::make_unique<FakeTransport>();
    auto* fake_raw = fake.get();
    fake_raw->fail_write_n_times = 1;
    IlpWriter w("h", 9009, std::move(fake), 1000);
    ASSERT_TRUE(w.connect());
    auto fallback = std::make_unique<std::ostringstream>();
    auto* fallback_raw = fallback.get();
    w.enable_fallback(std::move(fallback));
    w.set_failure_callback([] { throw std::runtime_error("callback failure"); });

    w.enqueue("a\n");

    EXPECT_FALSE(w.flush());
    EXPECT_TRUE(w.failure_latched());
    EXPECT_FALSE(w.is_connected());
    EXPECT_EQ(fallback_raw->str(), "a\n");
    EXPECT_EQ(w.fallback_lines(), 1u);
    EXPECT_EQ(w.pending_lines(), 0u);
}

TEST(QuestdbIlpWriter, ThrowingFallbackIsLatchedAndRetainsTheBuffer)
{
    auto fake = std::make_unique<FakeTransport>();
    auto* fake_raw = fake.get();
    fake_raw->fail_write_n_times = 1;
    IlpWriter w("h", 9009, std::move(fake), 1000);
    ASSERT_TRUE(w.connect());
    w.enable_fallback(std::make_unique<ThrowingStream>());
    w.enqueue("a\n");

    EXPECT_FALSE(w.flush());
    EXPECT_TRUE(w.failure_latched());
    EXPECT_FALSE(w.is_connected());
    EXPECT_EQ(w.fallback_lines(), 0u);
    EXPECT_EQ(w.pending_lines(), 1u);
    EXPECT_EQ(w.pending_bytes(), 2u);
}

TEST(QuestdbIlpWriter, BadFallbackDoesNotClaimOrDiscardBufferedLines)
{
    auto fake = std::make_unique<FakeTransport>();
    auto* fake_raw = fake.get();
    IlpWriter w("h", 9009, std::move(fake),
                /*flush_every_n_lines=*/1000,
                std::chrono::hours{1},
                /*max_pending_bytes=*/4);
    ASSERT_TRUE(w.connect());
    auto fallback = std::make_unique<std::ostringstream>();
    fallback->setstate(std::ios::badbit);
    w.enable_fallback(std::move(fallback));
    w.enqueue("a\n");
    w.enqueue("b\n");
    fake_raw->fail_write_n_times = 1;
    w.enqueue("c\n");
    EXPECT_TRUE(w.failure_latched());
    EXPECT_EQ(w.fallback_lines(), 0u);
    EXPECT_EQ(w.pending_lines(), 2u);
    EXPECT_EQ(w.pending_bytes(), 4u);
    EXPECT_EQ(w.dropped_lines(), 1u);
}

#endif // HAS_QUESTDB
