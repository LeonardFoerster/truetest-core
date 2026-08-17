#include "checkpoint.h"

#include "engine_config.h"
#include "execution/portfolio.h"

#include <chrono>
#include <iostream>

CheckpointManager::CheckpointManager(const engine_config& cfg)
    : cfg_(cfg)
{
}

void CheckpointManager::write_if_due(const portfolio& p, std::size_t event_count)
{
    if (cfg_.checkpoint_path.empty()) return;
    if (cfg_.checkpoint_interval_events == 0) return;
    if (event_count == 0 || event_count % cfg_.checkpoint_interval_events != 0) return;

    try {
        auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        checkpoint::write_file(cfg_.checkpoint_path, p,
                               static_cast<uint64_t>(event_count), wall_ms);
    } catch (const std::exception& e) {
        std::cerr << "[checkpoint] write failed: " << e.what() << std::endl;
    }
}

void CheckpointManager::restore(portfolio& p)
{
    if (cfg_.resume_checkpoint_path.empty()) return;

    (void)p;
    throw std::logic_error(
        "checkpoint resume is unavailable: version 1 snapshots do not contain "
        "complete deterministic engine state");
}
