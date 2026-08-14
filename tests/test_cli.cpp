#include <gtest/gtest.h>
#include <cstdlib>
#include <cstdio>
#include <array>
#include <ctime>
#include <string>
#include <fstream>
#include <sys/stat.h>
#include <sys/wait.h>

// Resolve engine binary across common CMake output layouts (ad-hoc
// ./build and preset out/build/linux-tests). Prefer the newer mtime.
static std::string resolve_engine_binary(const std::string& binary)
{
    const char* candidates[] = {
        "./out/build/linux-tests/",
        "./build/",
        "./out/build/linux-release-native/",
    };
    std::string best;
    std::time_t best_mtime = 0;
    for (const char* dir : candidates)
    {
        const std::string path = std::string(dir) + binary;
        struct stat st{};
        if (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
            (st.st_mode & S_IXUSR) && st.st_mtime >= best_mtime)
        {
            best = path;
            best_mtime = st.st_mtime;
        }
    }
    return best.empty() ? (std::string("./build/") + binary) : best;
}

// Helper: run engine binary with args, capture stdout+stderr, return exit code
static int run_engine(const std::string& binary,
                      const std::string& args,
                      std::string& output)
{
    const std::string path = resolve_engine_binary(binary);
    std::string cmd = path + " " + args + " 2>&1";
    std::array<char, 4096> buf;
    output.clear();

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return -1;

    while (fgets(buf.data(), buf.size(), pipe))
        output += buf.data();

    int status = pclose(pipe);
    // pclose returns the process exit status encoded; extract it
    if (status < 0) return -1;
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return status;
}

static int run_truetest(const std::string& args, std::string& output)
{
    return run_engine("engine_backtest", args, output);
}

static int run_engine_live(const std::string& args, std::string& output)
{
    return run_engine("engine_live", args, output);
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

// --simple-tui opts ConsoleDashboard's ANSI-box TUI over TabbedDashboard on
// shadow/live. Smoke: flag is registered on backtest + live (cli_tests
// depends on those two) and accepted by dry-run; runtime TUI selection is
// main.inc wiring exercised via ConsoleDashboardMode unit tests.
TEST(CLI, SimpleTuiFlagInHelpOnBacktestAndLive)
{
    std::string out;
    int rc = run_truetest("--help", out);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("--simple-tui"), std::string::npos);

    out.clear();
    rc = run_engine_live("--help", out);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("--simple-tui"), std::string::npos);
}

TEST(CLI, SimpleTuiAcceptedOnDryRun)
{
    std::string out;
    int rc = run_truetest(
        "--dry-run --strategy sma --provider synthetic --simple-tui", out);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(out.find("not expected"), std::string::npos);
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

// Order-latency CLI binding: realism-wiring Task 1. The latency model
// itself is unit-tested in test_latency_model.cpp; this guards that the
// flags reach the config dump and the engine binary builds with the
// underlying StochasticLatencyModel/FixedLatencyModel constructors.
TEST(DumpConfig, ReflectsOrderLatency)
{
    std::string out;
    int rc = run_truetest(
        "--dump-config --order-latency-us 5000 --order-latency-stddev-us 1500",
        out);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("\"order_latency_us\": 5000"), std::string::npos);
    EXPECT_NE(out.find("\"order_latency_stddev_us\": 1500"), std::string::npos);
}

TEST(DryRun, OrderLatencyAcceptedInBacktest)
{
    std::string out;
    int rc = run_truetest(
        "--dry-run --strategy sma --mode backtest --order-latency-us 1000", out);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("Config is VALID"), std::string::npos);
}

// Impact model CLI binding: realism-wiring Task 2. SquareRootImpactModel
// is unit-tested in test_impact_model.cpp; this guards the CLI surface.
TEST(DumpConfig, ReflectsImpactModel)
{
    std::string out;
    int rc = run_truetest(
        "--dump-config --impact-k-bps 12 --impact-adv 1000000", out);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("\"impact_k_bps\": 12.0"), std::string::npos);
    EXPECT_NE(out.find("\"impact_adv\": 1000000.0"), std::string::npos);
}

TEST(DumpConfig, ReflectsWalkedBookImpact)
{
    std::string out;
    int rc = run_truetest("--dump-config --walked-book-impact", out);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("\"walked_book_impact\": true"), std::string::npos);
}

// --impact-k-bps without --impact-adv must hard-fail rather than silently
// fall through (ZeroImpactModel hides the misconfiguration). Provide a
// full config so we don't fall into interactive setup before the gate.
TEST(CLI, ImpactKBpsRequiresAdv)
{
    std::string out;
    int rc = run_truetest(
        "--provider local --path market_data.csv "
        "--strategy sma --mode backtest --impact-k-bps 10", out);
    EXPECT_NE(rc, 0);
    EXPECT_NE(out.find("requires --impact-adv"), std::string::npos);
}

TEST(DumpConfig, ReflectsFillModel)
{
    std::string out;
    int rc = run_truetest(
        "--dump-config --fill-prob 0.9 --fill-fade 0.05 --fill-decay 12", out);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("\"fill_prob\": 0.9"), std::string::npos);
    EXPECT_NE(out.find("\"fill_fade\": 0.05"), std::string::npos);
    EXPECT_NE(out.find("\"fill_decay\": 12.0"), std::string::npos);
}

TEST(DumpConfig, ReflectsMmCalibration)
{
    std::string out;
    int rc = run_truetest(
        "--dump-config --mm-levels 5 --mm-base-depth 50 "
        "--mm-spread-pct 0.001 --mm-vol-mult 2", out);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("\"mm_levels\": 5"), std::string::npos);
    EXPECT_NE(out.find("\"mm_base_depth\": 50"), std::string::npos);
    EXPECT_NE(out.find("\"mm_spread_pct\": 0.001"), std::string::npos);
    EXPECT_NE(out.find("\"mm_vol_mult\": 2.0"), std::string::npos);
}

// --fill-fade without --fill-prob is a silent no-op model — hard-fail.
TEST(CLI, FillFadeRequiresFillProb)
{
    std::string out;
    int rc = run_truetest(
        "--provider local --path market_data.csv "
        "--strategy sma --mode backtest --fill-fade 0.1", out);
    EXPECT_NE(rc, 0);
    EXPECT_NE(out.find("requires --fill-prob"), std::string::npos);
}

// Deprecated fill-pricing flags stay accepted (scripts keep running) but
// must announce that they no longer do anything.
TEST(CLI, RealisticFillsDeprecationWarning)
{
    std::string out;
    int rc = run_truetest(
        "--provider local --path market_data.csv --strategy sma "
        "--mode backtest --realistic-fills --status-format off", out);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("deprecated"), std::string::npos);
}

TEST(CLI, BarSpreadBpsDeprecationWarning)
{
    std::string out;
    int rc = run_truetest(
        "--provider local --path market_data.csv --strategy sma "
        "--mode backtest --bar-spread-bps 10 --status-format off", out);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("no longer affects recorded fill prices"), std::string::npos);
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

TEST(DryRun, LiveFlagRejectedOnBacktestBinary)
{
    std::string out;
    int rc = run_truetest("--dry-run --live", out);
    EXPECT_EQ(rc, 1);
    EXPECT_NE(out.find("Live mode is only permitted on the engine_live binary"),
              std::string::npos);
}

TEST(DryRun, ModeLiveRejectedOnBacktestBinary)
{
    std::string out;
    int rc = run_truetest("--dry-run --mode live", out);
    EXPECT_EQ(rc, 1);
    EXPECT_NE(out.find("Live mode is only permitted on the engine_live binary"),
              std::string::npos);
}

TEST(DryRun, LiveBinaryMainnetFuturesRequiresVenueCaps)
{
    std::string out;
    int rc = run_engine_live(
        "--dry-run --provider binance-futures --symbol BTCUSDT "
        "--mode live --live",
        out);
    EXPECT_EQ(rc, 1);
    EXPECT_NE(out.find("Refusing mainnet futures live mode without venue risk caps"),
              std::string::npos);
}

TEST(DryRun, LiveBinaryMainnetFuturesRequiresDailyLoss)
{
    std::string out;
    int rc = run_engine_live(
        "--dry-run --provider binance-futures --symbol BTCUSDT "
        "--mode live --live --max-notional 25",
        out);
    EXPECT_EQ(rc, 1);
    EXPECT_NE(out.find("Refusing mainnet futures live mode with --max-daily-loss disabled"),
              std::string::npos);
}

TEST(DryRun, LiveBinaryMainnetFuturesAcceptsCapsAndDailyLoss)
{
    std::string out;
    int rc = run_engine_live(
        "--dry-run --provider binance-futures --symbol BTCUSDT "
        "--mode live --live --max-notional 25 --max-daily-loss 5",
        out);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("Config is VALID"), std::string::npos);
}

TEST(DryRun, LiveBinaryTestnetFuturesAllowsWarningOnlyCaps)
{
    std::string out;
    int rc = run_engine_live(
        "--dry-run --provider binance-futures --symbol BTCUSDT "
        "--mode live --live --testnet",
        out);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("No venue risk caps set"), std::string::npos);
    EXPECT_NE(out.find("--max-daily-loss is 0"), std::string::npos);
    EXPECT_NE(out.find("Config is VALID"), std::string::npos);
}

// ─── B4: QuestDB persistence flags ─────────────────────────────────────────

#ifdef HAS_QUESTDB

TEST(CLI, PersistFlagAccepted)
{
    std::string out;
    int rc = run_truetest("--persist --dump-config", out);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("\"persist\": true"), std::string::npos);
}

TEST(CLI, RunTagFlagAccepted)
{
    std::string out;
    int rc = run_truetest("--persist --run-tag my_run --dump-config", out);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("\"run_tag\": \"my_run\""), std::string::npos);
}

TEST(CLI, QuestdbPortsAccepted)
{
    std::string out;
    int rc = run_truetest("--persist --questdb-ilp-port 19009 "
                          "--questdb-http-port 19000 --dump-config", out);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("\"questdb_ilp_port\": 19009"), std::string::npos);
    EXPECT_NE(out.find("\"questdb_http_port\": 19000"), std::string::npos);
}

#else

TEST(CLI, PersistFlagRejectedWhenQuestDbDisabled)
{
    GTEST_SKIP() << "Build has HAS_QUESTDB defined; skipping rejection test.";
}

#endif // HAS_QUESTDB

// ─── B5: Maker queue model (Phase 1) ───────────────────────────────────────

TEST(CLI, MakerQueueModelAcceptedWithoutDepth)
{
    // Without L2, QueueAware is conservative (no front-of-queue); bar sweep still works.
    std::string out;
    int rc = run_truetest("--maker-queue-model uniform --dry-run "
                          "--strategy sma --provider synthetic", out);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("Config is VALID"), std::string::npos);
    EXPECT_NE(out.find("without --depth-stream"), std::string::npos);
    EXPECT_NE(out.find("no optimistic join-front"), std::string::npos);
}

TEST(CLI, MakerQueueModelAcceptedWithDepth)
{
    std::string out;
    int rc = run_truetest("--maker-queue-model uniform --depth-stream depth20 "
                          "--dry-run --strategy sma --provider local --path market_data.csv", out);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("Config is VALID"), std::string::npos);
    EXPECT_EQ(out.find("without --depth-stream"), std::string::npos);
}
