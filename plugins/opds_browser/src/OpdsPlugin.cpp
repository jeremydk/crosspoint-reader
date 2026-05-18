// OPDS Browser plugin manifest. Three surfaces:
//   - onBoot: hydrate the server list from SD before any activity reads it
//   - homeMenuEntries: a "Library" row that's visible only when at least one
//     server is configured, and routes straight to the book browser when
//     there's exactly one server (skipping the picker)
//   - settingsMenuEntries: an "OPDS Servers" row for managing the list

#include <I18n.h>
#include <PluginManifest.h>

#include <memory>

#include "OpdsBookBrowserActivity.h"
#include "OpdsServerListActivity.h"
#include "OpdsServerStore.h"
#include "activities/settings/SettingsActivity.h"  // SettingInfo

namespace {

void opdsOnBoot() { OPDS_STORE.loadFromFile(); }

bool opdsHasServers() { return OPDS_STORE.hasServers(); }

std::unique_ptr<Activity> launchOpdsBrowser(GfxRenderer& renderer, MappedInputManager& input) {
  const auto& servers = OPDS_STORE.getServers();
  if (servers.size() == 1) {
    return std::make_unique<OpdsBookBrowserActivity>(renderer, input, servers[0]);
  }
  return std::make_unique<OpdsServerListActivity>(renderer, input, true);
}

std::unique_ptr<Activity> launchOpdsServerList(GfxRenderer& renderer, MappedInputManager& input) {
  return std::make_unique<OpdsServerListActivity>(renderer, input);
}

const PluginSettingsMenuEntry kSettingsMenuEntries[] = {
    {StrId::STR_OPDS_SERVERS, &launchOpdsServerList},
};

const PluginHomeMenuEntry kHomeMenuEntries[] = {
    {StrId::STR_OPDS_BROWSER, &opdsHasServers, &launchOpdsBrowser},
};

}  // namespace

// Defined in OpdsWebRoutes.cpp.
extern const PluginWebRoute kOpdsWebRoutes[];
extern const uint8_t kOpdsWebRouteCount;

extern "C" const PluginManifest g_plugin_opds_browser = {
    .id = "opds_browser",
    .name = "OPDS Browser",
    .onBoot = &opdsOnBoot,
    .settingsMenuEntries = kSettingsMenuEntries,
    .settingsMenuEntryCount = 1,
    .readerMenuActions = nullptr,
    .readerMenuActionCount = 0,
    .homeMenuEntries = kHomeMenuEntries,
    .homeMenuEntryCount = 1,
    .themes = nullptr,
    .themeCount = 0,
    .webRoutes = kOpdsWebRoutes,
    .webRouteCount = 3,
    .appendWebSettings = nullptr,
};
