#include <gtest/gtest.h>

#include "strategy/symbol_state_store.h"
#include "types/symbol_table.h"

TEST(SymbolTable, InternReturnsCanonicalReference)
{
    SymbolTable table;
    const std::string a = "BTCUSDT";
    const std::string b = "BTCUSDT";
    const std::string& ref1 = table.intern(a);
    const std::string& ref2 = table.intern(b);
    EXPECT_EQ(&ref1, &ref2);
    EXPECT_EQ(table.id_of("BTCUSDT"), 0u);
    EXPECT_EQ(table.resolve(0), "BTCUSDT");
}

TEST(SymbolTable, DistinctSymbolsGetDistinctIds)
{
    SymbolTable table;
    table.intern("ETHUSDT");
    table.intern("BTCUSDT");
    EXPECT_EQ(table.size(), 2u);
    EXPECT_EQ(table.id_of("ETHUSDT"), 0u);
    EXPECT_EQ(table.id_of("BTCUSDT"), 1u);
}

TEST(SymbolTable, InternIdIsStableAndDense)
{
    SymbolTable table;
    EXPECT_EQ(table.intern_id("A"), 0u);
    EXPECT_EQ(table.intern_id("B"), 1u);
    EXPECT_EQ(table.intern_id("A"), 0u); // re-intern same id
    EXPECT_EQ(table.resolve(1), "B");
    EXPECT_EQ(table.id_of("missing"), SymbolTable::kInvalidId);
}

TEST(SymbolStateStore, DenseSlotAndInternedSymbol)
{
    int factory_calls = 0;
    SymbolStateStore<int> store([&]() {
        ++factory_calls;
        return 42;
    });

    auto a1 = store.get("ETH");
    auto a2 = store.get("ETH");
    EXPECT_EQ(a1.id, a2.id);
    EXPECT_EQ(&a1.state, &a2.state);
    EXPECT_EQ(a1.state, 42);
    EXPECT_EQ(&a1.symbol, &a2.symbol); // same interned buffer
    EXPECT_EQ(factory_calls, 1);       // one construction per symbol

    auto b = store.get("BTC");
    EXPECT_NE(a1.id, b.id);
    EXPECT_EQ(factory_calls, 2);
    EXPECT_EQ(store.size(), 2u);

    EXPECT_NE(store.find("ETH"), nullptr);
    EXPECT_EQ(store.find("XRP"), nullptr);
}