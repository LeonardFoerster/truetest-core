#pragma once

#include <string>
#include <unordered_map>
#include <vector>

struct cli_options;  // forward from main.inc (defined in same TU for now)

/**
 * RunPreset system — named bundles of sensible defaults for common workflows.
 *
 * Goal: drastically reduce the number of raw flags users and scripts must remember,
 * while preserving full power of explicit CLI / --config overrides and reproducibility.
 *
 * Precedence (intended): explicit CLI flags > --config JSON > --preset bundle > hard defaults.
 *
 * Usage (after CLI parse):
 *   if (!opts.preset.empty()) apply_preset(opts.preset, opts);
 *
 * Presets are intentionally conservative and documented. They are the foundation
 * for both shorter command lines and future GUI "new session from template" pickers.
 *
 * This header is intentionally small and self-contained. The concrete bundles live
 * in the implementation (preset.cpp or inline in main.inc during bring-up).
 */
namespace truetest::presets {

/** Return a human-readable description for a preset (for --help / TUI). */
std::string preset_description(const std::string& name);

/** List of known preset names (for help, TUI menus, validation). */
std::vector<std::string> known_presets();

/**
 * Human-friendly summary of what a preset typically configures (used in --help
 * and future TUI preset browser). Keep this in sync with the apply logic.
 */
std::string preset_summary(const std::string& name);

/** Returns true for presets that the current binary understands. */
inline bool is_known_preset(const std::string& name)
{
    return name == "futures-phase0" || name == "phase0" || name == "futures-p0" ||
           name == "mc-robustness" || name == "mc" || name == "monte-carlo" ||
           name == "backtest-local-l2" || name == "backtest-l2" || name == "local-l2" ||
           name == "shadow-tape" || name == "shadow" || name == "paper";
}

} // namespace truetest::presets
