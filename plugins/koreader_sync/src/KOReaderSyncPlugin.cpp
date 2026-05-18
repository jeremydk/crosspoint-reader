// KOReader Sync plugin manifest. Defines every surface this plugin contributes:
//
//   - onBoot                 — load credentials from SPIFFS
//   - settingsMenuEntries    — "KOReader Sync" row under System settings
//   - readerMenuActions      — "Sync Progress" row in the reader menu, gated on
//                              credentials being configured
//   - appendWebSettings      — username/password/server/match-method fields on
//                              the web config page
//
// Everything KOReader-specific that previously lived in src/, lib/I18n strings
// aside (deferred until plugin i18n is wired), now flows through this single
// translation unit and the headers shipped alongside it.

#include <Epub.h>
#include <I18n.h>
#include <Logging.h>
#include <PluginManifest.h>
#include <ReaderActionContext.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "KOReaderCredentialStore.h"
#include "KOReaderSettingsActivity.h"
#include "KOReaderSyncActivity.h"
#include "ProgressMapper.h"
#include "activities/settings/SettingsActivity.h"  // SettingInfo

namespace {

void koreaderOnBoot() { KOREADER_STORE.loadFromFile(); }

std::unique_ptr<Activity> launchKoreaderSettings(GfxRenderer& renderer, MappedInputManager& input) {
  return std::make_unique<KOReaderSettingsActivity>(renderer, input);
}

bool koreaderSyncAvailable() { return KOREADER_STORE.hasCredentials(); }

void koreaderSyncOnSelected(ReaderActionContext& ctx) {
  std::shared_ptr<Epub> epub = ctx.getEpub();
  if (!epub) return;

  const int spineIdx = ctx.getCurrentSpineIndex();
  const int currentPage = ctx.getCurrentPage();
  const int totalPages = ctx.getTotalPages();
  const std::optional<uint16_t> paragraphIndex = ctx.getCurrentParagraphIndex();

  CrossPointPosition localPos = {spineIdx, currentPage, totalPages};
  if (paragraphIndex.has_value()) {
    localPos.paragraphIndex = *paragraphIndex;
    localPos.hasParagraphIndex = true;
  }
  KOReaderPosition localKoPos = ProgressMapper::toKOReader(epub, localPos);
  const int tocIdx = epub->getTocIndexForSpineIndex(spineIdx);
  std::string localChapterName = (tocIdx >= 0) ? epub->getTocItem(tocIdx).title : "";
  const std::string savedEpubPath = ctx.getEpubPath();

  // saveProgress must succeed before the handoff: KOReaderSyncActivity returns
  // through goToReader(), which depends on the persisted position file.
  if (!ctx.saveProgress()) {
    LOG_ERR("KOSync", "Aborting sync: saveProgress failed");
    ctx.flashSaveErrorPopup();
    return;
  }

  GfxRenderer& renderer = ctx.getRenderer();
  MappedInputManager& input = ctx.getInput();
  auto next = std::make_unique<KOReaderSyncActivity>(renderer, input, savedEpubPath, spineIdx, currentPage, totalPages,
                                                     std::move(localKoPos), std::move(localChapterName),
                                                     paragraphIndex);

  // Drop our shared_ptr before the release so the reader's reset() is the last
  // owner; otherwise the Epub stays in RAM through the TLS handshake.
  epub.reset();
  ctx.releaseEpubAndReplace(std::move(next));
}

void koreaderAppendWebSettings(std::vector<SettingInfo>& v) {
  v.push_back(SettingInfo::DynamicString(
      StrId::STR_KOREADER_USERNAME, [] { return KOREADER_STORE.getUsername(); },
      [](const std::string& s) {
        KOREADER_STORE.setCredentials(s, KOREADER_STORE.getPassword());
        KOREADER_STORE.saveToFile();
      },
      "koUsername", StrId::STR_KOREADER_SYNC));
  v.push_back(SettingInfo::DynamicString(
      StrId::STR_KOREADER_PASSWORD, [] { return KOREADER_STORE.getPassword(); },
      [](const std::string& s) {
        KOREADER_STORE.setCredentials(KOREADER_STORE.getUsername(), s);
        KOREADER_STORE.saveToFile();
      },
      "koPassword", StrId::STR_KOREADER_SYNC));
  v.push_back(SettingInfo::DynamicString(
      StrId::STR_SYNC_SERVER_URL, [] { return KOREADER_STORE.getServerUrl(); },
      [](const std::string& s) {
        KOREADER_STORE.setServerUrl(s);
        KOREADER_STORE.saveToFile();
      },
      "koServerUrl", StrId::STR_KOREADER_SYNC));
  v.push_back(SettingInfo::DynamicEnum(
      StrId::STR_DOCUMENT_MATCHING, {StrId::STR_FILENAME, StrId::STR_BINARY},
      [] { return static_cast<uint8_t>(KOREADER_STORE.getMatchMethod()); },
      [](uint8_t method) {
        KOREADER_STORE.setMatchMethod(static_cast<DocumentMatchMethod>(method));
        KOREADER_STORE.saveToFile();
      },
      "koMatchMethod", StrId::STR_KOREADER_SYNC));
}

const PluginSettingsMenuEntry kSettingsMenuEntries[] = {
    {StrId::STR_KOREADER_SYNC, &launchKoreaderSettings},
};

const PluginReaderMenuAction kReaderMenuActions[] = {
    {StrId::STR_SYNC_PROGRESS, &koreaderSyncAvailable, &koreaderSyncOnSelected},
};

}  // namespace

extern "C" const PluginManifest g_plugin_koreader_sync = {
    .id = "koreader_sync",
    .name = "KOReader Sync",
    .onBoot = &koreaderOnBoot,
    .settingsMenuEntries = kSettingsMenuEntries,
    .settingsMenuEntryCount = 1,
    .readerMenuActions = kReaderMenuActions,
    .readerMenuActionCount = 1,
    .appendWebSettings = &koreaderAppendWebSettings,
};
