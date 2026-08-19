#include "engine/execution_router.h"

#include "engine/engine_config.h"
#include "execution/execution_adapter.h"
#include "orderbook/orderbook_registry.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

class CountingAdapter final : public IExecutionAdapter
{
public:
    void submit_order(const order_event&) override { ++submits; }
    bool poll_fills(std::vector<fill_event>&) override { return false; }
    int submits = 0;
};

TEST(ExecutionRouterCharacterization, ShadowAndSubmitResultMethodsRemainPartialSeams)
{
    OrderbookRegistry books;
    engine_config cfg;
    std::unordered_set<std::string> seeded;
    std::unordered_map<std::string, std::shared_ptr<IExecutionAdapter>> adapters;
    auto adapter = std::make_shared<CountingAdapter>();
    adapters.emplace("BTCUSDT", adapter);
    ExecutionRouter router(
        books, cfg, seeded, nullptr, adapters);

    order_event order(std::chrono::system_clock::now(), "BTCUSDT",
                      order_type::market, order_side::buy, 1.0);
    router.submit(order, adapter.get());
    EXPECT_EQ(adapter->submits, 1);

    router.drain_submit_results(adapter.get());
    router.submit_to_exchange_shadow(order);
    EXPECT_EQ(adapter->submits, 1)
        << "these methods intentionally characterize the current no-op seam";
}

} // namespace
