#pragma once

#include "PluginManifest.h"

#include <cstddef>

namespace PluginRegistry {

// All plugins compiled into this build, in declaration order from the generated
// registry. Empty when no plugins are present.
const PluginManifest* const* all();
size_t count();

// Convenience iteration helpers. `cb` receives each manifest pointer; the
// callback type is `void(*)(const PluginManifest&)` to keep std::function out
// of every iteration site.
void forEach(void (*cb)(const PluginManifest&));

// Boot-time fan-out: invokes every plugin's onBoot hook (those that supplied one).
// Called once from main.cpp after settings + I18n load.
void onBoot();

}  // namespace PluginRegistry
