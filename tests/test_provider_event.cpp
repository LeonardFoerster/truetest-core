#include <gtest/gtest.h>
#include "providers/provider_event.h"
#include "providers/provider_convert.h"
#include "providers/provider_sink.h"
#include "data/data_handler.h"

#include <chrono>

static auto epoch_ms(int64_t ms)
{
	return std::chrono::system_clock::time_point(std::chrono::milliseconds(ms));
}

// --- Conversion round-trips ---

TEST(ProviderEvent, BarRoundTrip)
{
	provider::bar b;
	b.date = "2024-01-01";
	b.symbol = "AAPL";
	b.open = 150.0;
	b.high = 155.0;
	b.low = 149.0;
	b.close = 153.0;
	b.volume = 1000000;

	auto rec = provider::to_bar_record(b);
	EXPECT_EQ(rec.date, "2024-01-01");
	EXPECT_EQ(rec.symbol, "AAPL");
	EXPECT_DOUBLE_EQ(rec.open, 150.0);
	EXPECT_DOUBLE_EQ(rec.high, 155.0);
	EXPECT_DOUBLE_EQ(rec.low, 149.0);
	EXPECT_DOUBLE_EQ(rec.close, 153.0);
	EXPECT_EQ(rec.volume, 1000000);

	auto back = provider::from_bar_record(rec);
	EXPECT_EQ(back.date, b.date);
	EXPECT_EQ(back.symbol, b.symbol);
	EXPECT_DOUBLE_EQ(back.open, b.open);
	EXPECT_DOUBLE_EQ(back.close, b.close);
	EXPECT_EQ(back.volume, b.volume);
}

TEST(ProviderEvent, TickRoundTrip)
{
	provider::tick t;
	t.timestamp = epoch_ms(1704067200000);
	t.symbol = "BTCUSDT";
	t.price = 42000.50;
	t.quantity = 100;
	t.side = 0;  // bid

	auto rec = provider::to_tick_record(t);
	EXPECT_EQ(rec.symbol, "BTCUSDT");
	EXPECT_DOUBLE_EQ(rec.price, 42000.50);
	EXPECT_EQ(rec.quantity, 100);
	EXPECT_EQ(rec.side, data_tick_side::bid);
	EXPECT_EQ(rec.timestamp, epoch_ms(1704067200000));

	auto back = provider::from_tick_record(rec);
	EXPECT_EQ(back.symbol, t.symbol);
	EXPECT_DOUBLE_EQ(back.price, t.price);
	EXPECT_EQ(back.quantity, t.quantity);
	EXPECT_EQ(back.side, t.side);
}

TEST(ProviderEvent, TickSideMapping)
{
	// bid=0
	provider::tick bid;
	bid.side = 0;
	EXPECT_EQ(provider::to_tick_record(bid).side, data_tick_side::bid);

	// ask=1
	provider::tick ask;
	ask.side = 1;
	EXPECT_EQ(provider::to_tick_record(ask).side, data_tick_side::ask);

	// unknown=2
	provider::tick unk;
	unk.side = 2;
	EXPECT_EQ(provider::to_tick_record(unk).side, data_tick_side::unknown);
}

// --- Event variant ---

TEST(ProviderEvent, VariantHoldsBar)
{
	provider::bar b;
	b.symbol = "TEST";
	provider::event ev = b;
	ASSERT_TRUE(std::holds_alternative<provider::bar>(ev));
	EXPECT_EQ(std::get<provider::bar>(ev).symbol, "TEST");
}

TEST(ProviderEvent, VariantHoldsTick)
{
	provider::tick t{};
	t.price = 99.9;
	provider::event ev = t;
	ASSERT_TRUE(std::holds_alternative<provider::tick>(ev));
	EXPECT_DOUBLE_EQ(std::get<provider::tick>(ev).price, 99.9);
}

TEST(ProviderEvent, VariantHoldsL2Snapshot)
{
	provider::l2_snapshot snap;
	snap.symbol = "ETHUSDT";
	snap.bids.push_back({3000.0, 10});
	provider::event ev = snap;
	ASSERT_TRUE(std::holds_alternative<provider::l2_snapshot>(ev));
	EXPECT_EQ(std::get<provider::l2_snapshot>(ev).bids.size(), 1u);
}

TEST(ProviderEvent, VariantHoldsStatus)
{
	provider::status s;
	s.provider = "binance";
	s.message = "connected";
	s.type = provider::status::kind::connected;
	provider::event ev = s;
	ASSERT_TRUE(std::holds_alternative<provider::status>(ev));
	EXPECT_EQ(std::get<provider::status>(ev).provider, "binance");
}

// --- Event sink ---

TEST(ProviderEvent, SinkBarPopulatesHandler)
{
	auto dh = std::make_shared<data_handler>();

	provider::bar b;
	b.date = "2024-01-01";
	b.symbol = "AAPL";
	b.open = 150.0;
	b.high = 155.0;
	b.low = 149.0;
	b.close = 153.0;
	b.volume = 1000000;

	provider::event ev = b;
	provider::event_sink(ev, dh);

	ASSERT_EQ(dh->db_data_close_value.size(), 1u);
	EXPECT_DOUBLE_EQ(dh->db_data_close_value[0], 153.0);
	EXPECT_EQ(dh->db_data_symbol[0], "AAPL");
}

TEST(ProviderEvent, SinkTickPopulatesHandler)
{
	auto dh = std::make_shared<data_handler>();

	provider::tick t;
	t.timestamp = epoch_ms(1704067200000);
	t.symbol = "BTCUSDT";
	t.price = 42000.0;
	t.quantity = 10;
	t.side = 1;

	provider::event ev = t;
	provider::event_sink(ev, dh);

	ASSERT_EQ(dh->tick_data.size(), 1u);
	EXPECT_DOUBLE_EQ(dh->tick_data[0].price, 42000.0);
	EXPECT_EQ(dh->tick_data[0].side, data_tick_side::ask);
}

TEST(ProviderEvent, SinkStatusNoCrash)
{
	auto dh = std::make_shared<data_handler>();

	provider::status s;
	s.provider = "test";
	s.message = "ok";
	s.type = provider::status::kind::info;

	provider::event ev = s;
	provider::event_sink(ev, dh);

	// No data should be stored
	EXPECT_FALSE(dh->has_bar_data());
	EXPECT_FALSE(dh->has_tick_data());
}

TEST(ProviderEvent, SinkL2SnapshotNoCrash)
{
	auto dh = std::make_shared<data_handler>();

	provider::l2_snapshot snap;
	snap.symbol = "TEST";
	snap.bids.push_back({100.0, 5});
	snap.asks.push_back({101.0, 3});

	provider::event ev = snap;
	provider::event_sink(ev, dh);

	EXPECT_FALSE(dh->has_bar_data());
	EXPECT_FALSE(dh->has_tick_data());
}
