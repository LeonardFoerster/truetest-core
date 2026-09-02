#include "reproducibility/canonical_json.h"
#include "reproducibility/deterministic_rng.h"
#include "reproducibility/deterministic_seed.h"
#include "reproducibility/sha256.h"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <type_traits>

namespace repro = truetest::reproducibility;

static_assert(!std::is_default_constructible_v<repro::DeterministicRng>);

TEST(DeterministicSeed, KnownAnswerVectorsAreStable)
{
    const repro::DeterministicSeedDeriver zero(0);
    EXPECT_EQ(zero.derive(repro::SeedDomain::run),
              0x17cd2c71bdd6f289ULL);
    EXPECT_EQ(zero.trial_seed(0), 0xc63256469589140dULL);
    EXPECT_EQ(zero.trial_seed(42), 0x6200d8423615c0f7ULL);

    const repro::DeterministicSeedDeriver sample(0x123456789abcdef0ULL);
    EXPECT_EQ(sample.derive(repro::SeedDomain::run),
              0xbc08a83b76a83dffULL);
    EXPECT_EQ(sample.trial_seed(0), 0x63066947ad514431ULL);
    EXPECT_EQ(sample.trial_seed(42), 0x5a261346f6d155b3ULL);
}

TEST(DeterministicSeed, DomainsAndTrialIndicesAreIndependentOfCallOrder)
{
    const repro::DeterministicSeedDeriver deriver(987654321ULL);
    const auto trial7 = deriver.trial_seed(7);
    const auto fill7 = deriver.trial_component_seed(
        7, repro::SeedDomain::fill_model);
    const auto trial8 = deriver.trial_seed(8);
    const auto queue7 = deriver.trial_component_seed(
        7, repro::SeedDomain::queue_model);

    EXPECT_EQ(trial7, deriver.trial_seed(7));
    EXPECT_EQ(fill7, deriver.trial_component_seed(
        7, repro::SeedDomain::fill_model));
    EXPECT_EQ(trial8, deriver.trial_seed(8));
    EXPECT_EQ(queue7, deriver.trial_component_seed(
        7, repro::SeedDomain::queue_model));
    EXPECT_NE(trial7, trial8);
    EXPECT_NE(fill7, queue7);
    EXPECT_NE(fill7, trial7);
}

TEST(DeterministicRng, XoshiroKnownAnswerVectorIsStable)
{
    repro::DeterministicRng rng(0x123456789abcdef0ULL);
    EXPECT_EQ(rng.next_u64(), 0x74600579b2508d52ULL);
    EXPECT_EQ(rng.next_u64(), 0x44e864b24859a60fULL);
    EXPECT_EQ(rng.next_u64(), 0x5f31df541de98f9dULL);
    EXPECT_EQ(rng.next_u64(), 0x619ded600407fb43ULL);
    EXPECT_EQ(rng.next_u64(), 0x5642d84dbc18ffc1ULL);
    EXPECT_EQ(rng.next_u64(), 0xb9866c9d03fa0ffeULL);
}

TEST(DeterministicRng, SamplingKnownAnswerVectorsAreStable)
{
    repro::DeterministicRng unit(0x123456789abcdef0ULL);
    for (const std::uint64_t expected : {
             0x3fdd18015e6c9422ULL, 0x3fd13a192c921668ULL,
             0x3fd7cc77d5077a62ULL, 0x3fd8677b580101feULL})
        EXPECT_EQ(std::bit_cast<std::uint64_t>(unit.uniform_unit()), expected);

    repro::DeterministicRng scaled(0x123456789abcdef0ULL);
    for (const std::uint64_t expected : {
             0x3fd17806d81ee4a8ULL, 0xbfe4eec11092c7fcULL,
             0xbfc20351adb53830ULL, 0xbfb7ea5d1febd820ULL})
        EXPECT_EQ(std::bit_cast<std::uint64_t>(scaled.uniform(-2.0, 3.0)),
                  expected);

    repro::DeterministicRng bounded(0x123456789abcdef0ULL);
    for (const std::uint64_t expected : {3U, 2U, 10U, 15U, 0U, 1U, 3U, 6U})
        EXPECT_EQ(bounded.uniform_bounded(17U), expected);

    repro::DeterministicRng normal(42U);
    constexpr std::array normal_bits{
        0x3ff0ba390cddfdd7ULL, 0xbfef0f835b857babULL,
        0x3fe9598bb68d55c2ULL, 0xbfe215622f79dffeULL,
        0xbfff3986e90f2c89ULL, 0xbfb5e4401e1b702fULL,
    };
    for (const std::uint64_t expected : normal_bits)
        EXPECT_EQ(std::bit_cast<std::uint64_t>(normal.standard_normal()),
                  expected);
    normal.reset(42U);
    EXPECT_EQ(std::bit_cast<std::uint64_t>(normal.standard_normal()),
              normal_bits[0]);
    EXPECT_EQ(std::bit_cast<std::uint64_t>(normal.standard_normal()),
              normal_bits[1]);
}

TEST(DeterministicRng, ResetClearsAllStateIncludingNormalCache)
{
    repro::DeterministicRng reused(42);
    const double first = reused.standard_normal();
    const double second = reused.standard_normal();
    (void)reused.next_u64();
    reused.reset(42);

    repro::DeterministicRng fresh(42);
    EXPECT_DOUBLE_EQ(reused.standard_normal(), first);
    EXPECT_DOUBLE_EQ(fresh.standard_normal(), first);
    EXPECT_DOUBLE_EQ(reused.standard_normal(), second);
    EXPECT_DOUBLE_EQ(fresh.standard_normal(), second);
    EXPECT_EQ(reused.next_u64(), fresh.next_u64());
}

TEST(DeterministicRng, UniformAndNormalSamplingRetainExpectedStatistics)
{
    constexpr std::size_t sample_count = 100'000;
    repro::DeterministicRng uniform_rng(11);
    repro::DeterministicRng normal_rng(12);
    double uniform_sum = 0.0;
    double normal_sum = 0.0;
    double normal_squared_sum = 0.0;
    for (std::size_t i = 0; i < sample_count; ++i)
    {
        const double uniform = uniform_rng.uniform_unit();
        EXPECT_GE(uniform, 0.0);
        EXPECT_LT(uniform, 1.0);
        uniform_sum += uniform;

        const double normal = normal_rng.standard_normal();
        normal_sum += normal;
        normal_squared_sum += normal * normal;
    }
    const double count = static_cast<double>(sample_count);
    const double normal_mean = normal_sum / count;
    const double normal_variance = normal_squared_sum / count
        - normal_mean * normal_mean;
    EXPECT_NEAR(uniform_sum / count, 0.5, 0.005);
    EXPECT_NEAR(normal_mean, 0.0, 0.015);
    EXPECT_NEAR(normal_variance, 1.0, 0.025);
}

TEST(Sha256, KnownAnswerAndStreamingVectorsAreStable)
{
    EXPECT_EQ(repro::sha256_hex(""),
              "e3b0c44298fc1c149afbf4c8996fb924"
              "27ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(repro::sha256_hex("abc"),
              "ba7816bf8f01cfea414140de5dae2223"
              "b00361a396177a9cb410ff61f20015ad");

    repro::Sha256 streaming;
    streaming.update("a");
    streaming.update("b");
    streaming.update("c");
    EXPECT_EQ(repro::sha256_hex(streaming.digest()), repro::sha256_hex("abc"));
    EXPECT_EQ(repro::sha256_hex(streaming.digest()), repro::sha256_hex("abc"));
}

TEST(CanonicalSerialization, SortsObjectsAndPreservesArrayOrder)
{
    repro::CanonicalJsonValue::Object object;
    object.emplace("z", 1);
    object.emplace("a", repro::CanonicalJsonValue::array({3, 2, 1}));
    object.emplace("m", "€\n");

    EXPECT_EQ(repro::serialize_canonical_json(object),
              R"({"a":[3,2,1],"m":"€\n","z":1})");
}

TEST(CanonicalSerialization, NormalizesNegativeZeroAndRoundTripsFiniteNumbers)
{
    const auto value = repro::CanonicalJsonValue::array(
        {-0.0, 1.5, 1.0e100, std::uint64_t{18'446'744'073'709'551'615ULL}});
    const std::string bytes = repro::serialize_canonical_json(value);
    EXPECT_EQ(bytes.substr(0, 3), "[0,");
    const auto parsed = repro::parse_json_strict(bytes);
    ASSERT_EQ(parsed.as_array().size(), 4U);
    EXPECT_DOUBLE_EQ(parsed.as_array()[0].as_double(), 0.0);
    EXPECT_DOUBLE_EQ(parsed.as_array()[1].as_double(), 1.5);
    EXPECT_DOUBLE_EQ(parsed.as_array()[2].as_double(), 1.0e100);
    EXPECT_EQ(parsed.as_array()[3].as_u64(),
              std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(repro::serialize_canonical_json(parsed), bytes);
}

TEST(CanonicalSerialization, RejectsNonFiniteInvalidUtf8AndDuplicateKeys)
{
    EXPECT_THROW((void)repro::serialize_canonical_json(
                     std::numeric_limits<double>::quiet_NaN()),
                 std::invalid_argument);
    EXPECT_THROW((void)repro::serialize_canonical_json(
                     std::numeric_limits<double>::infinity()),
                 std::invalid_argument);
    EXPECT_THROW((void)repro::serialize_canonical_json(
                     std::string(1, static_cast<char>(0xff))),
                 std::invalid_argument);
    EXPECT_THROW((void)repro::parse_json_strict(R"({"a":1,"a":2})"),
                 std::invalid_argument);
    EXPECT_THROW((void)repro::parse_json_strict("[1,]"),
                 std::invalid_argument);
}
