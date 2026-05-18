// OTA Update plugin manifest. Surfaces a single System settings row
// ("Check for Updates") that launches the OTA flow. No reader-menu hook, no
// web settings, no boot-time state to load.

#include <I18n.h>
#include <PluginManifest.h>

#include <memory>

#include "OtaUpdateActivity.h"
#include "activities/settings/SettingsActivity.h"  // SettingInfo, kept indirect

namespace {

std::unique_ptr<Activity> launchOtaUpdate(GfxRenderer& renderer, MappedInputManager& input) {
  return std::make_unique<OtaUpdateActivity>(renderer, input);
}

const PluginSettingsMenuEntry kSettingsMenuEntries[] = {
    {StrId::STR_CHECK_UPDATES, &launchOtaUpdate},
};

}  // namespace

extern "C" const PluginManifest g_plugin_ota_update = {
    .id = "ota_update",
    .name = "OTA Update",
    .onBoot = nullptr,
    .settingsMenuEntries = kSettingsMenuEntries,
    .settingsMenuEntryCount = 1,
    .readerMenuActions = nullptr,
    .readerMenuActionCount = 0,
    .appendWebSettings = nullptr,
};
