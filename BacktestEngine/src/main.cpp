#include <iostream>
#include <string>
#include <memory>
#include <cstdlib>
#include <cstring>

#include "core/engine.h"
#include "core/engine_config.h"
#include "core/event_log.h"
#include "data/data_source.h"
#include "data/csv_data_source.h"
#include "data/binary_cache_source.h"
#include "data/tick_csv_data_source.h"
#include "execution/fee_model.h"
#include "orderbook/orderbook.h"
#include "strategy/mean_reversion_strategy.h"
#include "strategy/sma_strategy.h"
#include "strategy/ma_crossover_strategy.h"
#include "market_maker/market_maker.h"
#include "threading/thread_config.h"

#ifdef HAS_POSTGRESQL
#include "data/pg_data_source.h"
#endif

int main(int argc, char* argv[])
{
    // --- CLI flags ---
    std::string replay_path;
    std::string event_log_path;
    uint64_t seed = 0;
    std::string thread_preset_str;
    bool no_pin = false;

    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--replay") == 0 && i + 1 < argc)
            replay_path = argv[++i];
        else if (std::strcmp(argv[i], "--log-events") == 0 && i + 1 < argc)
            event_log_path = argv[++i];
        else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc)
            seed = std::stoull(argv[++i]);
        else if (std::strcmp(argv[i], "--thread-preset") == 0 && i + 1 < argc)
            thread_preset_str = argv[++i];
        else if (std::strcmp(argv[i], "--no-pin") == 0)
            no_pin = true;
    }
    // --- Replay mode: skip TUI, replay events from log ---
    if (!replay_path.empty())
    {
        std::cout << "\n  Replaying events from: " << replay_path << "\n";

        // Strategy still needed for replay
        std::cout << "  Using Mean Reversion strategy (default) for replay.\n";
        auto strategy = std::make_shared<mean_reversion_strategy>(20);

        engine_config cfg;
        cfg.seed = seed;
        cfg.event_log_path = event_log_path;

        auto dh = std::make_shared<data_handler>();
        engine eng(dh, nullptr, strategy, std::move(cfg));
        eng.run_replay(replay_path);
        eng.print_summary();
        return 0;
    }

    std::cout << "\n";
    std::cout << "   _____                _____         _   \n";
    std::cout << "  |_   _| __ _   _  ___|_   _|__  ___| |_ \n";
    std::cout << "    | || '__| | | |/ _ \\ | |/ _ \\/ __| __|\n";
    std::cout << "    | || |  | |_| |  __/ | |  __/\\__ \\ |_ \n";
    std::cout << "    |_||_|   \\__,_|\\___| |_|\\___||___/\\__|\n";
    std::cout << "                                           \n";
    std::cout << "    Backtesting Engine v0.1\n";
    std::cout << "\n";

    // ── Strategy Selection ───────────────────────────────────────
    std::cout << "  +-------------------------------------------+\n";
    std::cout << "  |  Strategy                                 |\n";
    std::cout << "  +-------------------------------------------+\n";
    std::cout << "  |  [1]  Mean Reversion (SMA)                |\n";
    std::cout << "  |  [2]  SMA-based Strategy                  |\n";
    std::cout << "  |  [3]  MA Crossover                        |\n";
    std::cout << "  +-------------------------------------------+\n";
    std::cout << "  > ";
    int strategy_choice;
    std::cin >> strategy_choice;

    if (strategy_choice < 1 || strategy_choice > 3)
    {
        std::cout << "  ! Invalid choice, defaulting to Mean Reversion.\n";
        strategy_choice = 1;
    }

    // ── SMA Period ───────────────────────────────────────────────
    std::cout << "\n";
    std::cout << "  +-------------------------------------------+\n";
    std::cout << "  |  SMA Period                               |\n";
    std::cout << "  +-------------------------------------------+\n";
    std::cout << "  > ";
    std::size_t sma_period = 20;
    std::cin >> sma_period;

    std::shared_ptr<IStrategy> strategy;
    if (strategy_choice == 1)
    {
        strategy = std::make_shared<mean_reversion_strategy>(sma_period);
    } else if (strategy_choice == 2)
    {
        strategy = std::make_shared<sma_strategy>(sma_period);
    } else if (strategy_choice == 3)
    {
        strategy = std::make_shared<ma_crossover_strategy>(sma_period);
    }

    // ── Data Source Selection ────────────────────────────────────
    std::cout << "\n";
    std::cout << "  +-------------------------------------------+\n";
    std::cout << "  |  Data Source                               |\n";
    std::cout << "  +-------------------------------------------+\n";
#ifdef HAS_POSTGRESQL
    std::cout << "  |  [1]  PostgreSQL Database                  |\n";
#else
    std::cout << "  |  [1]  PostgreSQL (unavailable)             |\n";
    std::cout << "  |       build with -DENABLE_POSTGRESQL=ON    |\n";
#endif
    std::cout << "  |  [2]  CSV File (OHLCV)                     |\n";
    std::cout << "  |  [3]  Tick CSV File                        |\n";
    std::cout << "  +-------------------------------------------+\n";
    std::cout << "  > ";
    int data_choice;
    std::cin >> data_choice;

    std::unique_ptr<IDataSource> source;

    if (data_choice == 2)
    {
        std::cout << "\n  CSV path: ";
        std::string csv_path;
        std::cin >> csv_path;
        source = std::make_unique<CsvDataSource>(csv_path);
    }
    else if (data_choice == 3)
    {
        std::cout << "\n  Tick CSV path: ";
        std::string tick_path;
        std::cin >> tick_path;
        source = std::make_unique<TickCsvDataSource>(tick_path);
    }
#ifdef HAS_POSTGRESQL
    else if (data_choice == 1)
    {
        auto pg = std::make_unique<PgDataSource>();
        pg->establish_connection();
        pg->test_connection();
        source = std::make_unique<BinaryCacheSource>(std::move(pg), "data_cache.bin");
    }
#endif
    else
    {
        std::cout << "  ! Invalid choice.\n";
        return 1;
    }

    // ── Fee Model Selection ─────────────────────────────────────
    std::cout << "\n";
    std::cout << "  +-------------------------------------------+\n";
    std::cout << "  |  Fee Model                                |\n";
    std::cout << "  +-------------------------------------------+\n";
    std::cout << "  |  [1]  Zero Fees                           |\n";
    std::cout << "  |  [2]  Fixed Fee Per Trade                 |\n";
    std::cout << "  |  [3]  Tiered (Maker/Taker)                |\n";
    std::cout << "  +-------------------------------------------+\n";
    std::cout << "  > ";
    int fee_choice;
    std::cin >> fee_choice;

    engine_config config;
    config.seed = seed;
    config.event_log_path = event_log_path;
    config.disable_pinning = no_pin;

    // Thread preset: CLI override or auto-detect
    if (!thread_preset_str.empty())
        config.threading = string_to_preset(thread_preset_str);
    else
        config.threading = select_preset(detect_physical_cores());

    std::cout << "  Threading: " << preset_to_string(config.threading)
              << " (" << preset_worker_count(config.threading) << " workers)\n";

    if (fee_choice == 2)
    {
        std::cout << "  Fee per trade: ";
        double fee;
        std::cin >> fee;
        config.fee_model = std::make_shared<FixedFeeModel>(fee);
    }
    else if (fee_choice == 3)
    {
        std::cout << "  Maker rate (e.g. 0.001): ";
        double maker;
        std::cin >> maker;
        std::cout << "  Taker rate (e.g. 0.002): ";
        double taker;
        std::cin >> taker;
        config.fee_model = std::make_shared<TieredFeeModel>(maker, taker);
    }
    // else: config.fee_model stays nullptr (zero fees)

    // ── Engine Mode Selection ────────────────────────────────────
    std::cout << "\n";
    std::cout << "  +-------------------------------------------+\n";
    std::cout << "  |  Engine Mode                              |\n";
    std::cout << "  +-------------------------------------------+\n";
    std::cout << "  |  [1]  Backtest (batch data)               |\n";
    std::cout << "  |  [2]  Shadow  (live feed, paper fills)    |\n";
    std::cout << "  |  [3]  Live    (live feed, real fills)     |\n";
    std::cout << "  +-------------------------------------------+\n";
    std::cout << "  > ";
    int mode_choice;
    std::cin >> mode_choice;

    if (mode_choice == 2)
        config.mode = engine_mode::shadow;
    else if (mode_choice == 3)
        config.mode = engine_mode::live;
    // else: default backtest

    // ── Run ──────────────────────────────────────────────────────
    std::cout << "\n";
    std::cout << "  ============================================\n";
    if (config.mode == engine_mode::backtest)
        std::cout << "    Running backtest...\n";
    else if (config.mode == engine_mode::shadow)
        std::cout << "    Running shadow mode...\n";
    else
        std::cout << "    Running live mode...\n";
    std::cout << "  ============================================\n\n";

    auto dh = std::make_shared<data_handler>();
    if (!source->load_data(dh))
    {
        std::cerr << "  Failed to load data.\n";
        return 1;
    }

    engine eng(dh, nullptr, strategy, std::move(config));
    eng.run();
    eng.print_summary();

    return 0;
}
