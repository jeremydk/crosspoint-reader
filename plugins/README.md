# Authoring a CrossPoint plugin

This is a build-time plugin system. There is no runtime loader (ESP32-C3 has
no `dlopen`), so a plugin is a directory of C++ sources that compile into the
firmware when present and disappear when absent. Toggling a plugin off with
`"enabled": false` removes its sources from the build, its manifest from the
registry, and — for everything reachable only through plugin code — its
contributions from the linked binary.

The aim: keep `master` lean. Themes, reader-format support, sync clients,
niche OPDS dialects, and personal experiments belong here rather than as new
top-level features in `src/` or `lib/`.

## Layout

```
plugins/
  <plugin_id>/
    plugin.json            # manifest descriptor (id, symbol, build flags)
    library.json           # PlatformIO library descriptor (deps, build flags)
    src/                   # C++ sources, compiled when the plugin is enabled
    src/html/              # optional: HTML/JS assets for plugins that serve
                           # web pages; build_html.py finds them automatically
    i18n/                  # optional: per-language YAML files (not yet
                           # merged by gen_i18n.py; see "i18n" below)
```

The plugin's `library.json` makes it a normal PlatformIO library, sitting
under `lib_extra_dirs = plugins` in `platformio.ini`. `scripts/gen_plugins.py`
walks `plugins/*/plugin.json`, generates `lib/PluginCore/PluginRegistry.generated.h`
declaring each manifest as `extern "C"`, and at SCons time mutates `lib_deps`
to force-add each plugin's library name (without that, the LDF wouldn't
chase the extern references and the manifest symbols would come up undefined
at link time).

## `plugin.json`

```json
{
  "id": "koreader_sync",
  "name": "KOReader Sync",
  "version": "1.0.0",
  "manifest_symbol": "g_plugin_koreader_sync",
  "enabled": true,
  "build_flags": [],
  "i18n": []
}
```

| field | meaning |
|---|---|
| `id` | Stable identifier used in logs and as a prefix for generated symbols. |
| `name` | Human-readable fallback. Settings menus prefer the translated `StrId`. |
| `manifest_symbol` | The plugin's `PluginManifest` variable name. The plugin defines it with `extern "C"` linkage so the symbol name is predictable; the build script emits a matching `extern "C" const PluginManifest <symbol>;` declaration in the registry. |
| `enabled` | Set `false` to compile the plugin out without removing the directory. Defaults to `true`. |
| `build_flags` | Optional flags appended to the env's compile flags when the plugin is enabled. |
| `i18n` | Optional list of subdirectories whose `*.yaml` files get scanned for `STR_*` references (see "i18n" below). |

## `library.json`

Standard PlatformIO library descriptor. `dependencies` declares which other
libraries this plugin needs — both core libs (`Logging`, `I18n`, `Epub`, …)
and other plugins by their library name (e.g. `PluginCore`). The build
system applies a global include path that covers `src/`, `src/activities/`,
`src/activities/reader/`, `src/activities/settings/`, and `src/util/`, so
plugin sources can write `#include "activities/Activity.h"` directly.

## The manifest

Every plugin defines exactly one `PluginManifest` (see
[`lib/PluginCore/PluginManifest.h`](../lib/PluginCore/PluginManifest.h)) with
C linkage. Plain struct of function pointers — no `std::function`, no virtual
dispatch, nothing that lands in the heap. Surfaces the plugin doesn't use
stay nullable.

```cpp
extern "C" const PluginManifest g_plugin_hello = {
    .id = "hello",
    .name = "Hello (stub)",
    .onBoot = &helloOnBoot,
    .settingsMenuEntries = nullptr,
    .settingsMenuEntryCount = 0,
    // every other surface left to its zero default
};
```

### Extension surfaces

| field | what it adds | notes |
|---|---|---|
| `onBoot` | Function called once at firmware startup, after settings and i18n load. | Hydrate credential/state files here (e.g. `KOREADER_STORE.loadFromFile()`). |
| `settingsMenuEntries` | Rows in System settings → opens an Activity when selected. | `{StrId label, std::unique_ptr<Activity>(*launch)(renderer, input)}`. |
| `readerMenuActions` | Rows in the reader's options menu. | Plugin gets a `ReaderActionContext&` so it can read position, save progress, and call `releaseEpubAndReplace` for memory-hungry handoffs. |
| `homeMenuEntries` | Rows on the home screen between Recents and Settings. | `isAvailable` gates visibility (e.g. OPDS only when servers are configured). |
| `themes` | One or more theme variants. | `{id, label, make(), metrics*}`. `id` reuses `CrossPointSettings::UI_THEME` values for save-file compatibility. |
| `webRoutes` | HTTP route handlers contributed to the File Transfer web server. | `{path, method (HTTPMethod int), handler(WebServer&)}`. File Transfer registers these alongside its built-in routes. |
| `readerFormats` | Book-format support. | `{matches(path), makeReader(...), generateCoverThumb(path,h), icon}`. ReaderActivity walks these for dispatch; HomeActivity uses `generateCoverThumb`; UITheme uses `icon`. |
| `appendWebSettings` | Contributes `SettingInfo` rows to the web config page. | Uses the existing `SettingInfo::DynamicString`, `Toggle`, etc. helpers — no parallel API. |

### `ReaderActionContext`

Passed to reader-menu action handlers. Exposes:

- `getEpub()` — the active `std::shared_ptr<Epub>`
- `getEpubPath()` / `getCurrentSpineIndex()` / `getCurrentPage()` / `getTotalPages()` / `getCurrentParagraphIndex()` — read-only state
- `getRenderer()` / `getInput()` — pass-throughs for constructing the next activity
- `saveProgress()` — persist current position, returns `false` on disk failure
- `releaseEpubAndReplace(newActivity)` — frees the ~65 KB Epub state and switches activities; the standard handoff for plugins that need TLS headroom
- `flashSaveErrorPopup()` — arm a "save failed" popup that the reader shows on its next render

### `PluginReaderFormat` (book formats)

```cpp
struct PluginReaderFormat {
  bool (*matches)(const char* path);
  std::unique_ptr<Activity> (*makeReader)(GfxRenderer&, MappedInputManager&, const std::string& path);
  bool (*generateCoverThumb)(const char* path, int coverHeight);  // nullable
  int icon;                                                       // UIIcon enum value
};
```

`matches` is the dispatch predicate. `makeReader` returns the Activity to
push when the user opens a matching file (the function loads the file
internally — if loading fails it returns `nullptr` and the dispatcher falls
through to the next handler or to the EPUB built-in). `generateCoverThumb`
is called by `HomeActivity` to populate the Continue Reading card. `icon`
is what the file browser and recent-books list display.

EPUB stays in core as the fallback when no plugin format claims a file.
XTC and TXT live in `plugins/format_xtc/` and `plugins/format_txt/`.

**Coupling still in core** (as of the cover hook landing): a format's parser
lib (e.g. `lib/Xtc/`) is still pulled in by `SleepActivity` (sleep-screen
cover regeneration), `RecentBooksStore` (metadata extraction when a book
is opened), and `FileBrowserActivity` (extension filter for "show readable
files"). Until those three sites get their own hooks on `PluginReaderFormat`,
disabling a format plugin frees its reader Activity but leaves the parser
in flash. The hook shape is in place; the remaining migrations are
mechanical.

## i18n

**v1 limitation.** Plugin string YAML files are not yet merged by
`gen_i18n.py`. As a partial measure, the scan widened to include
`plugins/`, so `STR_*` identifiers referenced from plugin sources survive
`--strip-unused` — but the *definitions* of those strings still need to
live in `lib/I18n/translations/`. KOReader's `STR_KOREADER_USERNAME` is the
canonical example: it appears in `english.yaml` even though all consumers
are in the plugin.

`gen_plugins.py` already writes `.cache/plugin-i18n-dirs.txt` listing each
enabled plugin's `i18n` directory. Wiring `gen_i18n.py` to read that
sidecar and merge per-plugin YAMLs into the global string table is the
next planned cleanup.

## Plugin loading and registration

There is no static init at plugin level. The plugin's manifest is a
constant `extern "C"` object whose address is collected into a generated
registry array; core code iterates that array at startup (`onBoot`,
`homeMenuEntries`, theme registry) or on demand (settings menu rebuild,
reader-menu items, web-route registration, file-icon lookup).

`gen_plugins.py` runs as a PlatformIO `pre:` script — before `gen_i18n.py`,
so the i18n scan sees plugin sources, and before any compilation, so the
generated registry header is in place by the time `PluginRegistry.cpp`
compiles.

## Disabling a plugin

Set `"enabled": false` in `plugin.json`, or delete the directory. The
build script omits the plugin from the registry, from `lib_deps`, and
from the i18n scan. The plugin's sources stop compiling; symbols that
were reachable only through plugin code drop from the binary; the
linker strips any dependencies that no longer have consumers.

Combining disables compounds: disabling all four network plugins
(KOReader, OTA, OPDS, File Transfer) frees ~440 KB of flash because the
TLS stack and large parts of WiFi/networking only get linked when some
plugin asks for them.

## Adding a new plugin: the short version

1. Make a directory: `plugins/<your_id>/`.
2. Write `plugin.json` with a unique `id` and `manifest_symbol`.
3. Write `library.json` declaring whatever PlatformIO libs you depend on
   (plus `PluginCore`, always).
4. Put your sources under `src/`. The first .cpp file that defines your
   manifest is what matters; the rest is whatever your plugin needs.
5. Declare your manifest with C linkage and the name from `plugin.json`:
   ```cpp
   extern "C" const PluginManifest g_plugin_your_id = {
       .id = "your_id",
       .name = "Your Plugin",
       /* fill in any hooks you want; leave the rest zero */
   };
   ```
6. `pio run`. `gen_plugins.py` will pick it up, generate the registry,
   and PlatformIO will compile and link it. To turn it off, flip
   `"enabled": false` and rebuild.

## Why no runtime loader?

ESP32-C3 has 380 KB of usable RAM, no PSRAM, and no dynamic linker. The
firmware is built as one ELF and flashed in one go. Build-time plugins
keep the per-plugin runtime cost minimal (a const struct of function
pointers in flash) and give the linker the visibility it needs to strip
disabled code.
