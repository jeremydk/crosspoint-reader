#include "ChapterPrebuilder.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include "components/UITheme.h"

void ChapterPrebuilder::attach() {
  idleHandle_ = IdleDispatcher::instance().add(&ChapterPrebuilder::hookTrampoline, this);
}

void ChapterPrebuilder::detach() {
  IdleDispatcher::instance().remove(idleHandle_);
  idleHandle_ = IdleDispatcher::INVALID_HANDLE;
  reset();
}

void ChapterPrebuilder::reset() {
  inFlightSection_.reset();
  inFlightSpine_ = -1;
  seededForSpine_ = -1;
}

void ChapterPrebuilder::hookTrampoline(void* ctx) {
  if (ctx) static_cast<ChapterPrebuilder*>(ctx)->onTick();
}

void ChapterPrebuilder::onTick() {
  if (!inFlightSection_) return;
  // One chunk per tick. Each chunk is ~50-150 ms of layout work; the BUSY-poll
  // fires at 1 ms cadence so multiple chunks slot in during the refresh window.
  // Done and Failed are both terminal here -- a failed prebuild just means
  // the foreground load will see a cache miss and rebuild with the popup.
  if (inFlightSection_->stepBuild(1) != Section::StepResult::InProgress) {
    LOG_DBG("ERS", "Chunked prebuild for spine %d complete", inFlightSpine_);
    inFlightSection_.reset();
    inFlightSpine_ = -1;
  }
}

void ChapterPrebuilder::drainIfNeeded(int currentSpineIndex, GfxRenderer& renderer) {
  if (!inFlightSection_ || inFlightSpine_ != currentSpineIndex) return;
  LOG_DBG("ERS", "Draining in-flight prebuild for spine %d", currentSpineIndex);
  GUI.drawPopup(renderer, tr(STR_INDEXING));
  // drawPopup triggers a refresh; the idle hook may complete the build during
  // that BUSY-poll window. Re-check before entering the drain loop.
  const unsigned long t0 = millis();
  while (inFlightSection_ && inFlightSection_->stepBuild(UINT32_MAX) == Section::StepResult::InProgress) {}
  inFlightSection_.reset();
  inFlightSpine_ = -1;
  LOG_DBG("ERS", "Drain of spine %d took %lums", currentSpineIndex, millis() - t0);
}

void ChapterPrebuilder::step(int currentSpineIndex) {
  if (!inFlightSection_ || inFlightSpine_ == currentSpineIndex) return;
  const unsigned long t0 = millis();
  const auto res = inFlightSection_->stepBuild(1);
  LOG_DBG("ERS", "Per-page step spine %d: %lums%s", inFlightSpine_, millis() - t0,
          res == Section::StepResult::InProgress ? "" : " [done]");
  if (res != Section::StepResult::InProgress) {
    inFlightSection_.reset();
    inFlightSpine_ = -1;
  }
}

void ChapterPrebuilder::seedIfNeeded(int currentSpineIndex, const std::shared_ptr<Epub>& epub,
                                     GfxRenderer& renderer, const SectionBuildParams& p) {
  if (!epub) return;
  if (seededForSpine_ == currentSpineIndex) return;

  const int nextSpine = currentSpineIndex + 1;
  if (nextSpine >= epub->getSpineItemsCount()) {
    seededForSpine_ = currentSpineIndex;
    return;
  }

  if (inFlightSection_ && inFlightSpine_ != nextSpine) {
    inFlightSection_.reset();
    inFlightSpine_ = -1;
  }
  if (inFlightSection_) {
    // Already building this spine; let it run.
    seededForSpine_ = currentSpineIndex;
    return;
  }

  auto candidate = makeUniqueNoThrow<Section>(epub, nextSpine, renderer);
  if (!candidate) {
    LOG_ERR("ERS", "OOM allocating prebuild section for spine %d", nextSpine);
    return;
  }
  if (candidate->loadSectionFile(p)) {
    seededForSpine_ = currentSpineIndex;
    return;  // already cached
  }

  LOG_DBG("ERS", "Spawning chunked prebuild for spine %d", nextSpine);
  if (!candidate->startBuild(p)) {
    LOG_ERR("ERS", "Failed to start chunked prebuild for spine %d", nextSpine);
    return;
  }
  inFlightSection_ = std::move(candidate);
  inFlightSpine_ = nextSpine;
  seededForSpine_ = currentSpineIndex;
}
