#include "EpubReaderChapterSelectionActivity.h"

#include <Epub/Section.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

int EpubReaderChapterSelectionActivity::getTotalItems() const { return epub->getTocItemsCount(); }

void EpubReaderChapterSelectionActivity::onEnter() {
  Activity::onEnter();

  if (!epub) {
    return;
  }

  selectorIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
  if (selectorIndex == -1) {
    selectorIndex = 0;
  }

  requestUpdate();
}

void EpubReaderChapterSelectionActivity::onExit() { Activity::onExit(); }

void EpubReaderChapterSelectionActivity::loop() {
  const int pageItems = UITheme::getInstance().getNumberOfItemsPerPage(renderer, true, false, true, false);
  const int totalItems = getTotalItems();

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const auto newSpineIndex = epub->getSpineIndexForTocIndex(selectorIndex);
    if (newSpineIndex == -1) {
      ActivityResult result;
      result.isCancelled = true;
      setResult(std::move(result));
      finish();
    } else {
      setResult(ChapterResult{newSpineIndex});
      finish();
    }
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
  }

  buttonNavigator.onNextRelease([this, totalItems] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, totalItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this, totalItems] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, totalItems);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, totalItems, pageItems] {
    selectorIndex = ButtonNavigator::nextPageIndex(selectorIndex, totalItems, pageItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, totalItems, pageItems] {
    selectorIndex = ButtonNavigator::previousPageIndex(selectorIndex, totalItems, pageItems);
    requestUpdate();
  });

  maybePrebuildHoveredChapter();
}

void EpubReaderChapterSelectionActivity::maybePrebuildHoveredChapter() {
  if (!epub || buildParams.viewportWidth == 0 || buildParams.viewportHeight == 0) {
    return;
  }

  if (selectorIndex != lastSelectorIndex) {
    lastSelectorIndex = selectorIndex;
    lastSelectorChangeMs = millis();
    prebuildAttempted = false;
    return;
  }

  if (prebuildAttempted) return;
  if (millis() - lastSelectorChangeMs < PREBUILD_SETTLE_MS) return;

  const int targetSpine = epub->getSpineIndexForTocIndex(selectorIndex);
  if (targetSpine < 0 || targetSpine == currentSpineIndex || targetSpine == prebuiltSpineIndex) {
    prebuildAttempted = true;
    return;
  }

  // Already-cached and freshly-built paths both leave the section cache ready,
  // so the reader's load on confirm becomes a fast HIT instead of a foreground
  // BUILD with the INDEXING popup. createSectionFile blocks this activity's
  // loop for the duration of the build; that's fine -- the user has settled on
  // this entry and we'd be blocking them in the reader otherwise.
  Section target(epub, targetSpine, renderer);
  if (target.loadSectionFile(buildParams)) {
    prebuiltSpineIndex = targetSpine;
    prebuildAttempted = true;
    return;
  }

  LOG_DBG("ECS", "Hover-prebuilding spine %d", targetSpine);
  if (!target.createSectionFile(buildParams)) {
    LOG_ERR("ECS", "Failed hover prebuild for spine %d", targetSpine);
    return;  // leave attempted=false so a later settle retries
  }
  prebuiltSpineIndex = targetSpine;
  prebuildAttempted = true;
}

void EpubReaderChapterSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  auto metrics = UITheme::getInstance().getMetrics();
  Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_SELECT_CHAPTER));

  const int contentTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = screen.height - contentTop - metrics.verticalSpacing;

  const int totalItems = getTotalItems();
  GUI.drawList(renderer, Rect{screen.x, contentTop, screen.width, contentHeight}, totalItems, selectorIndex,
               [this](int index) {
                 auto item = epub->getTocItem(index);
                 std::string indent((item.level - 1) * 2, ' ');
                 return indent + item.title;
               });

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
