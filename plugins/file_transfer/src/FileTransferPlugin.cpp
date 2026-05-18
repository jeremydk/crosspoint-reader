// File Transfer plugin manifest. One surface: a home-menu row that launches
// the CrossPointWebServerActivity (which spins up the AP-mode WebServer +
// WebDAV handler and serves the file-management web UI).
//
// The plugin reaches into a couple of core stores by include — FontInstaller,
// WifiCredentialStore, SdCardFontSystem (all in src/) — plus into the OPDS
// plugin's OpdsServerStore for rendering the OPDS server admin page. The
// library.json dependency on PluginOpdsBrowser makes that cross-plugin link
// explicit. When the OPDS plugin is disabled, the OPDS panel of the web UI
// will fail to link, which is the intended coupling: if you turn off OPDS,
// you also turn off File Transfer (the web UI presents OPDS routes).

#include <I18n.h>
#include <PluginManifest.h>

#include <memory>

#include "CrossPointWebServerActivity.h"

namespace {

std::unique_ptr<Activity> launchFileTransfer(GfxRenderer& renderer, MappedInputManager& input) {
  return std::make_unique<CrossPointWebServerActivity>(renderer, input);
}

const PluginHomeMenuEntry kHomeMenuEntries[] = {
    {StrId::STR_FILE_TRANSFER, nullptr, &launchFileTransfer},
};

}  // namespace

extern "C" const PluginManifest g_plugin_file_transfer = {
    .id = "file_transfer",
    .name = "File Transfer",
    .onBoot = nullptr,
    .settingsMenuEntries = nullptr,
    .settingsMenuEntryCount = 0,
    .readerMenuActions = nullptr,
    .readerMenuActionCount = 0,
    .homeMenuEntries = kHomeMenuEntries,
    .homeMenuEntryCount = 1,
    .themes = nullptr,
    .themeCount = 0,
    .appendWebSettings = nullptr,
};
