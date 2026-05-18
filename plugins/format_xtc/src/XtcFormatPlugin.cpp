// XTC reader-format plugin. Owns the reader Activity (and its
// chapter-selection sub-activity) and registers itself with the core
// dispatcher via PluginReaderFormat. The shared parsing library stays in
// lib/Xtc/ because the home-screen cover path still uses it directly;
// extracting that coupling is the natural next step (a cover-thumbnail hook
// on the same PluginReaderFormat struct).

#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <PluginManifest.h>
#include <Xtc.h>

#include <memory>
#include <string>
#include <utility>

#include "XtcReaderActivity.h"

namespace {

bool xtcMatches(const char* path) { return FsHelpers::hasXtcExtension(path); }

std::unique_ptr<Activity> xtcMakeReader(GfxRenderer& renderer, MappedInputManager& input, const std::string& path) {
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("XTC", "File does not exist: %s", path.c_str());
    return nullptr;
  }
  auto xtc = std::unique_ptr<Xtc>(new Xtc(path, "/.crosspoint"));
  if (!xtc->load()) {
    LOG_ERR("XTC", "Failed to load XTC");
    return nullptr;
  }
  return std::make_unique<XtcReaderActivity>(renderer, input, std::move(xtc));
}

const PluginReaderFormat kFormats[] = {
    {&xtcMatches, &xtcMakeReader},
};

}  // namespace

extern "C" const PluginManifest g_plugin_format_xtc = {
    .id = "format_xtc",
    .name = "XTC reader",
    .onBoot = nullptr,
    .settingsMenuEntries = nullptr,
    .settingsMenuEntryCount = 0,
    .readerMenuActions = nullptr,
    .readerMenuActionCount = 0,
    .homeMenuEntries = nullptr,
    .homeMenuEntryCount = 0,
    .themes = nullptr,
    .themeCount = 0,
    .webRoutes = nullptr,
    .webRouteCount = 0,
    .readerFormats = kFormats,
    .readerFormatCount = 1,
    .appendWebSettings = nullptr,
};
