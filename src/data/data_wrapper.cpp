#include "data_wrapper.h"
#include "csv_data_source.h"
#include "market_series.h"
#include "tick_csv_data_source.h"

#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {

std::size_t checked_validation_delta(const MarketSeries& series,
                                     const MarketSeries::AppendCheckpoint& checkpoint)
{
    if (series.validation_errors() < checkpoint.validation_errors)
        throw std::logic_error(
            "DataWrapper: source violated the append-only IMarketSource contract");
    return series.validation_errors() - checkpoint.validation_errors;
}

void checked_accumulate(std::size_t& total, std::size_t increment)
{
    if (increment > std::numeric_limits<std::size_t>::max() - total)
        throw std::overflow_error("DataWrapper: LoadStats counter overflow");
    total += increment;
}

class MultiSource final : public IMarketSource
{
public:
    MultiSource(std::vector<std::unique_ptr<IMarketSource>> parts, bool allow_partial)
        : parts_(std::move(parts))
        , allow_partial_(allow_partial)
    {}

    bool load_into(IMarketSink& sink, LoadStats* stats) override
    {
        LoadStats total;
        bool any = false;
        bool had_partial_failure = false;
        std::string first_failure;
        auto* series = dynamic_cast<MarketSeries*>(&sink);
        for (auto& p : parts_) {
            const auto checkpoint =
                series ? series->append_checkpoint() : MarketSeries::AppendCheckpoint{};
            LoadStats part;
            const bool loaded = p->load_into(sink, &part);
            if (series) {
                const std::size_t observed_sink_rejections =
                    checked_validation_delta(*series, checkpoint);
                part.rejected = std::max(part.rejected, observed_sink_rejections);
            }
            // Rejected records from a skipped partial source must remain visible
            // to strict callers even though that part's accepted prefix is rolled
            // back and therefore is not committed to this composite source.
            checked_accumulate(total.rejected, part.rejected);
            if (!loaded) {
                // Partial mode means a failed input is skipped, not that a
                // source may leave a prefix of its failed batch in the series.
                // Native sources receive an append-only MarketSeries here; the
                // outer DataWrapper remains the exception rollback boundary.
                if (series) series->rollback_appends(checkpoint);
                had_partial_failure = true;
                if (first_failure.empty()) {
                    first_failure =
                        part.message.empty() ? "multi-source partial failure" : part.message;
                }
            } else {
                checked_accumulate(total.accepted, part.accepted);
                if (part.accepted > 0) any = true;
            }
            if (had_partial_failure && !allow_partial_) {
                if (stats) {
                    *stats = total;
                    // DR-REPLAY-03: default fail-closed so multi-file portfolios
                    // never run green with missing legs. Callers that relied on
                    // the old soft-success default must now opt in explicitly.
                    stats->message = first_failure +
                                     " (fail-closed default; set "
                                     "DataLoadOptions::allow_partial_sources=true to keep "
                                     "rows accepted so far)";
                }
                return false;
            }
        }
        if (stats) {
            *stats = total;
            if (had_partial_failure) stats->message = first_failure;
        }
        return any;
    }

private:
    std::vector<std::unique_ptr<IMarketSource>> parts_;
    bool allow_partial_ = false;
};

std::unique_ptr<IMarketSource> source_for_path(const std::filesystem::path& path, bool force_tick)
{
	const auto ext = path.extension().string();
	if (force_tick)
		return std::make_unique<TickCsvDataSource>(path.string());
	// Default: bars for .csv; tick if name hints
	const auto name = path.filename().string();
	if (name.find("tick") != std::string::npos)
		return std::make_unique<TickCsvDataSource>(path.string());
	return std::make_unique<CsvDataSource>(path);
}

}  // namespace

DataWrapper::DataWrapper(std::unique_ptr<IMarketSource> source, DataLoadOptions opt)
	: source_(std::move(source))
	, opt_(std::move(opt))
{
	if (!source_)
		throw std::invalid_argument("DataWrapper: null source");
}

DataWrapper DataWrapper::from_source(std::unique_ptr<IMarketSource> source,
                                     DataLoadOptions opt)
{
	return DataWrapper(std::move(source), std::move(opt));
}

DataWrapper DataWrapper::from_path(const std::filesystem::path& path,
                                   DataLoadOptions opt)
{
	return from_paths({path}, std::move(opt));
}

DataWrapper DataWrapper::from_paths(const std::vector<std::filesystem::path>& paths,
                                    DataLoadOptions opt)
{
	if (paths.empty())
		throw std::invalid_argument("DataWrapper::from_paths: empty path list");

	if (paths.size() == 1)
		return DataWrapper(source_for_path(paths.front(), /*force_tick=*/false),
		                   std::move(opt));

	std::vector<std::unique_ptr<IMarketSource>> parts;
	parts.reserve(paths.size());
	for (const auto& p : paths)
		parts.push_back(source_for_path(p, /*force_tick=*/false));
	const bool allow_partial = opt.allow_partial_sources;
	return DataWrapper(
		std::make_unique<MultiSource>(std::move(parts), allow_partial),
		std::move(opt));
}

DataWrapper DataWrapper::from_uri(std::string_view uri, DataLoadOptions opt)
{
	// Schemes: csv:, csv+tick:, bare filesystem path, file://
	bool force_tick = false;
	std::string path;

	auto starts_with = [](std::string_view s, std::string_view p) {
		return s.size() >= p.size() && s.substr(0, p.size()) == p;
	};

	if (starts_with(uri, "csv+tick:"))
	{
		force_tick = true;
		path = std::string(uri.substr(9));
	}
	else if (starts_with(uri, "csv:"))
	{
		path = std::string(uri.substr(4));
	}
	else if (starts_with(uri, "file://"))
	{
		path = std::string(uri.substr(7));
	}
	else if (starts_with(uri, "parquet:"))
	{
		throw std::runtime_error(
			"DataWrapper: parquet: URI requires ENABLE_PARQUET (docs/internal/data-pipeline.md#D-08; deferred)");
	}
	else
	{
		path = std::string(uri);
	}

	// Strip leading // from csv:///path style
	while (path.size() >= 2 && path[0] == '/' && path[1] == '/')
		path.erase(0, 1);

	return DataWrapper(source_for_path(path, force_tick), std::move(opt));
}

bool DataWrapper::load(MarketSeries& out)
{
    last_load_stats_ = {};
    if (opt_.reserve_hint > 0) out.reserve(opt_.reserve_hint);

    const auto checkpoint = out.append_checkpoint();
    auto preserves_append_boundary = [&]() noexcept {
        return out.bar_count() >= checkpoint.bar_count &&
               out.tick_count() >= checkpoint.tick_count &&
               out.validation_errors() >= checkpoint.validation_errors;
    };
    LoadStats stats;
    bool ok = false;
    try {
        ok = source_->load_into(out, &stats);
    } catch (...) {
        if (!preserves_append_boundary()) {
            throw std::logic_error(
                "DataWrapper: source violated the append-only IMarketSource contract");
        }
        stats.rejected = std::max(stats.rejected, checked_validation_delta(out, checkpoint));
        last_load_stats_ = stats;
        // Source failures are allowed to be reported as false, but an
        // exception is never an opt-in partial-source result. Restore the
        // append-only marker before preserving the source's exception.
        out.rollback_appends(checkpoint);
        throw;
    }
    if (!preserves_append_boundary()) {
        // Do not perform suffix arithmetic after a source has invalidated its
        // checkpoint. The façade cannot reconstruct pre-existing rows without
        // an unacceptable full copy, so make the contract violation loud.
        throw std::logic_error(
            "DataWrapper: source violated the append-only IMarketSource contract");
    }
    // A source owns parser-level rejection accounting, but the MarketSeries is
    // the authority for sink validation. Reconcile the counter so a custom or
    // buggy source cannot turn an observed sink rejection into strict success.
    const std::size_t observed_sink_rejections = checked_validation_delta(out, checkpoint);
    if (stats.rejected < observed_sink_rejections) stats.rejected = observed_sink_rejections;
    last_load_stats_ = stats;
    if (!ok) {
        // allow_partial_sources is resolved inside MultiSource: a successful
        // partial result returns true. Any false result is a failed batch and
        // must never leave a direct or multi-source prefix behind.
        out.rollback_appends(checkpoint);
        if (!stats.message.empty())
            std::cerr << "  ! DataWrapper load failed: " << stats.message << "\n";
        return false;
    }
    if (!stats.message.empty()) {
        // MultiSource records the first skipped source when partial mode is
        // explicitly enabled. A successful partial result must still be loud.
        std::cerr << "  ! DataWrapper partial source failure: " << stats.message << "\n";
    }
    if (opt_.fail_on_rejected_rows && stats.rejected > 0) {
        out.rollback_appends(checkpoint);
        if (!last_load_stats_.message.empty()) last_load_stats_.message += "; ";
        last_load_stats_.message += "strict batch rejected malformed or invalid records";
        std::cerr << "  ! DataWrapper: rejected " << stats.rejected
		          << " malformed or invalid record(s); strict batch rolled back\n";
		return false;
    }

    // DR-REPLAY-04: apply declared from/to/symbols filters (were previously no-ops).
	// Existing rows belong to the caller, not this batch; only filter the
	// suffix accepted since the checkpoint so a failed/empty load cannot erase
	// a reusable series' earlier data.
	if (opt_.from || opt_.to || !opt_.symbols.empty())
	{
		const std::size_t before_bars = out.bar_count() - checkpoint.bar_count;
		const std::size_t before_ticks = out.tick_count() - checkpoint.tick_count;
		try
		{
			out.filter_appended_window(checkpoint, opt_.from, opt_.to, opt_.symbols);
		}
		catch (...)
		{
			// The filter operates only on the appended suffix, so truncation is
			// still sufficient if its cold-path symbol set cannot be allocated.
			out.rollback_appends(checkpoint);
			throw;
		}
		const std::size_t dropped =
			(before_bars - (out.bar_count() - checkpoint.bar_count))
			+ (before_ticks - (out.tick_count() - checkpoint.tick_count));
		if (dropped > 0)
		{
			std::cerr << "  · DataWrapper: filtered " << dropped
			          << " records outside from/to/symbols window\n";
		}
	}

	const bool appended_empty = out.bar_count() == checkpoint.bar_count
		&& out.tick_count() == checkpoint.tick_count;
	if (opt_.fail_if_empty && appended_empty)
	{
		out.rollback_appends(checkpoint);
		std::cerr << "  ! DataWrapper: load produced no records\n";
		return false;
	}

	if (opt_.sort_after_load && !appended_empty)
	{
		try
		{
			// Build all sort plans before either store changes. See
			// MarketSeries::sort_all_by_time for the in-place application.
			out.sort_all_by_time();
		}
		catch (...)
		{
			out.rollback_appends(checkpoint);
			throw;
		}
	}

	return true;
}

StreamResult DataWrapper::stream(IMarketSink& sink, std::atomic<bool>* halt)
{
	if (!source_->supports_stream())
		return {stream_termination::unsupported};
	return source_->stream_into(sink, halt, nullptr);
}

IMarketSource& DataWrapper::source()
{
	return *source_;
}
