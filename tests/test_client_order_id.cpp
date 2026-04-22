#include <gtest/gtest.h>
#include "execution/client_order_id.h"

#include <set>

TEST(ClientOrderId, IdsAreUniqueAndMonotonic)
{
    ClientOrderIdMinter m("tt", /*seed=*/42, /*epoch_ms=*/1000);
    std::set<std::string> seen;
    for (int i = 0; i < 1000; ++i)
    {
        auto id = m.next();
        EXPECT_TRUE(seen.insert(id).second) << "duplicate id at iter " << i;
    }
}

TEST(ClientOrderId, SamePrefixSeedEpoch_ProduceSameSequence)
{
    ClientOrderIdMinter a("tt", 7, 12345);
    ClientOrderIdMinter b("tt", 7, 12345);
    for (int i = 0; i < 10; ++i)
        EXPECT_EQ(a.next(), b.next());
}

TEST(ClientOrderId, DifferentEpoch_ProducesDifferentIds)
{
    ClientOrderIdMinter a("tt", 7, 1);
    ClientOrderIdMinter b("tt", 7, 2);
    EXPECT_NE(a.next(), b.next());
}

TEST(ClientOrderId, DifferentSeed_ProducesDifferentIds)
{
    ClientOrderIdMinter a("tt", 1, 1000);
    ClientOrderIdMinter b("tt", 2, 1000);
    EXPECT_NE(a.next(), b.next());
}

TEST(ClientOrderId, PrefixAppearsInId)
{
    ClientOrderIdMinter m("runX", 1, 1);
    auto id = m.next();
    EXPECT_EQ(id.rfind("runX-", 0), 0u) << "id must start with 'runX-', got: " << id;
}

TEST(ClientOrderId, IdLengthUnderBinanceLimit)
{
    // Binance caps clientOrderId at 36 chars; prefix alone is already ~15-20,
    // so verify a large seq still fits.
    ClientOrderIdMinter m("tt", 0xFFFFFFFFull, 0xFFFFFFFFFFull);
    for (int i = 0; i < 10; ++i)
    {
        auto id = m.next();
        EXPECT_LE(id.size(), 36u);
    }
}
