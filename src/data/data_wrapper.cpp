#include "data_wrapper.h"
#include "csv_data_source.h"
#include "tick_csv_data_source.h"

#include <iostream>
#include <stdexcept>

namespace {

class MultiSource final : public IMarketSource
{
public:
	explicit MultiSource(std::vector<std::unique_ptr<IMarketSource>> parts)
		: parts_(std::move(parts)) {}

	bool load_into(IMarketSink& sink, LoadStats* stats) override
	{
		LoadStats total;
		bool any = false;
		for (auto& p : parts_)
		{
			LoadStats part;
			if (!p->load_into(sink, &part))
			{
				if (stats)
				{
					*stats = total;
					stats->message = part.message.empty()
						? "multi-source partial failure"
						: part.message;
				}
				return any; // soft: keep what we have if any accepted
			}
			total.accepted += part.accepted;
			total.rejected += part.rejected;
			if (part.accepted > 0) any = true;
		}
		if (stats) *stats = total;
		return any;
	}

private:
	std::vector<std::unique_ptr<IMarketSource>> parts_;
};

std::unique_ptr<IMarketSource> source_for_path(const std::filesystem::path& path,
                                               bool force_tick)
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

} // namespace

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
	return DataWrapper(std::make_unique<MultiSource>(std::move(parts)), std::move(opt));
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
			"DataWrapper: parquet: URI requires ENABLE_PARQUET (docs/data.md#D-08; deferred)");
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
	if (opt_.reserve_hint > 0)
		out.reserve(opt_.reserve_hint);

	LoadStats stats;
	const bool ok = source_->load_into(out, &stats);
	if (!ok)
	{
		if (!stats.message.empty())
			std::cerr << "  ! DataWrapper load failed: " << stats.message << "\n";
		return false;
	}

	if (opt_.fail_if_empty && out.empty())
	{
		std::cerr << "  ! DataWrapper: load produced no records\n";
		return false;
	}

	if (opt_.sort_after_load && out.has_bar_data())
		out.sort_bars_by_time();

	return true;
}

bool DataWrapper::stream(IMarketSink& sink, std::atomic<bool>* halt)
{
	if (!source_->supports_stream())
		return false;
	return source_->stream_into(sink, halt, nullptr);
}

IMarketSource& DataWrapper::source()
{
	return *source_;
}
