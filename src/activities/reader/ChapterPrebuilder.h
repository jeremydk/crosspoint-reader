#pragma once
#include <Epub.h>
#include <Epub/Section.h>
#include <memory>

#include "util/IdleDispatcher.h"

class GfxRenderer;

// Owns the background chunked prebuild for the next chapter.
//
// One instance lives inside EpubReaderActivity. All public methods run on
// the render task -- SdFat's shared volume state must not be touched from
// two tasks at once.
class ChapterPrebuilder {
 public:
  void attach();
  void detach();
  void reset();

  // If a build is in flight for currentSpineIndex, drain it to completion
  // (blocking) and emit the PERF drain log. Shows INDEXING popup during drain.
  void drainIfNeeded(int currentSpineIndex, GfxRenderer& renderer);

  // Advance the in-flight build by one chunk (post-display, per page turn).
  void step(int currentSpineIndex);

  // Seed a resumable background build for spine currentSpineIndex+1
  // if it isn't already cached. No-op at end-of-book.
  void seedIfNeeded(int currentSpineIndex, const std::shared_ptr<Epub>& epub,
                    GfxRenderer& renderer, const SectionBuildParams& params);

 private:
  std::unique_ptr<Section> inFlightSection_;
  int inFlightSpine_ = -1;
  int seededForSpine_ = -1;
  IdleDispatcher::Handle idleHandle_ = IdleDispatcher::INVALID_HANDLE;

  static void hookTrampoline(void* ctx);
  void onTick();
};
