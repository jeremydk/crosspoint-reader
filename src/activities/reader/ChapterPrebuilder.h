#pragma once
#include <Epub.h>
#include <Epub/Section.h>
#include <memory>

#include "util/IdleDispatcher.h"

class GfxRenderer;

// Owns a background chunked Section build, amortized across e-ink BUSY-poll
// idle windows via IdleDispatcher. Lives inside EpubReaderActivity, which
// owns its lifecycle; the chapter-select sub-activity takes a reference and
// retargets it instead of holding its own instance (two prebuilders cannot
// share a render-task safely -- each holds Section::file open across step
// boundaries while the other's parser opens tmpHtml, and the resulting
// SdFat cluster-cache rotation invalidates the held handle).
//
// Threading contract: public methods touch SdFat state and the in-flight
// Section -- both are also reached by the idle hook (onTick) on the render
// task. Callers MUST be on the render task OR hold a RenderLock. The reader
// satisfies this naturally (everything runs in render()); chapter-select
// must wrap setTargetSpine() and drainIfNeeded() in RenderLock because its
// loop() runs on the main task.
class ChapterPrebuilder {
 public:
  void attach();
  void detach();
  void reset();

  // Drain any in-flight build (blocking) if it matches `spine`. Used by the
  // reader before loading a spine on chapter-cross, and by chapter-select on
  // confirm, so the load that follows is a cache hit. Shows INDEXING popup
  // during the wait. No-op when no build is in flight or its spine differs.
  void drainIfNeeded(int spine, GfxRenderer& renderer);

  // Advance the in-flight build by one chunk. The reader calls this once per
  // page turn so the build progresses even between idle ticks. Chapter-select
  // doesn't call it; its only amortization is the idle hook.
  void step();

  // Set the desired prebuild target. Idempotent for the same target (repeated
  // calls are O(1) no-ops). When the target changes, drops any prior in-flight
  // build and starts a new one (or no-ops if the new target is already cached
  // on SD, or out of range, or epub is null). Pass <0 or >=spineCount to clear.
  void setTargetSpine(int targetSpine, const std::shared_ptr<Epub>& epub,
                      GfxRenderer& renderer, const SectionBuildParams& params);

 private:
  std::unique_ptr<Section> inFlightSection_;
  int inFlightSpine_ = -1;
  // Last target setTargetSpine() was called with and acted on (cached hit,
  // started build, or recorded out-of-range). Repeated calls with the same
  // value are no-ops -- the seed evaluation is sticky until the target moves.
  // Failures (OOM, startBuild error) leave this unchanged so a later call
  // retries.
  int evaluatedTarget_ = -1;
  IdleDispatcher::Handle idleHandle_ = IdleDispatcher::INVALID_HANDLE;

  static void hookTrampoline(void* ctx);
  void onTick();
};
