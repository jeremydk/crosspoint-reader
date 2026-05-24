#include "EpubReaderActivity.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_system.h>

#include <functional>
#include <iterator>
#include <limits>

#include "ChapterPrebuilder.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "EpubReaderFootnotesActivity.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "EpubReaderUtils.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderSyncActivity.h"
#include "MappedInputManager.h"
#include "ProgressMapper.h"
#include "QrDisplayActivity.h"
#include "ReadFolderUtils.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ScreenshotUtil.h"

namespace {
constexpr int PAGE_TURN_RATES[] = {1, 1, 3, 6, 12};

int clampPercent(int percent) {
  if (percent < 0) return 0;
  if (percent > 100) return 100;
  return percent;
}
}  // namespace

void EpubReaderActivity::onEnter() {
  Activity::onEnter();

  if (!epub) {
    return;
  }

  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
  prebuilder_.attach();
  epub->setupCacheDir();

  FsFile f;
  if (Storage.openFileForRead("ERS", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[6];
    int dataSize = f.read(data, 6);
    if (dataSize == 4 || dataSize == 6) {
      currentSpineIndex = data[0] + (data[1] << 8);
      nextPageNumber = data[2] + (data[3] << 8);
      if (nextPageNumber == UINT16_MAX) {
        // UINT16_MAX is an in-memory navigation sentinel for "open previous chapter on
        // its last page" -- never valid as persisted resume state.
        LOG_DBG("ERS", "Ignoring stale last-page sentinel from progress cache");
        nextPageNumber = 0;
      }
      cachedSpineIndex = currentSpineIndex;
      LOG_DBG("ERS", "Loaded cache: %d, %d", currentSpineIndex, nextPageNumber);
    }
    if (dataSize == 6) {
      cachedChapterTotalPageCount = data[4] + (data[5] << 8);
    }
  }

  if (currentSpineIndex == 0) {
    int textSpineIndex = epub->getSpineIndexForTextReference();
    if (textSpineIndex != 0) {
      currentSpineIndex = textSpineIndex;
      LOG_DBG("ERS", "Opened for first time, navigating to text reference at index %d", textSpineIndex);
    }
  }

  APP_STATE.openEpubPath = epub->getPath();
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());

  requestUpdate();
}

void EpubReaderActivity::onExit() {
  Activity::onExit();

  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  // Unhook before dropping the build: a stray refresh-wait tick from teardown
  // must not fire into a half-destructed prebuilder.
  prebuilder_.detach();

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  section.reset();
  if (pendingReadFolderMove && epub) {
    const std::string srcPath = epub->getPath();
    const std::string oldCachePath = epub->getCachePath();
    const std::string dstPath = ReadFolderUtils::buildDestination(srcPath);
    epub.reset();
    ReadFolderUtils::moveFinishedBook(srcPath, dstPath, oldCachePath);
  } else {
    epub.reset();
  }
}

void EpubReaderActivity::loop() {
  if (!epub) {
    finish();
    return;
  }

  // pendingBuild access -- creation, drain, and per-render step -- all stays on
  // the render task (ActivityManager's renderTaskLoop). SdFat's shared volume state
  // must not be accessed from two tasks at once.

  const bool atEndOfBook = currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount();

  if (SETTINGS.removeReadBooksFromRecents) {
    if (atEndOfBook && !recentsEntryRemoved) {
      recentsEntryRemoved = RECENT_BOOKS.removeByPath(epub->getPath());
    } else if (!atEndOfBook && recentsEntryRemoved) {
      RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());
      recentsEntryRemoved = false;
    }
  }

  if (atEndOfBook) {
    pendingReadFolderMove = SETTINGS.moveFinishedToReadFolder && !ReadFolderUtils::isInReadFolder(epub->getPath());
  } else {
    pendingReadFolderMove = false;
  }

  if (automaticPageTurnActive) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      automaticPageTurnActive = false;
      requestUpdate();
      return;
    }

    if (!section) {
      requestUpdate();
      return;
    }

    if (RenderLock::peek()) {
      lastPageTurnTime = millis();
      return;
    }

    if ((millis() - lastPageTurnTime) >= pageTurnDuration) {
      pageTurn(true);
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const int currentPage = section ? section->currentPage + 1 : 0;
    const int totalPages = section ? section->pageCount : 0;
    float bookProgress = 0.0f;
    if (epub->getBookSize() > 0 && section && section->pageCount > 0) {
      const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
      bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
    }
    const int bookProgressPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
    startActivityForResult(std::make_unique<EpubReaderMenuActivity>(
                               renderer, mappedInput, epub->getTitle(), currentPage, totalPages, bookProgressPercent,
                               SETTINGS.orientation, !currentPageFootnotes.empty()),
                           [this](const ActivityResult& result) {
                             const auto& menu = std::get<MenuResult>(result.data);
                             applyOrientation(menu.orientation);
                             toggleAutoPageTurn(menu.pageTurnOption);
                             if (!result.isCancelled) {
                               onReaderMenuConfirm(static_cast<EpubReaderMenuActivity::MenuAction>(menu.action));
                             }
                           });
  }

  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
    activityManager.goToFileBrowser(epub ? epub->getPath() : "");
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    if (footnoteDepth > 0) {
      restoreSavedPosition();
      return;
    }
    onGoHome();
    return;
  }

  const auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  if (currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount()) {
    if (nextTriggered) {
      onGoHome();
    } else {
      currentSpineIndex = epub->getSpineItemsCount() - 1;
      nextPageNumber = 0;
      pendingPageJump = std::numeric_limits<uint16_t>::max();
      requestUpdate();
    }
    return;
  }

  const bool longPress = !fromTilt && mappedInput.getHeldTime() > ReaderUtils::SKIP_HOLD_MS;

  // Don't skip chapter after screenshot (both buttons released simultaneously)
  if (gpio.wasReleased(HalGPIO::BTN_POWER) && gpio.wasReleased(HalGPIO::BTN_DOWN)) {
    return;
  }

  if (longPress && SETTINGS.longPressButtonBehavior == SETTINGS.CHAPTER_SKIP) {
    {
      RenderLock lock(*this);
      nextPageNumber = 0;
      currentSpineIndex = nextTriggered ? currentSpineIndex + 1 : currentSpineIndex - 1;
      resetSectionForNavigation();
    }
    requestUpdate();
    return;
  }

  if (longPress && SETTINGS.longPressButtonBehavior == SETTINGS.ORIENTATION_CHANGE) {
    const uint8_t newOrientation =
        nextTriggered ? (SETTINGS.orientation - 1 + SETTINGS.ORIENTATION_COUNT) % SETTINGS.ORIENTATION_COUNT
                      : (SETTINGS.orientation + 1) % SETTINGS.ORIENTATION_COUNT;
    applyOrientation(newOrientation);
    requestUpdate();
    return;
  }

  if (!section) {
    requestUpdate();
    return;
  }

  if (prevTriggered) {
    pageTurn(false);
  } else {
    pageTurn(true);
  }
}

void EpubReaderActivity::jumpToPercent(int percent) {
  if (!epub) {
    return;
  }

  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) {
    return;
  }

  percent = clampPercent(percent);

  // Overflow-safe: (bookSize / 100) * percent + (bookSize % 100) * percent / 100.
  // Addition form `pos + n > total` wraps for large untrusted values.
  size_t targetSize =
      (bookSize / 100) * static_cast<size_t>(percent) + (bookSize % 100) * static_cast<size_t>(percent) / 100;
  if (percent >= 100) {
    targetSize = bookSize - 1;
  }

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount == 0) {
    return;
  }

  int targetSpineIndex = spineCount - 1;
  size_t prevCumulative = 0;

  for (int i = 0; i < spineCount; i++) {
    const size_t cumulative = epub->getCumulativeSpineItemSize(i);
    if (targetSize <= cumulative) {
      targetSpineIndex = i;
      prevCumulative = (i > 0) ? epub->getCumulativeSpineItemSize(i - 1) : 0;
      break;
    }
  }

  const size_t cumulative = epub->getCumulativeSpineItemSize(targetSpineIndex);
  const size_t spineSize = (cumulative > prevCumulative) ? (cumulative - prevCumulative) : 0;
  pendingSpineProgress =
      (spineSize == 0) ? 0.0f : static_cast<float>(targetSize - prevCumulative) / static_cast<float>(spineSize);
  if (pendingSpineProgress < 0.0f) {
    pendingSpineProgress = 0.0f;
  } else if (pendingSpineProgress > 1.0f) {
    pendingSpineProgress = 1.0f;
  }

  {
    RenderLock lock(*this);
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    pendingPercentJump = true;
    resetSectionForNavigation();
  }
}

void EpubReaderActivity::onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action) {
  switch (action) {
    case EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER: {
      const int spineIdx = currentSpineIndex;
      const std::string path = epub->getPath();
      startActivityForResult(
          std::make_unique<EpubReaderChapterSelectionActivity>(renderer, mappedInput, epub, path, spineIdx,
                                                               lastBuildParams_, prebuilder_),
          [this](const ActivityResult& result) {
            if (!result.isCancelled && currentSpineIndex != std::get<ChapterResult>(result.data).spineIndex) {
              RenderLock lock(*this);
              currentSpineIndex = std::get<ChapterResult>(result.data).spineIndex;
              nextPageNumber = 0;
              resetSectionForNavigation();
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::FOOTNOTES: {
      startActivityForResult(std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes),
                             [this](const ActivityResult& result) {
                               if (!result.isCancelled) {
                                 const auto& footnoteResult = std::get<FootnoteResult>(result.data);
                                 navigateToHref(footnoteResult.href, true);
                               }
                               requestUpdate();
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_TO_PERCENT: {
      float bookProgress = 0.0f;
      if (epub && epub->getBookSize() > 0 && section && section->pageCount > 0) {
        const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
        bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
      }
      const int initialPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
      startActivityForResult(
          std::make_unique<EpubReaderPercentSelectionActivity>(renderer, mappedInput, initialPercent),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              jumpToPercent(std::get<PercentResult>(result.data).percent);
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DISPLAY_QR: {
      if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
        auto p = section->loadPageFromSectionFile();
        if (p) {
          std::string fullText;
          for (const auto& el : p->elements) {
            if (el->getTag() == TAG_PageLine) {
              const auto& line = static_cast<const PageLine&>(*el);
              if (line.getBlock()) {
                const auto& words = line.getBlock()->getWords();
                for (const auto& w : words) {
                  if (!fullText.empty()) fullText += " ";
                  fullText += w;
                }
              }
            }
          }
          if (!fullText.empty()) {
            startActivityForResult(std::make_unique<QrDisplayActivity>(renderer, mappedInput, fullText),
                                   [this](const ActivityResult& result) {});
            break;
          }
        }
      }
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_HOME: {
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_CACHE: {
      {
        RenderLock lock(*this);
        if (epub && section) {
          uint16_t backupSpine = currentSpineIndex;
          uint16_t backupPage = section->currentPage;
          uint16_t backupPageCount = section->pageCount;
          resetSectionForNavigation();
          epub->clearCache();
          epub->setupCacheDir();
          if (!saveProgress(backupSpine, backupPage, backupPageCount)) {
            LOG_ERR("ERS", "Failed to save progress before cache clear");
          }
        }
      }
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::SCREENSHOT: {
      {
        RenderLock lock(*this);
        pendingScreenshot = true;
      }
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SYNC: {
      if (KOREADER_STORE.hasCredentials()) {
        const int currentPage = section ? section->currentPage : nextPageNumber;
        const int totalPages = section ? section->pageCount : cachedChapterTotalPageCount;
        std::optional<uint16_t> paragraphIndex;
        if (section && currentPage >= 0 && currentPage < section->pageCount) {
          const uint16_t paragraphPage =
              currentPage > 0 ? static_cast<uint16_t>(currentPage - 1) : static_cast<uint16_t>(currentPage);
          if (const auto pIdx = section->getParagraphIndexForPage(paragraphPage)) {
            paragraphIndex = *pIdx;
          }
        }

        CrossPointPosition localPos = {currentSpineIndex, currentPage, totalPages};
        if (paragraphIndex.has_value()) {
          localPos.paragraphIndex = *paragraphIndex;
          localPos.hasParagraphIndex = true;
        }
        KOReaderPosition localKoPos = ProgressMapper::toKOReader(epub, localPos);
        const int tocIdx = epub->getTocIndexForSpineIndex(currentSpineIndex);
        std::string localChapterName = (tocIdx >= 0) ? epub->getTocItem(tocIdx).title : "";
        const std::string savedEpubPath = epub->getPath();

        if (!saveProgress(currentSpineIndex, currentPage, totalPages)) {
          LOG_ERR("KOSync", "Aborting sync because current progress could not be saved");
          pendingSyncSaveError = true;
          requestUpdate();
          return;
        }

        LOG_DBG("KOSync", "Releasing epub for sync (heap before: %u)", (unsigned)ESP.getFreeHeap());
        {
          RenderLock lock(*this);
          if (section) {
            nextPageNumber = section->currentPage;
          }
          section.reset();
          epub.reset();
        }
        LOG_DBG("KOSync", "Epub released (heap after: %u)", (unsigned)ESP.getFreeHeap());

        activityManager.replaceActivity(std::make_unique<KOReaderSyncActivity>(
            renderer, mappedInput, savedEpubPath, currentSpineIndex, currentPage, totalPages, std::move(localKoPos),
            std::move(localChapterName), paragraphIndex));
      }
      break;
    }
  }
}

void EpubReaderActivity::applyOrientation(const uint8_t orientation) {
  if (SETTINGS.orientation == orientation) {
    return;
  }

  {
    RenderLock lock(*this);
    if (section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }

    SETTINGS.orientation = orientation;
    SETTINGS.saveToFile();
    ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
    resetSectionForNavigation();
  }
}

void EpubReaderActivity::toggleAutoPageTurn(const uint8_t selectedPageTurnOption) {
  if (selectedPageTurnOption == 0 || selectedPageTurnOption >= std::size(PAGE_TURN_RATES)) {
    automaticPageTurnActive = false;
    return;
  }

  lastPageTurnTime = millis();
  pageTurnDuration = (1UL * 60 * 1000) / PAGE_TURN_RATES[selectedPageTurnOption];
  automaticPageTurnActive = true;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
    RenderLock lock(*this);
    if (section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }
    resetSectionForNavigation();
  }
}

void EpubReaderActivity::pageTurn(bool isForwardTurn) {
  if (isForwardTurn) {
    if (section->currentPage < section->pageCount - 1) {
      // Hold the render lock so advancing currentPage and reading preRendered is atomic against
      // the render task's pre-render pass, which writes preRendered under the same lock.
      RenderLock lock;
      const int next = section->currentPage + 1;
      section->currentPage = next;
      // No staged-page check needed here: render()'s fast path fires intrinsically when
      // preRendered.pageIndex == currentPage. Drop the stage only when this is a non-staged
      // forward turn (so the staged buffer for page N+1 doesn't leak into a render of N+2).
      if (!preRendered.ready || preRendered.spineIndex != currentSpineIndex ||
          preRendered.pageIndex != next) {
        invalidatePreRender();
      }
    } else {
      {
        RenderLock lock(*this);
        nextPageNumber = 0;
        currentSpineIndex++;
        resetSectionForNavigation();
      }
    }
  } else {
    if (section->currentPage > 0) {
      // Lock for the same reason as the forward branch: the render task's pre-render pass reads
      // currentPage and owns preRendered under this lock.
      RenderLock lock;
      section->currentPage--;
      invalidatePreRender();
    } else if (currentSpineIndex > 0) {
      {
        RenderLock lock(*this);
        nextPageNumber = 0;
        pendingPageJump = std::numeric_limits<uint16_t>::max();
        currentSpineIndex--;
        resetSectionForNavigation();
      }
    }
  }
  lastPageTurnTime = millis();
  requestUpdate();
}

void EpubReaderActivity::showPendingSyncSaveError() {
  if (!pendingSyncSaveError) return;
  pendingSyncSaveError = false;
  GUI.drawPopup(renderer, tr(STR_SAVE_PROGRESS_FAILED));
}

void EpubReaderActivity::loadSectionForCurrentSpine() {
  invalidatePreRender();  // a freshly loaded section has no valid staged page
  const auto filepath = epub->getSpineItem(currentSpineIndex).href;
  LOG_DBG("ERS", "Loading file: %s, index: %d", filepath.c_str(), currentSpineIndex);
  section = makeUniqueNoThrow<Section>(epub, currentSpineIndex, renderer);
  if (!section) {
    LOG_ERR("ERS", "OOM allocating section for spine %d", currentSpineIndex);
    return;
  }

  prebuilder_.drainIfNeeded(currentSpineIndex, renderer);

  const auto& p = lastBuildParams_;
  const unsigned long loadStart = millis();
  const bool loaded = section->loadSectionFile(p);
  if (!loaded) {
    LOG_DBG("ERS", "Cache not found, building...");
    GUI.drawPopup(renderer, tr(STR_INDEXING));
    const auto popupFn = [this]() { GUI.drawPopup(renderer, tr(STR_INDEXING)); };
    const unsigned long buildStart = millis();
    if (!section->createSectionFile(p, popupFn)) {
      LOG_ERR("ERS", "Failed to persist page data to SD");
      section.reset();
      return;
    }
    LOG_DBG("ERS", "Cache miss spine %d: build=%lums", currentSpineIndex, millis() - buildStart);
  } else {
    LOG_DBG("ERS", "Cache hit spine %d: load=%lums", currentSpineIndex, millis() - loadStart);
  }

  // Resolve the initial currentPage from whichever pending source applies.
  // Order matters: explicit page jump wins over cached resume, then an anchor
  // remaps within the section, then cached-progress reflow, then a percent
  // jump. Each source consumes its pending state on use.
  if (pendingPageJump.has_value()) {
    if (*pendingPageJump >= section->pageCount && section->pageCount > 0) {
      section->currentPage = section->pageCount - 1;
    } else {
      section->currentPage = *pendingPageJump;
    }
    pendingPageJump.reset();
  } else {
    section->currentPage = nextPageNumber;
    if (section->currentPage < 0) {
      section->currentPage = 0;
    } else if (section->currentPage >= section->pageCount && section->pageCount > 0) {
      LOG_DBG("ERS", "Clamping cached page %d to %d", section->currentPage, section->pageCount - 1);
      section->currentPage = section->pageCount - 1;
    }
  }

  if (!pendingAnchor.empty()) {
    if (const auto page = section->getPageForAnchor(pendingAnchor)) {
      section->currentPage = *page;
      LOG_DBG("ERS", "Resolved anchor '%s' to page %d", pendingAnchor.c_str(), *page);
    } else {
      LOG_DBG("ERS", "Anchor '%s' not found in section %d", pendingAnchor.c_str(), currentSpineIndex);
    }
    pendingAnchor.clear();
  }

  if (cachedChapterTotalPageCount > 0) {
    if (currentSpineIndex == cachedSpineIndex && section->pageCount != cachedChapterTotalPageCount) {
      const float progress = static_cast<float>(section->currentPage) / static_cast<float>(cachedChapterTotalPageCount);
      section->currentPage = static_cast<int>(progress * section->pageCount);
    }
    cachedChapterTotalPageCount = 0;
  }

  if (pendingPercentJump && section->pageCount > 0) {
    int newPage = static_cast<int>(pendingSpineProgress * static_cast<float>(section->pageCount));
    if (newPage >= section->pageCount) {
      newPage = section->pageCount - 1;
    }
    section->currentPage = newPage;
    pendingPercentJump = false;
  }
}

void EpubReaderActivity::render(RenderLock&& lock) {
  if (!epub) {
    return;
  }

  if (currentSpineIndex < 0) {
    currentSpineIndex = 0;
  }
  if (currentSpineIndex > epub->getSpineItemsCount()) {
    currentSpineIndex = epub->getSpineItemsCount();
  }

  if (currentSpineIndex == epub->getSpineItemsCount()) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_END_OF_BOOK), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  // Margins/viewport are computed against the live status bar and auto-page-turn state.
  // The TRBL margins (used as paint coordinates) are also needed below for paintPage; keep
  // those local while delegating the SectionBuildParams construction to the shared helper.
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;
  orientedMarginRight += SETTINGS.screenMargin;
  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  if (automaticPageTurnActive &&
      (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight())) {
    orientedMarginBottom +=
        std::max(SETTINGS.screenMargin,
                 static_cast<uint8_t>(statusBarHeight + UITheme::getInstance().getMetrics().statusBarVerticalMargin));
  } else {
    orientedMarginBottom += std::max(SETTINGS.screenMargin, statusBarHeight);
  }
  lastBuildParams_ = EpubReaderUtils::computeBuildParams(renderer, automaticPageTurnActive);

  if (!section) {
    loadSectionForCurrentSpine();
    if (!section) {
      showPendingSyncSaveError();
      return;  // OOM or persistent build failure; bail this frame
    }
  }

  // Fast path: the framebuffer already holds the page we're about to display (forward turn
  // consumed a staged pre-render). Branch before clearScreen() so the staged content survives;
  // paintPage flushes it without re-rastering. The intrinsic state is sufficient -- every
  // non-forward navigation path calls invalidatePreRender(), so a staged page with matching
  // (spine, pageIndex) can only have been produced by the forward path that placed it there.
  const bool fastPath = section && preRendered.ready && preRendered.page &&
                        preRendered.spineIndex == currentSpineIndex &&
                        preRendered.pageIndex == section->currentPage;
  if (fastPath) {
    currentPageFootnotes = std::move(preRendered.page->footnotes);
    const auto start = millis();
    paintPage(*preRendered.page, orientedMarginTop, orientedMarginLeft, /*contentAlreadyInFb=*/true);
    LOG_DBG("ERS", "Displayed pre-rendered page in %dms", millis() - start);
    invalidatePreRender();
    prerenderNextPage(orientedMarginTop, orientedMarginLeft);
  } else {
    renderer.clearScreen();

    if (section->pageCount == 0) {
      LOG_DBG("ERS", "No pages to render");
      renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_CHAPTER), true, EpdFontFamily::BOLD);
      renderStatusBar();
      renderer.displayBuffer();
      automaticPageTurnActive = false;
      showPendingSyncSaveError();
      return;
    }

    if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
      LOG_DBG("ERS", "Page out of bounds: %d (max %d)", section->currentPage, section->pageCount);
      renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_OUT_OF_BOUNDS), true, EpdFontFamily::BOLD);
      renderStatusBar();
      renderer.displayBuffer();
      automaticPageTurnActive = false;
      showPendingSyncSaveError();
      return;
    }

    {
      auto p = section->loadPageFromSectionFile();
      if (!p) {
        LOG_ERR("ERS", "Failed to load page from SD - clearing section cache");
        section->clearCache();
        resetSectionForNavigation();  // cache just wiped; any staged pre-render is stale
        requestUpdate();  // try again after clearing cache
                          // TODO: prevent infinite loop if the page keeps failing to load
        automaticPageTurnActive = false;
        showPendingSyncSaveError();
        return;
      }

      // Collect footnotes from the loaded page
      currentPageFootnotes = std::move(p->footnotes);

      const auto start = millis();
      paintPage(*p, orientedMarginTop, orientedMarginLeft, /*contentAlreadyInFb=*/false);
      LOG_DBG("ERS", "Rendered page in %dms", millis() - start);
      prerenderNextPage(orientedMarginTop, orientedMarginLeft);
    }
  }

  // Target the next chapter for background prebuild. setTargetSpine handles
  // end-of-book (targetSpine == spineItemsCount) by clearing instead of
  // building, so this is safe at any position.
  prebuilder_.setTargetSpine(currentSpineIndex + 1, epub, renderer, lastBuildParams_);

  // One chunk per page turn, after displayBuffer so the build spreads across
  // reading time with no perceived latency. Render-task only (SdFat-safe).
  prebuilder_.step();

  saveProgress(currentSpineIndex, section->currentPage, section->pageCount);
  showPendingSyncSaveError();

  if (pendingScreenshot) {
    pendingScreenshot = false;
    ScreenshotUtil::takeScreenshot(renderer);
  }
}

bool EpubReaderActivity::saveProgress(int spineIndex, int currentPage, int pageCount) {
  return EpubReaderUtils::saveProgress(*epub, spineIndex, currentPage, pageCount);
}

// paintPage / renderPageContentOnly / prerenderNextPage / invalidatePreRender /
// renderStatusBar are defined out-of-line in EpubReaderRender.cpp to keep this
// file under the size threshold; they're still members of EpubReaderActivity.

void EpubReaderActivity::resetSectionForNavigation() {
  section.reset();
  invalidatePreRender();
}

void EpubReaderActivity::navigateToHref(const std::string& hrefStr, const bool savePosition) {
  if (!epub) return;

  if (savePosition && section && footnoteDepth < MAX_FOOTNOTE_DEPTH) {
    savedPositions[footnoteDepth] = {currentSpineIndex, section->currentPage};
    footnoteDepth++;
    LOG_DBG("ERS", "Saved position [%d]: spine %d, page %d", footnoteDepth, currentSpineIndex, section->currentPage);
  }

  std::string anchor;
  const auto hashPos = hrefStr.find('#');
  if (hashPos != std::string::npos && hashPos + 1 < hrefStr.size()) {
    anchor = hrefStr.substr(hashPos + 1);
  }

  bool sameFile = !hrefStr.empty() && hrefStr[0] == '#';

  int targetSpineIndex;
  if (sameFile) {
    targetSpineIndex = currentSpineIndex;
  } else {
    targetSpineIndex = epub->resolveHrefToSpineIndex(hrefStr);
  }

  if (targetSpineIndex < 0) {
    LOG_DBG("ERS", "Could not resolve href: %s", hrefStr.c_str());
    if (savePosition && footnoteDepth > 0) footnoteDepth--;
    return;
  }

  {
    RenderLock lock(*this);
    pendingAnchor = std::move(anchor);
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    resetSectionForNavigation();
  }
  requestUpdate();
  LOG_DBG("ERS", "Navigated to spine %d for href: %s", targetSpineIndex, hrefStr.c_str());
}

void EpubReaderActivity::restoreSavedPosition() {
  if (footnoteDepth <= 0) return;
  footnoteDepth--;
  const auto& pos = savedPositions[footnoteDepth];
  LOG_DBG("ERS", "Restoring position [%d]: spine %d, page %d", footnoteDepth, pos.spineIndex, pos.pageNumber);

  {
    RenderLock lock(*this);
    currentSpineIndex = pos.spineIndex;
    nextPageNumber = pos.pageNumber;
    resetSectionForNavigation();
  }
  requestUpdate();
}

ScreenshotInfo EpubReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Epub;
  if (epub) {
    snprintf(info.title, sizeof(info.title), "%s", epub->getTitle().c_str());
    info.spineIndex = currentSpineIndex;
  }
  if (section) {
    info.currentPage = section->currentPage + 1;
    info.totalPages = section->pageCount;
    if (epub && epub->getBookSize() > 0 && section->pageCount > 0) {
      const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
      int pct = static_cast<int>(epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f + 0.5f);
      if (pct < 0) pct = 0;
      if (pct > 100) pct = 100;
      info.progressPercent = pct;
    }
  }
  return info;
}
