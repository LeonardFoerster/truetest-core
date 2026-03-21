#include <gtest/gtest.h>
#include "threading/thread_preset.h"

TEST(ThreadPreset, SelectPreset_CoreBuckets)
{
    EXPECT_EQ(select_preset(1), thread_preset::inline_mode);
    EXPECT_EQ(select_preset(2), thread_preset::inline_mode);
    EXPECT_EQ(select_preset(3), thread_preset::light);
    EXPECT_EQ(select_preset(4), thread_preset::standard);
    EXPECT_EQ(select_preset(5), thread_preset::standard);
    EXPECT_EQ(select_preset(6), thread_preset::full);
    EXPECT_EQ(select_preset(7), thread_preset::full);
    EXPECT_EQ(select_preset(8), thread_preset::extended);
    EXPECT_EQ(select_preset(64), thread_preset::extended);
}

TEST(ThreadPreset, StringRoundTrip)
{
    for (auto p : {thread_preset::inline_mode, thread_preset::light,
                   thread_preset::standard, thread_preset::full,
                   thread_preset::extended})
    {
        EXPECT_EQ(string_to_preset(preset_to_string(p)), p);
    }
}

TEST(ThreadPreset, StringToPreset_Invalid)
{
    EXPECT_THROW(string_to_preset("bogus"), std::invalid_argument);
}

TEST(ThreadPreset, WorkerCount)
{
    EXPECT_EQ(preset_worker_count(thread_preset::inline_mode), 0);
    EXPECT_EQ(preset_worker_count(thread_preset::light), 1);
    EXPECT_EQ(preset_worker_count(thread_preset::standard), 2);
    EXPECT_EQ(preset_worker_count(thread_preset::full), 3);
    EXPECT_EQ(preset_worker_count(thread_preset::extended), 4);
}

TEST(ThreadPreset, PresetCapabilities)
{
    EXPECT_FALSE(preset_has_mm_worker(thread_preset::full));
    EXPECT_TRUE(preset_has_mm_worker(thread_preset::extended));

    EXPECT_FALSE(preset_has_separate_risk(thread_preset::standard));
    EXPECT_TRUE(preset_has_separate_risk(thread_preset::full));

    EXPECT_FALSE(preset_has_separate_logging(thread_preset::light));
    EXPECT_TRUE(preset_has_separate_logging(thread_preset::standard));
}
