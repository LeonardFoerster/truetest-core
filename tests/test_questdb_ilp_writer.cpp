#ifdef HAS_QUESTDB

#include "data/questdb/ilp_writer.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

using truetest::questdb::IIlpTransport;
using truetest::questdb::IlpWriter;
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
        write_calls++;
        if (fail_write_n_times > 0)
        {
            fail_write_n_times--;
            return false;
        }
        writes.emplace_back(data);
        return true;
    }
    void close() override { connected_ = false; }
    bool is_connected() const override { return connected_; }

    std::vector<std::string> writes;
    int connect_calls = 0;
    int write_calls = 0;
    int fail_connect_n_times = 0;
    int fail_write_n_times = 0;

private:
    bool connected_ = false;
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
    ASSERT_EQ(fake_raw->writes.size(), 1u);
    EXPECT_EQ(fake_raw->writes[0], "hello\n");
    EXPECT_GE(fake_raw->connect_calls, 2);
}

#endif // HAS_QUESTDB
