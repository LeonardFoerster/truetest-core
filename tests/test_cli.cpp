#include <gtest/gtest.h>
#include <cstdlib>
#include <cstdio>
#include <array>
#include <string>
#include <fstream>

// Helper: run truetest with args, capture stdout+stderr, return exit code
static int run_truetest(const std::string& args, std::string& output)
{
    std::string cmd = "./build/truetest " + args + " 2>&1";
    std::array<char, 4096> buf;
    output.clear();

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return -1;

    while (fgets(buf.data(), buf.size(), pipe))
        output += buf.data();

    int status = pclose(pipe);
    // pclose returns the process exit status encoded; extract it
    return WEXITSTATUS(status);
}

// ─── B1: CLI11 parsing ─────────────────────────────────────────────────────

TEST(CLI, HelpExitsCleanly)
{
    std::string out;
    int rc = run_truetest("--help", out);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("--strategy"), std::string::npos);
    EXPECT_NE(out.find("--provider"), std::string::npos);
    EXPECT_NE(out.find("--balance"), std::string::npos);
}

TEST(CLI, UnknownFlagRejected)
{
    std::string out;
    int rc = run_truetest("--nonexistent-flag", out);
    EXPECT_NE(rc, 0);
    EXPECT_NE(out.find("not expected"), std::string::npos);
}

TEST(CLI, InvalidStrategyDetectedByDryRun)
{
    std::string out;
    int rc = run_truetest("--dry-run --strategy bogus", out);
    EXPECT_EQ(rc, 1);
    EXPECT_NE(out.find("Unknown strategy"), std::string::npos);
}

TEST(CLI, InvalidModeDetectedByDryRun)
{
    std::string out;
    int rc = run_truetest("--dry-run --mode foobar", out);
    EXPECT_EQ(rc, 1);
    EXPECT_NE(out.find("Unknown mode"), std::string::npos);
}

// ─── B2: JSON config file ──────────────────────────────────────────────────

TEST(ConfigFile, LoadsValuesFromJSON)
{
    // Write temp config
    std::string cfg_path = "/tmp/truetest_test_config.json";
    {
        std::ofstream f(cfg_path);
        f << R"({"strategy":"sma","balance":77777,"sma_period":99})";
    }

    std::string out;
    int rc = run_truetest("--config " + cfg_path + " --dump-config", out);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("\"strategy\": \"sma\""), std::string::npos);
    EXPECT_NE(out.find("\"balance\": 77777.0"), std::string::npos);
    EXPECT_NE(out.find("\"sma_period\": 99"), std::string::npos);

    std::remove(cfg_path.c_str());
}

TEST(ConfigFile, CLIOverridesConfigFile)
{
    std::string cfg_path = "/tmp/truetest_test_override.json";
    {
        std::ofstream f(cfg_path);
        f << R"({"balance":99999,"strategy":"sma"})";
    }

    std::string out;
    int rc = run_truetest("--config " + cfg_path + " --balance 1234 --dump-config", out);
    EXPECT_EQ(rc, 0);
    // CLI --balance should win over config file
    EXPECT_NE(out.find("\"balance\": 1234.0"), std::string::npos);
    // Config file strategy should still apply
    EXPECT_NE(out.find("\"strategy\": \"sma\""), std::string::npos);

    std::remove(cfg_path.c_str());
}

TEST(ConfigFile, InvalidJSONExitsWithError)
{
    std::string cfg_path = "/tmp/truetest_test_bad.json";
    {
        std::ofstream f(cfg_path);
        f << "{invalid json!!!";
    }

    std::string out;
    int rc = run_truetest("--config " + cfg_path + " --dump-config", out);
    EXPECT_NE(rc, 0);
    EXPECT_NE(out.find("parse error"), std::string::npos);

    std::remove(cfg_path.c_str());
}

TEST(ConfigFile, MissingConfigFileExitsWithError)
{
    std::string out;
    int rc = run_truetest("--config /tmp/nonexistent_12345.json --dump-config", out);
    EXPECT_NE(rc, 0);
    EXPECT_NE(out.find("Cannot open config file"), std::string::npos);
}

TEST(ConfigFile, RiskLimitsFromJSON)
{
    std::string cfg_path = "/tmp/truetest_test_risk.json";
    {
        std::ofstream f(cfg_path);
        f << R"({"risk":{"max_drawdown":0.15,"max_open_orders":50}})";
    }

    std::string out;
    int rc = run_truetest("--config " + cfg_path + " --dump-config", out);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("\"max_drawdown\": 0.15"), std::string::npos);
    EXPECT_NE(out.find("\"max_open_orders\": 50"), std::string::npos);

    std::remove(cfg_path.c_str());
}

// ─── B2: --dump-config ─────────────────────────────────────────────────────

TEST(DumpConfig, OutputsValidJSON)
{
    std::string out;
    int rc = run_truetest("--dump-config", out);
    EXPECT_EQ(rc, 0);
    // Should contain opening brace (JSON object)
    EXPECT_NE(out.find("{"), std::string::npos);
    EXPECT_NE(out.find("\"strategy\""), std::string::npos);
    EXPECT_NE(out.find("\"balance\""), std::string::npos);
    EXPECT_NE(out.find("\"risk\""), std::string::npos);
}

TEST(DumpConfig, ReflectsCLIValues)
{
    std::string out;
    int rc = run_truetest("--dump-config --balance 42000 --seed 999", out);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("\"balance\": 42000.0"), std::string::npos);
    EXPECT_NE(out.find("\"seed\": 999"), std::string::npos);
}

// ─── B3: --dry-run ─────────────────────────────────────────────────────────

TEST(DryRun, ValidConfigExitsZero)
{
    std::string out;
    int rc = run_truetest("--dry-run --strategy sma --mode backtest", out);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("Config is VALID"), std::string::npos);
    EXPECT_NE(out.find("Strategy:   sma"), std::string::npos);
}

TEST(DryRun, ShowsBalanceAndRisk)
{
    std::string out;
    int rc = run_truetest("--dry-run --balance 50000 --risk-fraction 0.05", out);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("$50000"), std::string::npos);
    EXPECT_NE(out.find("5%"), std::string::npos);
}

TEST(DryRun, InvalidFeeModelExitsOne)
{
    std::string out;
    int rc = run_truetest("--dry-run --fee badmodel", out);
    EXPECT_EQ(rc, 1);
    EXPECT_NE(out.find("Unknown fee model"), std::string::npos);
}
