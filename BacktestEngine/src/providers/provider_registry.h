#pragma once

#include "provider.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <stdexcept>

using provider_config = std::unordered_map<std::string, std::string>;
using provider_factory = std::function<
	std::shared_ptr<IProvider>(const provider_config&)>;

class ProviderRegistry
{
public:
	static ProviderRegistry& instance()
	{
		static ProviderRegistry reg;
		return reg;
	}

	void register_provider(const std::string& name, provider_factory factory)
	{
		factories_[name] = std::move(factory);
	}

	std::shared_ptr<IProvider> create(
		const std::string& name,
		const provider_config& config = {}) const
	{
		auto it = factories_.find(name);
		if (it == factories_.end())
			throw std::runtime_error(
				"ProviderRegistry: unknown provider '" + name + "'");
		return it->second(config);
	}

	std::vector<std::string> available() const
	{
		std::vector<std::string> names;
		names.reserve(factories_.size());
		for (const auto& [name, _] : factories_)
			names.push_back(name);
		return names;
	}

	bool has(const std::string& name) const
	{
		return factories_.count(name) > 0;
	}

private:
	ProviderRegistry() = default;
	std::unordered_map<std::string, provider_factory> factories_;
};

#define REGISTER_PROVIDER(name, factory) \
	namespace { \
		static const bool _reg_##__LINE__ = []() { \
			ProviderRegistry::instance().register_provider(name, factory); \
			return true; \
		}(); \
	}
