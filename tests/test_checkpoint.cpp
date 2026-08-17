#include <gtest/gtest.h>

#include "engine/checkpoint.h"
#include "engine/engine_config.h"
#include "execution/portfolio.h"

TEST(CheckpointManager, ResumeRefusesBeforePortfolioMutation)
{
    engine_config cfg;
    cfg.resume_checkpoint_path = "unused-v1-checkpoint.bin";
    CheckpointManager manager(cfg);
    portfolio p(12345.0);

    EXPECT_THROW(manager.restore(p), std::logic_error);
    EXPECT_DOUBLE_EQ(p.get_cash(), 12345.0);
    EXPECT_TRUE(p.get_positions().empty());
    EXPECT_EQ(p.get_total_trades(), 0u);
}
