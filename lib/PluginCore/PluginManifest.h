#pragma once

#include <I18n.h>

#include <cstdint>
#include <memory>
#include <vector>

class Activity;
class BaseTheme;
class GfxRenderer;
class MappedInputManager;
class ReaderActionContext;
class WebServer;
struct SettingInfo;
struct ThemeMetrics;

// PluginSettingsMenuEntry: one row in the System settings menu.
// `launch` returns the activity to push when the row is activated.
struct PluginSettingsMenuEntry {
  StrId label;
  std::unique_ptr<Activity> (*launch)(GfxRenderer& renderer, MappedInputManager& input);
};

// PluginHomeMenuEntry: one row in the home-screen menu.
// `isAvailable` (nullable; null means always visible) gates the row for the
// current device state — OPDS, for example, only shows when at least one
// server is configured.
struct PluginHomeMenuEntry {
  StrId label;
  bool (*isAvailable)();
  std::unique_ptr<Activity> (*launch)(GfxRenderer& renderer, MappedInputManager& input);
};

// PluginReaderMenuAction: an extra row in EpubReaderMenuActivity's options menu.
// `isAvailable` (nullable; null means always visible) gates whether the row appears
// for the current book + state — KOReader sync, for example, only shows when
// credentials are configured.
// `onSelected` runs with a context that exposes the current position and a
// release-and-handoff helper for memory-hungry next activities (TLS, etc.).
struct PluginReaderMenuAction {
  StrId label;
  bool (*isAvailable)();
  void (*onSelected)(ReaderActionContext& ctx);
};

// PluginWebSettingsAppend: contributes SettingInfo rows to the web config page.
// The plugin appends to `out`. Existing SettingInfo helpers (DynamicString, Toggle, …)
// are reused so there is no parallel API to learn.
using PluginWebSettingsAppend = void (*)(std::vector<SettingInfo>& out);

// PluginReaderFormat: registers a book-format handler. The dispatcher
// (ReaderActivity in core) walks the registered formats in plugin-registration
// order, picks the first whose `matches` returns true, and launches the
// activity returned by `makeReader`. EPUB stays in core as the fallback when
// no plugin format matches.
//
// Beyond dispatch, the same entry contributes the cover-thumbnail generator
// and file icon used by HomeActivity / UITheme / the file browser. Without
// these, a format's parser lib stays linked even when the reader Activity is
// gone (covers and icons would still call into it), which defeats the disable.
struct PluginReaderFormat {
  bool (*matches)(const char* path);
  std::unique_ptr<Activity> (*makeReader)(GfxRenderer& renderer, MappedInputManager& input, const std::string& path);

  // Generate the cover thumbnail for `path` at the requested height. Returns
  // true on success. Nullable — formats without covers (TXT, PDF without
  // embedded thumbnails) leave it null and HomeActivity skips the cover.
  bool (*generateCoverThumb)(const char* path, int coverHeight);

  // UIIcon enum value (from src/components/themes/BaseTheme.h) shown in the
  // file browser and home recent-books list. Kept as int so PluginManifest.h
  // doesn't pull in the icon header.
  int icon;
};

// PluginWebRoute: one HTTP route the plugin contributes to the File Transfer
// web server. `method` is the int value of HTTPMethod from <WebServer.h>
// (HTTP_GET=0, HTTP_POST=3, …); kept as int so PluginManifest.h doesn't
// transitively pull in the Arduino-ESP32 WebServer header.
//
// The handler receives the active WebServer so it can read args and emit
// responses. Used to keep OPDS/Wifi/Font-management routes out of core: each
// plugin owns its own routes, and File Transfer's web server just iterates
// PluginRegistry and registers whatever the loaded plugins exposed.
using PluginWebRouteHandler = void (*)(WebServer& server);

struct PluginWebRoute {
  const char* path;
  int method;
  PluginWebRouteHandler handler;
};

// PluginThemeEntry: registers a UITheme variant. `id` is the stable numeric
// value stored in SETTINGS.uiTheme — themes use the existing UI_THEME enum
// values so saved settings survive a plugin disable (the lookup falls back to
// Classic when the requested theme isn't loaded). `metrics` lives in flash and
// is owned by the plugin; the theme instance returned by `make` is owned by
// UITheme.
struct PluginThemeEntry {
  uint8_t id;
  StrId label;
  std::unique_ptr<BaseTheme> (*make)();
  const ThemeMetrics* metrics;
};

// PluginManifest: the single source of truth for what a plugin contributes.
// Every field except `id` is optional. Leave the count zero and the pointer null
// for surfaces the plugin doesn't extend.
//
// One manifest object per plugin. The plugin defines its instance with C
// linkage (`extern "C" const PluginManifest g_plugin_xyz = { … };`) so the
// symbol name is predictable across compilers; scripts/gen_plugins.py emits
// matching `extern "C"` declarations in the registry header.
struct PluginManifest {
  const char* id;
  const char* name;

  void (*onBoot)();

  const PluginSettingsMenuEntry* settingsMenuEntries;
  uint8_t settingsMenuEntryCount;

  const PluginReaderMenuAction* readerMenuActions;
  uint8_t readerMenuActionCount;

  const PluginHomeMenuEntry* homeMenuEntries;
  uint8_t homeMenuEntryCount;

  const PluginThemeEntry* themes;
  uint8_t themeCount;

  const PluginWebRoute* webRoutes;
  uint8_t webRouteCount;

  const PluginReaderFormat* readerFormats;
  uint8_t readerFormatCount;

  PluginWebSettingsAppend appendWebSettings;
};
