// RoundedRaff theme plugin. Registers a single theme at id=3, the same value
// CrossPointSettings::UI_THEME::ROUNDEDRAFF used to hardcode, so saved
// uiTheme selections survive the extraction.

#include <CrossPointSettings.h>
#include <I18n.h>
#include <PluginManifest.h>

#include <memory>

#include "RoundedRaffTheme.h"

namespace {

std::unique_ptr<BaseTheme> makeRoundedRaff() { return std::make_unique<RoundedRaffTheme>(); }

const PluginThemeEntry kThemes[] = {
    {static_cast<uint8_t>(CrossPointSettings::UI_THEME::ROUNDEDRAFF), StrId::STR_THEME_ROUNDEDRAFF, &makeRoundedRaff,
     &RoundedRaffMetrics::values},
};

}  // namespace

extern "C" const PluginManifest g_plugin_theme_roundedraff = {
    .id = "theme_roundedraff",
    .name = "RoundedRaff theme",
    .onBoot = nullptr,
    .settingsMenuEntries = nullptr,
    .settingsMenuEntryCount = 0,
    .readerMenuActions = nullptr,
    .readerMenuActionCount = 0,
    .homeMenuEntries = nullptr,
    .homeMenuEntryCount = 0,
    .themes = kThemes,
    .themeCount = 1,
    .appendWebSettings = nullptr,
};
