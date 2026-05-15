#include <gtest/gtest.h>
#include "providers/provider_registry.h"
#include "providers/local/local_provider.h"

#include <sstream>

static std::string fixture_path(const std::string& name)
{
	return std::string(TEST_FIXTURES_DIR) + "/" + name;
}

// RAII helper to silence stdout/stderr.
// Members declared in init order to avoid using `sink` before construction.
namespace {
struct SilenceRegistry {
	std::ostringstream sink;
	std::streambuf* orig_out;
	std::streambuf* orig_err;
	SilenceRegistry()
		: sink()
		, orig_out(std::cout.rdbuf(sink.rdbuf()))
		, orig_err(std::cerr.rdbuf(sink.rdbuf())) {}
	~SilenceRegistry() {
		std::cout.rdbuf(orig_out);
		std::cerr.rdbuf(orig_err);
	}
};
}

TEST(ProviderRegistry, CreateThrowsForUnregistered)
{
	EXPECT_THROW(
		ProviderRegistry::instance().create("nonexistent_provider_xyz"),
		std::runtime_error
	);
}

TEST(ProviderRegistry, HasReturnsFalseForUnregistered)
{
	EXPECT_FALSE(ProviderRegistry::instance().has("nonexistent_provider_xyz"));
}

TEST(ProviderRegistry, LocalIsRegistered)
{
	// "local" is registered via static init in local_register.cpp
	EXPECT_TRUE(ProviderRegistry::instance().has("local"));
}

TEST(ProviderRegistry, AvailableContainsLocal)
{
	auto names = ProviderRegistry::instance().available();
	bool found = false;
	for (const auto& n : names)
	{
		if (n == "local")
		{
			found = true;
			break;
		}
	}
	EXPECT_TRUE(found);
}

TEST(ProviderRegistry, CreateLocalWithPathSucceeds)
{
	SilenceRegistry quiet;
	provider_config cfg;
	cfg["path"] = fixture_path("sample_ohlcv.csv");

	auto provider = ProviderRegistry::instance().create("local", cfg);
	ASSERT_NE(provider, nullptr);
	EXPECT_EQ(provider->name(), "local");
}

TEST(ProviderRegistry, CreateLocalWithoutPathThrows)
{
	provider_config cfg;  // no "path" key
	EXPECT_THROW(
		ProviderRegistry::instance().create("local", cfg),
		std::runtime_error
	);
}

// --- LocalProvider tests ---

TEST(LocalProvider, HasDataFeedTrue)
{
	LocalProvider lp(fixture_path("sample_ohlcv.csv"));
	EXPECT_TRUE(lp.has_data_feed());
}

TEST(LocalProvider, HasExecutionFalse)
{
	LocalProvider lp(fixture_path("sample_ohlcv.csv"));
	EXPECT_FALSE(lp.has_execution());
}

TEST(LocalProvider, GetExecutionAdapterReturnsNull)
{
	LocalProvider lp(fixture_path("sample_ohlcv.csv"));
	EXPECT_EQ(lp.get_execution_adapter(), nullptr);
}

TEST(LocalProvider, OpenSucceeds)
{
	LocalProvider lp(fixture_path("sample_ohlcv.csv"));
	ASSERT_TRUE(lp.open());
	EXPECT_NE(lp.get_transport(), nullptr);
	lp.close();
}

TEST(LocalProvider, TransportIsNotStreaming)
{
	LocalProvider lp(fixture_path("sample_ohlcv.csv"));
	ASSERT_TRUE(lp.open());
	EXPECT_FALSE(lp.get_transport()->is_streaming());
	lp.close();
}

TEST(LocalProvider, TransportReadsLines)
{
	SilenceRegistry quiet;
	LocalProvider lp(fixture_path("sample_ohlcv.csv"));
	ASSERT_TRUE(lp.open());

	auto transport = lp.get_transport();
	auto line1 = transport->read_line();
	ASSERT_TRUE(line1.has_value());  // header
	auto line2 = transport->read_line();
	ASSERT_TRUE(line2.has_value());  // first data row
	EXPECT_NE(line2->find("AAPL"), std::string::npos);

	lp.close();
}

TEST(LocalProvider, OpenFailsForNonexistent)
{
	LocalProvider lp("/nonexistent/path/to/data.csv");
	EXPECT_FALSE(lp.open());
}

// --- Custom provider registration ---

class MockProvider : public IProvider
{
public:
	std::string name() const override { return "mock"; }
	bool has_data_feed() const override { return false; }
	bool has_execution() const override { return false; }
	bool open() override { return true; }
	void close() override {}
	std::shared_ptr<IDataTransport> get_transport() override { return nullptr; }
	std::shared_ptr<IExecutionAdapter> get_execution_adapter() override { return nullptr; }
};

TEST(ProviderRegistry, RegisterAndCreateCustomProvider)
{
	ProviderRegistry::instance().register_provider("test_mock",
		[](const provider_config&) {
			return std::make_shared<MockProvider>();
		});

	EXPECT_TRUE(ProviderRegistry::instance().has("test_mock"));

	auto p = ProviderRegistry::instance().create("test_mock");
	ASSERT_NE(p, nullptr);
	EXPECT_EQ(p->name(), "mock");
}
