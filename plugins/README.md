# CrossPoint plugins

This directory holds **build-time** components. There is no runtime loader — the
ESP32-C3 has no `dlopen` — so a plugin is just a directory whose sources get
compiled into the firmware when present and stripped out when absent.

The aim is to keep `master` lean: things like themes, sync clients, niche OPDS
dialects, and personal experiments belong here rather than as new top-level
features in `src/` or `lib/`.

## Layout

```
plugins/
  <plugin_id>/
    plugin.json            # manifest descriptor (see schema below)
    src/                   # plugin C++ sources (compiled when the plugin is enabled)
    include/               # plugin public headers (added to include path)
    i18n/                  # optional per-plugin translation YAML files
    library.json           # standard PlatformIO library descriptor (optional)
```

`scripts/gen_plugins.py` walks every subdirectory containing `plugin.json`,
generates `lib/PluginCore/PluginRegistry.generated.h`, and tells PlatformIO to
include the plugin's `src/` as a library.

## plugin.json

```json
{
  "id": "koreader_sync",
  "name": "KOReader Sync",
  "version": "1.0.0",
  "manifest_symbol": "g_plugin_koreader_sync",
  "enabled": true,
  "build_flags": ["-DPLUGIN_KOREADER_SYNC=1"],
  "i18n": ["i18n"]
}
```

| field | meaning |
|---|---|
| `id` | Stable identifier. Used in logs and as the prefix for generated symbol names. |
| `name` | Human-readable fallback. Settings menus prefer the translated `StrId`. |
| `manifest_symbol` | The plugin's `PluginManifest` variable name. The plugin defines it with `extern "C"` linkage so the symbol name is predictable across compilers; the build script emits a matching `extern "C" const PluginManifest <symbol>;` declaration in the registry array. |
| `enabled` | Set `false` to compile the plugin out without removing the directory. Defaults to `true`. |
| `build_flags` | Optional flags appended to the env's compile flags when the plugin is enabled. |
| `i18n` | Optional list of subdirectories whose `*.yaml` files get merged into the global i18n generator's scan. |

## The manifest

Every plugin declares exactly one `PluginManifest` (see
[`lib/PluginCore/PluginManifest.h`](../lib/PluginCore/PluginManifest.h)). It is a
plain struct of function pointers — no `std::function`, no virtual dispatch,
nothing that would land in the heap. Surfaces the plugin doesn't use stay
nullable.

Available extension surfaces:

- **`onBoot`** — runs once at firmware startup after settings and i18n load. Use
  for loading plugin-specific credential or state files.
- **`settingsMenuEntries`** — rows appended to the System settings menu. Each
  entry's `launch` returns the `Activity` to push when the user selects it.
- **`readerMenuActions`** — rows appended to the reader's options menu. The
  handler receives a `ReaderActionContext` that exposes the current position
  and the `releaseEpubAndReplace` helper for memory-hungry handoffs (anything
  doing TLS).
- **`appendWebSettings`** — contributes `SettingInfo` rows to the web config
  page using the existing `SettingInfo::DynamicString`, `Toggle`, etc.
  helpers. No parallel API.

## i18n

**v1 limitation:** plugin string contributions are not wired through `gen_i18n.py`
yet — `STR_KOREADER_*` and friends still live in `lib/I18n/translations/`. Until
that's done, a slim build that disables KOReader still pays for its strings.
`gen_plugins.py` already writes `.cache/plugin-i18n-dirs.txt`; extending
`gen_i18n.py` to read it is the next planned cleanup.

Once that lands, plugins will drop per-language YAML files in `i18n/` and
reference keys via the global `tr(STR_FOO)` macro. There is no per-plugin
string namespace; if two plugins define the same `STR_*` key the build script
aborts.

## Disabling a plugin

Set `"enabled": false` in `plugin.json`, or delete the directory. Either way
the plugin's sources stop compiling and its manifest stops appearing in the
generated registry.
