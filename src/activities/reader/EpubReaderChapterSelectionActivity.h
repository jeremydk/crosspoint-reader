#pragma once
#include <Epub.h>
#include <Epub/Section.h>

#include <memory>

#include "ChapterPrebuilder.h"
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

  // Borrowed reference to the reader's ChapterPrebuilder. On hover-settle we
  // call setTargetSpine() on this shared instance, which drops the reader's
  // speculative spine+1 build and re-targets to the hovered chapter. The
  // build chunks across the idle hook (already attached by the reader). On
  // confirm we drainIfNeeded() so the reader's load lands as a cache hit.
  //
  // Crucially we do NOT own a separate ChapterPrebuilder: two prebuilders
  // running concurrent Section builds on the render task corrupt SdFat
  // state (each holds Section::file open across step boundaries while the
  // other's parser opens tmpHtml; the cluster cache rotation invalidates
  // the held handle, then the next write through it stores into a bogus
  // address and panics).
  ChapterPrebuilder& prebuilder;

  int lastSelectorIndex = -1;
  unsigned long lastSelectorChangeMs = 0;
  void maybePrebuildHoveredChapter();
  static constexpr unsigned long PREBUILD_SETTLE_MS = 500;

  int getPageItems() const;
  int getTotalItems() const;

 public:
  explicit EpubReaderChapterSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                              const std::shared_ptr<Epub>& epub, const std::string& epubPath,
                                              const int currentSpineIndex, const SectionBuildParams& buildParams,
                                              ChapterPrebuilder& prebuilder)
      : Activity("EpubReaderChapterSelection", renderer, mappedInput),
        epub(epub),
        epubPath(epubPath),
        currentSpineIndex(currentSpineIndex),
        buildParams(buildParams),
        prebuilder(prebuilder) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
