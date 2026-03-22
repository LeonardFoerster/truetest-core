#pragma once

#include "provider.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <stdexcept>

// Factory function type: takes a generic config map, returns a provider.
// The config map carries provider-specific settings (file path, API key,
// endpoint URL, etc.) as string key-value pairs.
using provider_config = std::unordered_map<std::string, std::string>;
using provider_factory = std::function<
	std::shared_ptr<IProvider>(const provider_config&)>;

class ProviderRegistry
{
public:
	// Singleton access (the registry is process-global).
	static ProviderRegistry& instance()
	{
		static ProviderRegistry reg;
		return reg;
	}

	// Register a provider factory under a name.
	// Called once per provider, typically at static init time.
	void register_provider(const std::string& name, provider_factory factory)
	{
		factories_[name] = std::move(factory);
	}

	// Create a provider by name with the given config.
	// Throws if the name is not registered.
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

	// List all registered provider names (for TUI menu, help text).
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

// Helper macro for self-registering providers.
// Place in the provider's .cpp file:
//   REGISTER_PROVIDER("binance", [](const provider_config& cfg) {
//       return std::make_shared<BinanceProvider>(cfg.at("api_key"), ...);
//   });
#define REGISTER_PROVIDER(name, factory) \
	namespace { \
		static const bool _reg_##__LINE__ = []() { \
			ProviderRegistry::instance().register_provider(name, factory); \
			return true; \
		}(); \
	}
