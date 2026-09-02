#include "csv_data_source.h"
#include "market_series.h"
#include "market_types.h"
#include "strict_market_csv.h"

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool load_csv_bars(const std::filesystem::path& path, IMarketSink& sink, LoadStats* stats)
{
    if (stats) *stats = {};
    std::ifstream file(path);
    if (!file.good()) {
        if (stats) stats->message = "CSV file error: " + path.string();
        return false;
    }

    std::string line;
    if (!std::getline(file, line)) {
        if (stats) stats->message = "CSV empty: " + path.string();
        return false;
    }

    tt::strict_market_csv::bar_schema schema;
    if (!schema.parse_header(line)) {
        if (stats) stats->message = "CSV invalid or incomplete header: " + path.string();
        return false;
    }

    // Pre-reserve when sink is a MarketSeries
    if (auto* series = dynamic_cast<MarketSeries*>(&sink)) {
        const auto fsz = std::filesystem::file_size(path);
        const size_t est_rows = (fsz > 0) ? (fsz / 60 + 1024) : 2'000'000;
        series->reserve_bars(est_rows);
    }

    std::size_t accepted = 0;
    std::size_t rejected = 0;
    std::vector<Bar> staged;

    while (std::getline(file, line)) {
        tt::strict_market_csv::parsed_bar parsed;
        if (!schema.parse_row(line, parsed)) {
            ++rejected;
            continue;
        }

        Bar bar;
        bar.date = std::string(parsed.date);
        bar.symbol = std::string(parsed.symbol);
        bar.ts = parsed.timestamp;
        bar.open = parsed.open;
        bar.high = parsed.high;
        bar.low = parsed.low;
        bar.close = parsed.close;
        bar.volume = parsed.volume;
        bar.quantity_scale = parsed.quantity_scale;
        staged.push_back(std::move(bar));
    }

    if (file.bad()) {
        if (stats) {
            stats->accepted = accepted;
            stats->rejected = rejected;
            stats->message = "CSV read error before EOF: " + path.string();
        }
        return false;
    }

    // Input parsing is transactional: never expose a thinned valid/bad/valid
    // dataset to a strategy. Sink-side transactional semantics are a separate
    // port concern, so no sink callback is made until every row is valid.
    if (rejected != 0) {
        if (stats) {
            stats->accepted = 0;
            stats->rejected = rejected;
            stats->message = "CSV contains invalid market rows";
        }
        return false;
    }

    for (const auto& bar : staged) {
        if (!sink.on_bar(bar)) {
            if (stats) {
                stats->accepted = accepted;
                stats->rejected = 1;
                stats->message = "CSV sink rejected a validated market row";
            }
            return false;
        }
        ++accepted;
    }

    if (stats) {
        stats->accepted = accepted;
        stats->rejected = 0;
    }

    std::cout << "Loaded " << accepted << " records from CSV." << std::endl;
    return accepted > 0;
}

}  // namespace

CsvDataSource::CsvDataSource(std::filesystem::path path)
    : path_(std::move(path))
{}

bool CsvDataSource::load_into(IMarketSink& sink, LoadStats* stats)
{
    auto* series = dynamic_cast<MarketSeries*>(&sink);
    if (!series) {
        if (stats) {
            *stats = {};
            stats->message =
                "CSV load requires a transactional MarketSeries sink";
        }
        return false;
    }
    const auto checkpoint = series->append_checkpoint();
    try {
        const bool loaded = load_csv_bars(path_, sink, stats);
        if (!loaded) {
            series->rollback_appends(checkpoint);
            if (stats) stats->accepted = 0;
        }
        return loaded;
    } catch (...) {
        series->rollback_appends(checkpoint);
        if (stats) stats->accepted = 0;
        throw;
    }
}

bool CsvDataSource::load_data(std::shared_ptr<data_handler> handler)
{
    if (!handler) return false;
    const auto checkpoint = handler->append_checkpoint();
    try {
        const bool loaded = load_into(*handler, nullptr);
        if (!loaded) handler->rollback_appends(checkpoint);
        return loaded;
    } catch (...) {
        handler->rollback_appends(checkpoint);
        throw;
    }
}
