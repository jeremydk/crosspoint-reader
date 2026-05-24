#pragma once
#include <Epub.h>
#include <Epub/Section.h>

#include <memory>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class EpubReaderChapterSelectionActivity final : public Activity {
  std::shared_ptr<Epub> epub;
  std::string epubPath;
  ButtonNavigator buttonNavigator;
  int currentSpineIndex = 0;
  int selectorIndex = 0;

  // Full build-params snapshot from the reader at the moment chapter selection
  // opened. Used to build a cache that the reader will load on confirm.
  SectionBuildParams buildParams = {};

  // Hover-prebuild state.
  int lastSelectorIndex = -1;
  unsigned long lastSelectorChangeMs = 0;
  int prebuiltSpineIndex = -1;
  bool prebuildAttempted = false;
  void maybePrebuildHoveredChapter();
  static constexpr unsigned long PREBUILD_SETTLE_MS = 500;

  int getPageItems() const;
  int getTotalItems() const;

 public:
  explicit EpubReaderChapterSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                              const std::shared_ptr<Epub>& epub, const std::string& epubPath,
                                              const int currentSpineIndex, const SectionBuildParams& buildParams)
      : Activity("EpubReaderChapterSelection", renderer, mappedInput),
        epub(epub),
        epubPath(epubPath),
        currentSpineIndex(currentSpineIndex),
        buildParams(buildParams) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
