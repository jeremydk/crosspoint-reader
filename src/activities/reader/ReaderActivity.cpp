#include "ReaderActivity.h"

#include <FsHelpers.h>
#include <HalStorage.h>

#include "CrossPointSettings.h"
#include "Epub.h"
#include "EpubReaderActivity.h"
#include "PluginManifest.h"
#include "PluginRegistry.h"
#include "SdCardFontSystem.h"
#include "activities/util/BmpViewerActivity.h"
#include "activities/util/FullScreenMessageActivity.h"

bool ReaderActivity::isBmpFile(const std::string& path) { return FsHelpers::hasBmpExtension(path); }

std::unique_ptr<Epub> ReaderActivity::loadEpub(const std::string& path) {
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return nullptr;
  }

  auto epub = std::unique_ptr<Epub>(new Epub(path, "/.crosspoint"));
  if (epub->load(true, SETTINGS.embeddedStyle == 0)) {
    return epub;
  }

  LOG_ERR("READER", "Failed to load epub");
  return nullptr;
}

void ReaderActivity::goToLibrary(const std::string& fromBookPath) {
  // If coming from a book, start in that book's folder; otherwise start from root
  auto initialPath = fromBookPath.empty() ? "/" : FsHelpers::extractFolderPath(fromBookPath);
  activityManager.goToFileBrowser(std::move(initialPath));
}

void ReaderActivity::onGoToEpubReader(std::unique_ptr<Epub> epub) {
  const auto epubPath = epub->getPath();
  currentBookPath = epubPath;
  activityManager.replaceActivity(std::make_unique<EpubReaderActivity>(renderer, mappedInput, std::move(epub)));
}

void ReaderActivity::onGoToBmpViewer(const std::string& path) {
  activityManager.replaceActivity(std::make_unique<BmpViewerActivity>(renderer, mappedInput, path));
}

void ReaderActivity::onEnter() {
  Activity::onEnter();

  if (initialBookPath.empty()) {
    goToLibrary();  // Start from root when entering via Browse
    return;
  }

  sdFontSystem.ensureLoaded(renderer);

  currentBookPath = initialBookPath;
  if (isBmpFile(initialBookPath)) {
    onGoToBmpViewer(initialBookPath);
    return;
  }

  // Plugin-contributed formats get first crack. The first one whose `matches`
  // accepts the path wins; if none do (or its loader fails), fall through to
  // the TXT/EPUB built-ins below.
  for (size_t i = 0; i < PluginRegistry::count(); ++i) {
    const PluginManifest* m = PluginRegistry::all()[i];
    if (!m) continue;
    for (uint8_t j = 0; j < m->readerFormatCount; ++j) {
      const PluginReaderFormat& f = m->readerFormats[j];
      if (!f.matches || !f.matches(initialBookPath.c_str())) continue;
      auto next = f.makeReader ? f.makeReader(renderer, mappedInput, initialBookPath) : nullptr;
      if (!next) {
        onGoBack();
        return;
      }
      currentBookPath = initialBookPath;
      activityManager.replaceActivity(std::move(next));
      return;
    }
  }

  auto epub = loadEpub(initialBookPath);
  if (!epub) {
    onGoBack();
    return;
  }
  onGoToEpubReader(std::move(epub));
}

void ReaderActivity::onGoBack() { finish(); }
