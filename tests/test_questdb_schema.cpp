#ifdef HAS_QUESTDB

#include "data/questdb/schema.h"

#include <gtest/gtest.h>

namespace ts = truetest::questdb::schema;

TEST(QuestdbSchema, RunsMetaHasStartedAtAsDesignatedTimestamp)
{
    const std::string ddl = ts::runs_meta_ddl();
    EXPECT_NE(ddl.find("TIMESTAMP(started_at) PARTITION BY WEEK"),
              std::string::npos);
}

TEST(QuestdbSchema, PerRunDdlsCount)
{
    EXPECT_EQ(ts::per_run_ddls("myrun").size(), 8u);
}

TEST(QuestdbSchema, PerRunDdlsPrefix)
{
    const auto ddls = ts::per_run_ddls("myrun");
    for (const auto& d : ddls)
    {
        EXPECT_EQ(d.find("CREATE TABLE IF NOT EXISTS myrun_"), 0u)
            << "ddl=" << d.substr(0, 80);
    }
}

TEST(QuestdbSchema, PerRunDdlsTables)
{
    const auto ddls = ts::per_run_ddls("p");
    const std::string suffixes[] = {"_orders", "_order_status", "_fills",
                                    "_funding", "_events",
                                    "_rejections", "_cancellations",
                                    "_amendments"};
    ASSERT_EQ(ddls.size(), 8u);
    for (std::size_t i = 0; i < 8; ++i)
    {
        EXPECT_NE(ddls[i].find("p" + suffixes[i] + " ("), std::string::npos)
            << "ddl=" << ddls[i].substr(0, 80);
    }
}

TEST(QuestdbSchema, OrdersDdlColumnsMatchAppendixA)
{
    const auto ddls = ts::per_run_ddls("p");
    const std::string& d = ddls[0];
    EXPECT_NE(d.find("order_id         LONG"), std::string::npos);
    EXPECT_NE(d.find("opener_order_id  LONG"), std::string::npos);
    EXPECT_NE(d.find("symbol           SYMBOL"), std::string::npos);
    EXPECT_NE(d.find("side             SYMBOL"), std::string::npos);
    EXPECT_NE(d.find("strategy_name    SYMBOL"), std::string::npos);
    EXPECT_NE(d.find("initial_status   SYMBOL"), std::string::npos);
}

TEST(QuestdbSchema, FillsDdlHasSourceColumn)
{
    const auto ddls = ts::per_run_ddls("p");
    const std::string& d = ddls[2]; // fills
    EXPECT_NE(d.find("source           SYMBOL CAPACITY 8"), std::string::npos);
}

TEST(QuestdbSchema, AllPerRunPartitionByDay)
{
    for (const auto& d : ts::per_run_ddls("p"))
    {
        EXPECT_NE(d.find("PARTITION BY DAY"), std::string::npos);
    }
}

TEST(QuestdbSchema, AllDdlsStartsWithRunsMeta)
{
    const auto ddls = ts::all_ddls("p");
    ASSERT_EQ(ddls.size(), 9u);
    EXPECT_EQ(ddls[0], ts::runs_meta_ddl());
}

#endif // HAS_QUESTDB
