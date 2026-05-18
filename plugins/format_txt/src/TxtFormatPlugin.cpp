// TXT (and Markdown) reader-format plugin. Same shape as format_xtc — owns
// the reader Activity, registers via PluginReaderFormat. Markdown matches the
// TXT extension predicate until there's a dedicated MD renderer.

#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <PluginManifest.h>
#include <Txt.h>

#include <memory>
#include <string>
#include <utility>

#include "TxtReaderActivity.h"
#include "components/themes/BaseTheme.h"  // UIIcon

namespace {

bool txtMatches(const char* path) {
  const std::string p(path);
  return FsHelpers::hasTxtExtension(p) || FsHelpers::hasMarkdownExtension(p);
}

std::unique_ptr<Activity> txtMakeReader(GfxRenderer& renderer, MappedInputManager& input, const std::string& path) {
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("TXT", "File does not exist: %s", path.c_str());
    return nullptr;
  }
  auto txt = std::unique_ptr<Txt>(new Txt(path, "/.crosspoint"));
  if (!txt->load()) {
    LOG_ERR("TXT", "Failed to load TXT");
    return nullptr;
  }
  return std::make_unique<TxtReaderActivity>(renderer, input, std::move(txt));
}

const PluginReaderFormat kFormats[] = {
    // No cover generator: TXT/MD files have no embedded thumbnails.
    {&txtMatches, &txtMakeReader, /*generateCoverThumb*/ nullptr, /*icon*/ Text},
};

}  // namespace

extern "C" const PluginManifest g_plugin_format_txt = {
    .id = "format_txt",
    .name = "TXT reader",
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
