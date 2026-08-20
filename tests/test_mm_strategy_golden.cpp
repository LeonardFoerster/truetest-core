// R1 golden + differential suite.
//
// tests/golden/mm/cases.json holds the inputs, tests/golden/mm/expected.json
// the outputs computed by tests/reference/mm_strategy_reference.py — an exact
// rational reimplementation that shares no code with the engine. This test
// runs the C++ strategy over the same inputs and demands agreement.
//
// Regenerating after an intentional semantic change:
//   python3 tests/reference/mm_strategy_reference.py --write
// Verifying the checked-in file is current:
//   python3 tests/reference/mm_strategy_reference.py --check

#include <gtest/gtest.h>

#include "helpers/mm_test_harness.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace truetest::mm;
using namespace truetest::mm::test;

namespace
{

std::filesystem::path golden_mm_dir()
{
    return std::filesystem::path(TEST_FIXTURES_DIR).parent_path() / "golden" / "mm";
}

nlohmann::json load_json(const std::filesystem::path& path)
{
    std::ifstream in(path);
    EXPECT_TRUE(in.good()) << "cannot open " << path;
    nlohmann::json j;
    in >> j;
    return j;
}

std::vector<std::string> sorted_strings(const nlohmann::json& array)
{
    std::vector<std::string> out;
    for (const auto& item : array)
        out.push_back(item.get<std::string>());
    std::sort(out.begin(), out.end());
    return out;
}

const nlohmann::json* find_expected_quote(const nlohmann::json& quotes,
                                          const std::string& side, unsigned level)
{
    for (const auto& q : quotes)
        if (q.at("side").get<std::string>() == side && q.at("level").get<unsigned>() == level)
            return &q;
    return nullptr;
}

} // namespace

TEST(MMStrategyGolden, ReferenceExpectationsMatchTheEngine)
{
    const auto dir = golden_mm_dir();
    const auto base_config_json = load_json(dir / "reference_config.json");
    const auto cases_json = load_json(dir / "cases.json");
    const auto expected_json = load_json(dir / "expected.json");

    const auto& cases = cases_json.at("cases");
    const auto& expected_cases = expected_json.at("cases");
    ASSERT_EQ(cases.size(), expected_cases.size());
    ASSERT_GT(cases.size(), 8u) << "the golden set must keep covering every branch";

    for (std::size_t i = 0; i < cases.size(); ++i)
    {
        const auto& case_json = cases[i];
        const auto& expected = expected_cases[i];
        const std::string name = case_json.at("name").get<std::string>();
        SCOPED_TRACE(name);
        ASSERT_EQ(expected.at("name").get<std::string>(), name);

        // The reference and the engine read the same configuration file, so a
        // divergence can only come from the model, not from setup drift.
        mm_config cfg = default_config();
        apply_config_json(base_config_json, cfg);
        if (case_json.contains("config_overrides"))
            apply_config_json(case_json.at("config_overrides"), cfg);

        auto fixture = parse_fixture(case_json);
        fixture.config = cfg;

        auto strat = make_strategy(fixture.config);
        const auto res = strat.evaluate(fixture.market, fixture.inventory, fixture.context);
        ASSERT_TRUE(res.ok()) << to_string(res.status);
        const auto& d = res.decision;

        EXPECT_EQ(std::string(to_string(d.state)), expected.at("state").get<std::string>());
        EXPECT_EQ(d.market_snapshot_id, expected.at("market_snapshot_id").get<std::uint64_t>());
        EXPECT_EQ(d.market_age_ns, expected.at("market_age_ns").get<std::int64_t>());

        const auto expected_reasons = sorted_strings(expected.at("reason_codes"));
        std::vector<std::string> actual_reasons;
        for (std::size_t r = 0; r < d.reasons.size(); ++r)
            actual_reasons.emplace_back(to_string(d.reasons[r]));
        std::sort(actual_reasons.begin(), actual_reasons.end());
        EXPECT_EQ(actual_reasons, expected_reasons);

        if (d.state == mm_state::paused)
        {
            EXPECT_TRUE(d.intents.empty());
            EXPECT_TRUE(expected.at("quotes").empty());
            continue;
        }

        // Fixed-point results are compared exactly. The reference verifies at
        // generation time that no rounding step sits near a boundary, so the
        // exact/double difference cannot move any of these values.
        EXPECT_EQ(decimal_from_price(d.fair_value),
                  expected.at("fair_value").get<std::string>());
        EXPECT_EQ(decimal_from_price(d.reservation_price),
                  expected.at("reservation_price").get<std::string>());
        EXPECT_EQ(decimal_from_atoms(d.bid_size), expected.at("bid_size").get<std::string>());
        EXPECT_EQ(decimal_from_atoms(d.ask_size), expected.at("ask_size").get<std::string>());

        // Pure-double aggregates carry accumulated rounding, so they get a
        // relative tolerance rather than an exact match.
        EXPECT_NEAR(d.target_half_spread_bps,
                    expected.at("target_half_spread_bps").get<double>(),
                    1e-9 * std::max(1.0, expected.at("target_half_spread_bps").get<double>()));
        EXPECT_NEAR(d.inventory_utilization,
                    expected.at("inventory_utilization").get<double>(), 1e-12);

        const auto& expected_quotes = expected.at("quotes");
        ASSERT_EQ(d.intents.size(), expected_quotes.size());
        for (std::size_t k = 0; k < d.intents.size(); ++k)
        {
            const auto& q = d.intents[k];
            const std::string side = (q.side == order_side::buy) ? "BUY" : "SELL";
            const auto* want = find_expected_quote(expected_quotes, side, q.level);
            ASSERT_NE(want, nullptr) << side << " level " << q.level << " not expected";
            EXPECT_EQ(decimal_from_price(q.price), want->at("price").get<std::string>())
                << side << " level " << q.level;
            EXPECT_EQ(decimal_from_atoms(q.quantity), want->at("quantity").get<std::string>())
                << side << " level " << q.level;
            EXPECT_EQ(q.post_only, want->at("post_only").get<bool>());
        }
    }
}

// Reproducibility anchors. The config hash pins the meaning of the reference
// configuration; the folded decision hash pins the whole golden run. Either
// changing is a semantic change that must be argued for, not absorbed.
TEST(MMStrategyGolden, ConfigAndResultHashesAreStable)
{
    const auto dir = golden_mm_dir();
    const auto base_config_json = load_json(dir / "reference_config.json");
    const auto cases_json = load_json(dir / "cases.json");

    mm_config reference = default_config();
    apply_config_json(base_config_json, reference);
    const std::uint64_t reference_config_hash = config_hash(reference);

    std::uint64_t folded = 1469598103934665603ULL;
    for (const auto& case_json : cases_json.at("cases"))
    {
        mm_config cfg = reference;
        if (case_json.contains("config_overrides"))
            apply_config_json(case_json.at("config_overrides"), cfg);

        auto fixture = parse_fixture(case_json);
        fixture.config = cfg;

        auto strat = make_strategy(fixture.config);
        const auto d = strat.evaluate(fixture.market, fixture.inventory,
                                      fixture.context).decision;
        folded ^= decision_hash(d);
        folded *= 1099511628211ULL;
    }

    std::printf("\n=== R1 reproducibility anchors ===\n");
    std::printf("reference config_hash : 0x%016llX\n",
                static_cast<unsigned long long>(reference_config_hash));
    std::printf("golden result_hash    : 0x%016llX\n\n",
                static_cast<unsigned long long>(folded));

    EXPECT_EQ(reference_config_hash, 0x834DD815BAEB86F4ULL);
    EXPECT_EQ(folded, 0x73FA545867D95FFEULL);
}

// The golden set is only meaningful if it still exercises every terminal
// state; this guards against a future edit quietly deleting the fail-closed
// cases.
TEST(MMStrategyGolden, GoldenSetCoversEveryTerminalState)
{
    const auto expected_json = load_json(golden_mm_dir() / "expected.json");
    bool has_active = false;
    bool has_reducing = false;
    bool has_paused = false;
    bool has_hard_limit = false;
    bool has_stale = false;
    bool has_gap = false;
    bool has_unknown_inventory = false;
    bool has_multi_level = false;

    for (const auto& c : expected_json.at("cases"))
    {
        const auto state = c.at("state").get<std::string>();
        has_active |= (state == "ACTIVE");
        has_reducing |= (state == "REDUCING_ONLY");
        has_paused |= (state == "PAUSED");
        for (const auto& r : c.at("reason_codes"))
        {
            const auto reason = r.get<std::string>();
            has_hard_limit |= (reason == "INVENTORY_HARD_LIMIT");
            has_stale |= (reason == "STALE_MARKET_DATA");
            has_gap |= (reason == "SEQUENCE_GAP");
            has_unknown_inventory |= (reason == "UNKNOWN_INVENTORY");
        }
        for (const auto& q : c.at("quotes"))
            has_multi_level |= (q.at("level").get<unsigned>() > 0);
    }

    EXPECT_TRUE(has_active);
    EXPECT_TRUE(has_reducing);
    EXPECT_TRUE(has_paused);
    EXPECT_TRUE(has_hard_limit);
    EXPECT_TRUE(has_stale);
    EXPECT_TRUE(has_gap);
    EXPECT_TRUE(has_unknown_inventory);
    EXPECT_TRUE(has_multi_level);
}
