#include "providers/provider_registry.h"
#include "providers/local/local_provider.h"

REGISTER_PROVIDER("local", [](const provider_config& cfg) {
	auto it = cfg.find("path");
	if (it == cfg.end())
		throw std::runtime_error("LocalProvider requires 'path' in config");
	return std::make_shared<LocalProvider>(it->second);
});
