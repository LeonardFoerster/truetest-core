#include "tick_csv_data_source.h"
#include "market_series.h"
#include "market_types.h"
#include "strict_market_csv.h"

#include <fstream>
#include <iostream>
#include <vector>

bool TickCsvDataSource::load_into(IMarketSink& sink, LoadStats* stats)
{
    auto* series = dynamic_cast<MarketSeries*>(&sink);
    if (!series) {
        if (stats) {
            *stats = {};
            stats->message =
                "Tick CSV load requires a transactional MarketSeries sink";
        }
        return false;
    }
    const auto checkpoint = series->append_checkpoint();
    const auto rollback = [&] {
        series->rollback_appends(checkpoint);
        if (stats) stats->accepted = 0;
    };
    try {
    if (stats) *stats = {};
    std::ifstream file(path_);
    if (!file.is_open()) {
        std::cerr << "  ! Failed to open tick CSV: " << path_ << "\n";
        if (stats) stats->message = "Failed to open tick CSV: " + path_;
        rollback();
        return false;
    }

    std::string first_line;
    if (!std::getline(file, first_line)) {
        if (stats) stats->message = "Tick CSV empty: " + path_;
        rollback();
        return false;
    }
    tt::strict_market_csv::tick_schema schema;
    if (!schema.parse_header_or_first_row(first_line)) {
        if (stats) stats->message = "Tick CSV invalid header or first row: " + path_;
        rollback();
        return false;
    }

    auto* series = dynamic_cast<MarketSeries*>(&sink);
    if (series) series->reserve_ticks(4096);

    std::size_t accepted = 0;
    std::size_t rejected = 0;
    std::vector<Tick> staged;

    auto emit = [&](std::string_view line) {
        tt::strict_market_csv::parsed_tick parsed;
        if (!schema.parse_row(line, parsed)) {
            ++rejected;
            return;
        }
        Tick record;
        record.timestamp = parsed.timestamp;
        record.symbol = std::string(parsed.symbol);
        record.price = parsed.price;
        record.quantity = parsed.quantity;
        record.quantity_scale = parsed.quantity_scale;
        record.side = parsed.side;
        staged.push_back(std::move(record));
    };

    if (schema.header_contains_record()) emit(first_line);
    std::string line;
    while (std::getline(file, line))
        emit(line);

    if (file.bad()) {
        if (stats) {
            stats->accepted = accepted;
            stats->rejected = rejected;
            stats->message = "Tick CSV read error before EOF: " + path_;
        }
        rollback();
        return false;
    }

    if (rejected != 0) {
        if (stats) {
            stats->accepted = 0;
            stats->rejected = rejected;
            stats->message = "Tick CSV contains invalid market rows";
        }
        rollback();
        return false;
    }

    for (const auto& record : staged) {
        if (!sink.on_tick(record)) {
            if (stats) {
                stats->accepted = accepted;
                stats->rejected = 1;
                stats->message = "Tick CSV sink rejected a validated market row";
            }
            rollback();
            return false;
        }
        ++accepted;
    }

    if (stats) {
        stats->accepted = accepted;
        stats->rejected = 0;
    }

    std::cout << "  Loaded " << accepted << " ticks from " << path_ << "\n";
    if (accepted == 0) {
        rollback();
        return false;
    }
    return true;
    } catch (...) {
        rollback();
        throw;
    }
}

bool TickCsvDataSource::load_data(std::shared_ptr<data_handler> handler)
{
    if (!handler) return false;
    const auto checkpoint = handler->append_checkpoint();
    try {
        const bool loaded = load_into(*handler, nullptr);
        if (!loaded) {
            handler->rollback_appends(checkpoint);
            return false;
        }
        // Direct legacy users retain the historical sorted-tape contract. The
        // DataWrapper owns sorting for multi-source loads, once every source has
        // succeeded, so a later failure cannot reorder pre-existing ticks.
        handler->sort_ticks_by_time();
        return true;
    } catch (...) {
        handler->rollback_appends(checkpoint);
        throw;
    }
}
