// Lyra theme plugin. Ships two themes that share a base class:
// the original Lyra (id=1, matching the legacy UI_THEME::LYRA enum value) and
// Lyra 3-Covers (id=2). IDs are deliberately the same values core used to
// hardcode in CrossPointSettings::UI_THEME, so saved settings keep working.

#include <CrossPointSettings.h>
#include <I18n.h>
#include <PluginManifest.h>

#include <memory>

#include "Lyra3CoversTheme.h"
#include "LyraTheme.h"

namespace {

std::unique_ptr<BaseTheme> makeLyra() { return std::make_unique<LyraTheme>(); }
std::unique_ptr<BaseTheme> makeLyra3Covers() { return std::make_unique<Lyra3CoversTheme>(); }

const PluginThemeEntry kThemes[] = {
    {static_cast<uint8_t>(CrossPointSettings::UI_THEME::LYRA), StrId::STR_THEME_LYRA, &makeLyra, &LyraMetrics::values},
    {static_cast<uint8_t>(CrossPointSettings::UI_THEME::LYRA_3_COVERS), StrId::STR_THEME_LYRA_EXTENDED,
     &makeLyra3Covers, &Lyra3CoversMetrics::values},
};

}  // namespace

extern "C" const PluginManifest g_plugin_theme_lyra = {
    .id = "theme_lyra",
    .name = "Lyra theme",
    .onBoot = nullptr,
    .settingsMenuEntries = nullptr,
    .settingsMenuEntryCount = 0,
    .readerMenuActions = nullptr,
    .readerMenuActionCount = 0,
    .homeMenuEntries = nullptr,
    .homeMenuEntryCount = 0,
    .themes = kThemes,
    .themeCount = 2,
    .appendWebSettings = nullptr,
};
