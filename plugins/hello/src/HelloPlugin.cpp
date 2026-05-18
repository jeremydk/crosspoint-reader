// Minimal stub plugin. Exists to prove the build-time plugin pipeline works
// end-to-end: gen_plugins.py discovers this directory, PlatformIO compiles
// HelloPlugin.cpp via lib_extra_dirs, the linker resolves g_plugin_hello from
// PluginRegistry.cpp's extern declaration, and at boot the registry calls
// helloOnBoot() through the manifest's function pointer.
//
// Delete this directory (or set "enabled": false in plugin.json) to compile it
// out entirely.

#include <Logging.h>
#include <PluginManifest.h>

namespace {

void helloOnBoot() { LOG_INF("hello", "stub plugin loaded"); }

}  // namespace

extern "C" const PluginManifest g_plugin_hello = {
    .id = "hello",
    .name = "Hello (stub)",
    .onBoot = &helloOnBoot,
    .settingsMenuEntries = nullptr,
    .settingsMenuEntryCount = 0,
    .readerMenuActions = nullptr,
    .readerMenuActionCount = 0,
    .appendWebSettings = nullptr,
};
