#include "ui/desk/research_views.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace truetest::ui::desk {

research_view_handle make_demo_research_presentation()
{
    auto view = std::make_shared<ResearchPresentation>();
    view->state = DeskDataState::demo;
    view->source = "deterministic local fixture";
    // Fixed, not drawn from next_research_version(): two calls to this
    // function must produce the SAME version (see
    // ImGuiDeskDemo.FixtureIsDeterministicBoundedAndExplicit - identical
    // fixture content is expected to compare as identical). Demo-vs-live
    // version-number collisions are prevented at the cache layer instead
    // (research_panels.cpp keys on (state, version), not version alone) -
    // see FootprintBoundsCache's comment.
    view->version = 1;
    for (auto& surface : view->surfaces)
    {
        surface.state = DeskDataState::demo;
        surface.source = view->source;
        surface.version = view->version;
    }

    constexpr double base = 68120.0;
    constexpr int bar_count = 28;
    constexpr int levels_per_bar = 13;
    view->footprint.reserve(bar_count);
    double cvd = 0.0;
    for (int bar = 0; bar < bar_count; ++bar)
    {
        FootprintBarView out;
        out.start_ms = 1'754'200'000'000LL + static_cast<std::int64_t>(bar) * 60'000;
        const double center = base + std::sin(static_cast<double>(bar) * 0.37) * 34.0
            + static_cast<double>(bar) * 1.4;
        out.open = center - std::cos(static_cast<double>(bar) * 0.41) * 7.0;
        out.close = center + std::sin(static_cast<double>(bar) * 0.53) * 8.0;
        out.low = std::min(out.open, out.close) - 24.0;
        out.high = std::max(out.open, out.close) + 24.0;
        out.levels.reserve(levels_per_bar);
        for (int level = 0; level < levels_per_bar; ++level)
        {
            const int offset = level - levels_per_bar / 2;
            const double bell = static_cast<double>(levels_per_bar / 2 + 1 - std::abs(offset));
            const double buy = bell * (2.0 + static_cast<double>((bar + level * 3) % 7));
            const double sell = bell * (1.5 + static_cast<double>((bar * 2 + level) % 6));
            out.levels.push_back({center + static_cast<double>(offset) * 5.0, sell, buy});
            cvd += buy - sell;
        }
        out.cvd = cvd;
        view->footprint.push_back(std::move(out));
    }

    view->dom.reserve(31);
    for (int row = 15; row >= -15; --row)
    {
        const double distance = static_cast<double>(std::abs(row));
        const double resting = 5.0 + std::fmod(distance * distance * 1.7 + 11.0, 58.0);
        const bool ask = row > 0;
        const bool bid = row < 0;
        view->dom.push_back({
            base + static_cast<double>(row) * 5.0,
            bid ? resting : 0.0,
            ask ? resting : 0.0,
            static_cast<double>((row + 30) % 5) * 1.7,
            static_cast<double>((row + 28) % 7) * 1.2,
        });
    }

    view->heatmap_columns = 96;
    view->heatmap_rows = 56;
    view->heatmap_start_ms = 1'754'200'000'000LL;
    view->heatmap_end_ms = view->heatmap_start_ms + 95'000;
    view->heatmap_min_price = base - 140.0;
    view->heatmap_max_price = base + 140.0;
    view->heatmap.reserve(static_cast<std::size_t>(view->heatmap_columns)
                          * view->heatmap_rows);
    for (std::uint16_t column = 0; column < view->heatmap_columns; ++column)
    {
        const double price_path = 27.0 + std::sin(static_cast<double>(column) * 0.11) * 6.0;
        for (std::uint16_t row = 0; row < view->heatmap_rows; ++row)
        {
            const double wall_a = std::exp(-std::pow((static_cast<double>(row) - 16.0) / 2.1, 2.0));
            const double wall_b = std::exp(-std::pow((static_cast<double>(row) - 42.0) / 3.0, 2.0));
            const double near_price = std::exp(-std::pow((static_cast<double>(row) - price_path) / 4.0, 2.0));
            const float value = static_cast<float>(
                0.08 + wall_a * (column < 72 ? 0.82 : 0.14)
                + wall_b * (column > 18 ? 0.58 : 0.10) + near_price * 0.18);
            view->heatmap.push_back({column, row, std::min(value, 1.0f)});
        }
    }

    view->profile.reserve(33);
    for (int row = -16; row <= 16; ++row)
    {
        const double shape = 18.0 - std::abs(static_cast<double>(row));
        std::string tpo;
        const int letters = std::max(1, static_cast<int>(shape / 2.4));
        for (int i = 0; i < letters; ++i)
            tpo.push_back(static_cast<char>('A' + (i + std::abs(row)) % 20));
        view->profile.push_back({
            base + static_cast<double>(row) * 5.0,
            shape * (1.2 + static_cast<double>((row + 19) % 4) * 0.3),
            shape * (1.0 + static_cast<double>((row + 17) % 5) * 0.22),
            std::move(tpo),
        });
    }

    view->funding = {
        {"BTCUSDT", "Binance", 0.00010, 0.1095, 1.8, 13'420},
        {"BTCUSDT", "Bitget", -0.00004, -0.0438, -0.7, 13'420},
        {"ETHUSDT", "Binance", 0.00017, 0.1862, 2.4, 13'420},
        {"SOLUSDT", "Binance", -0.00021, -0.2301, -3.1, 13'420},
        {"XRPUSDT", "Bitget", 0.00008, 0.0876, 0.9, 13'420},
    };

    view->watchlist = {
        {"BTCUSDT", 68120.0, 1.42, 1'920'000'000.0},
        {"ETHUSDT", 3568.4, 0.83, 1'180'000'000.0},
        {"SOLUSDT", 172.61, -0.46, 642'000'000.0},
        {"XRPUSDT", 0.6214, 2.18, 511'000'000.0},
        {"BNBUSDT", 602.7, -0.11, 281'000'000.0},
    };

    for (int i = 0; i < 12; ++i)
    {
        view->liquidations.push_back({
            view->heatmap_start_ms + static_cast<std::int64_t>(i) * 7'000,
            base + static_cast<double>((i % 7) - 3) * 11.0,
            18'000.0 + static_cast<double>((i * 17) % 9) * 13'500.0,
            (i % 3) != 0,
        });
    }

    view->correlation_symbols = {"BTC", "ETH", "SOL", "XRP", "BNB"};
    constexpr double matrix[] = {
        1.00, 0.88, 0.72, 0.58, 0.76,
        0.88, 1.00, 0.79, 0.62, 0.73,
        0.72, 0.79, 1.00, 0.54, 0.65,
        0.58, 0.62, 0.54, 1.00, 0.49,
        0.76, 0.73, 0.65, 0.49, 1.00,
    };
    view->correlation.assign(std::begin(matrix), std::end(matrix));
    return view;
}

} // namespace truetest::ui::desk
