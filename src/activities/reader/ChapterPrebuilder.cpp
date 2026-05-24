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
  evaluatedTarget_ = -1;
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

void ChapterPrebuilder::step() {
  if (!inFlightSection_) return;
  const unsigned long t0 = millis();
  const auto res = inFlightSection_->stepBuild(1);
  LOG_DBG("ERS", "Per-page step spine %d: %lums%s", inFlightSpine_, millis() - t0,
          res == Section::StepResult::InProgress ? "" : " [done]");
  if (res != Section::StepResult::InProgress) {
    inFlightSection_.reset();
    inFlightSpine_ = -1;
  }
}

void ChapterPrebuilder::setTargetSpine(int targetSpine, const std::shared_ptr<Epub>& epub,
                                       GfxRenderer& renderer, const SectionBuildParams& p) {
  if (!epub) return;
  if (evaluatedTarget_ == targetSpine) return;  // sticky idempotency

  // Target changed (or first call): drop any in-flight build for the prior target.
  if (inFlightSpine_ != targetSpine) {
    inFlightSection_.reset();
    inFlightSpine_ = -1;
  }

  // Treat <0 or out-of-range as "clear" (e.g., reader at end-of-book passes
  // currentSpineIndex+1 which can equal spineCount).
  if (targetSpine < 0 || targetSpine >= epub->getSpineItemsCount()) {
    evaluatedTarget_ = targetSpine;
    return;
  }

  // Already building the new target -- let it run.
  if (inFlightSection_) {
    evaluatedTarget_ = targetSpine;
    return;
  }

  auto candidate = makeUniqueNoThrow<Section>(epub, targetSpine, renderer);
  if (!candidate) {
    LOG_ERR("ERS", "OOM allocating prebuild section for spine %d", targetSpine);
    return;  // leave evaluatedTarget_ untouched so caller can retry
  }
  if (candidate->loadSectionFile(p)) {
    evaluatedTarget_ = targetSpine;
    return;  // already cached on SD, no build needed
  }

  LOG_DBG("ERS", "Spawning chunked prebuild for spine %d", targetSpine);
  if (!candidate->startBuild(p)) {
    LOG_ERR("ERS", "Failed to start chunked prebuild for spine %d", targetSpine);
    return;  // leave evaluatedTarget_ untouched so caller can retry
  }
  inFlightSection_ = std::move(candidate);
  inFlightSpine_ = targetSpine;
  evaluatedTarget_ = targetSpine;
}
