#include <gtest/gtest.h>

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