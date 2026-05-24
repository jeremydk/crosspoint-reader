#include "BookOpenPrebuilder.h"

#include <Epub.h>
#include <Epub/Section.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>

#include <memory>

#include "CrossPointSettings.h"
#include "activities/reader/EpubReaderUtils.h"

namespace {
// Translate SETTINGS.orientation (uint8 enum) to the renderer's Orientation
// enum. Returns nullopt for unknown values so the caller can skip prebuild.
struct ExpectedOrientation {
  bool valid;
  GfxRenderer::Orientation value;
};
ExpectedOrientation expectedRendererOrientation() {
  switch (SETTINGS.orientation) {
    case CrossPointSettings::PORTRAIT:
      return {true, GfxRenderer::Orientation::Portrait};
    case CrossPointSettings::LANDSCAPE_CW:
      return {true, GfxRenderer::Orientation::LandscapeClockwise};
    case CrossPointSettings::INVERTED:
      return {true, GfxRenderer::Orientation::PortraitInverted};
    case CrossPointSettings::LANDSCAPE_CCW:
      return {true, GfxRenderer::Orientation::LandscapeCounterClockwise};
    default:
      return {false, GfxRenderer::Orientation::Portrait};
  }
}

// Read the resume spine index from the book's progress.bin. Returns 0 if no
// progress is recorded, the file is truncated, or the value is out of range
// for this epub's spine count -- in all those cases spine 0 is the right
// fallback (it's where the reader's render() ends up too).
//
// progress.bin format mirrors EpubReaderActivity::onEnter's reader and
// EpubReaderUtils::saveProgress's writer: 6 bytes little-endian
// [spineLo, spineHi, pageLo, pageHi, countLo, countHi].
int resumeSpineFromProgress(const Epub& epub) {
  FsFile f;
  if (!Storage.openFileForRead("BOP", epub.getCachePath() + "/progress.bin", f)) return 0;
  uint8_t data[6];
  if (f.read(data, sizeof(data)) != sizeof(data)) return 0;
  const int spine = data[0] + (data[1] << 8);
  if (spine < 0 || spine >= epub.getSpineItemsCount()) return 0;
  return spine;
}
}  // namespace

void BookOpenPrebuilder::attach(GfxRenderer& renderer) {
  renderer_ = &renderer;
  hoveredPath_.clear();
  actedFor_ = false;
}

void BookOpenPrebuilder::detach() {
  renderer_ = nullptr;
  hoveredPath_.clear();
  actedFor_ = false;
}

void BookOpenPrebuilder::noteHover(const std::string& bookPath) {
  if (!renderer_) return;
  if (bookPath != hoveredPath_) {
    hoveredPath_ = bookPath;
    hoverChangedMs_ = millis();
    actedFor_ = false;
    return;
  }
  if (actedFor_) return;
  if (bookPath.empty()) return;
  if (millis() - hoverChangedMs_ < SETTLE_MS) return;
  actedFor_ = true;  // attempted; reset to false in tryPrebuild on transient failure
  tryPrebuild(bookPath);
}

void BookOpenPrebuilder::tryPrebuild(const std::string& bookPath) {
  if (!FsHelpers::hasEpubExtension(bookPath)) return;  // only EPUBs have a Section indexing step

  const auto expected = expectedRendererOrientation();
  if (!expected.valid || renderer_->getOrientation() != expected.value) {
    // Reader would use a different viewport; our cache key would mismatch.
    return;
  }

  // Transient Epub for the prebuild only. For recent books, book.bin is on SD
  // so load is ~50 ms; for cold-cache books load can be 500 ms+ -- that risk
  // belongs to FileBrowser integration (not in v1 scope).
  auto epub = std::shared_ptr<Epub>(new Epub(bookPath, "/.crosspoint"));
  if (!epub->load(/*buildIfMissing=*/true, /*skipLoadingCss=*/SETTINGS.embeddedStyle == 0)) {
    LOG_ERR("BOP", "Epub load failed for %s", bookPath.c_str());
    return;
  }
  if (epub->getSpineItemsCount() == 0) return;

  // Prebuild the spine the reader will actually resume at, not spine 0. For
  // recent books that's almost always somewhere mid-book (where the user left
  // off). For brand-new books with no progress yet this falls back to spine 0.
  const int resumeSpine = resumeSpineFromProgress(*epub);

  const auto params = EpubReaderUtils::computeBuildParams(*renderer_, /*autoPageTurnBottomMargin=*/false);
  Section target(epub, resumeSpine, *renderer_);
  if (target.loadSectionFile(params)) {
    LOG_DBG("BOP", "Section %d already cached for %s", resumeSpine, bookPath.c_str());
    return;
  }
  LOG_DBG("BOP", "Hover-prebuilding section %d for %s", resumeSpine, bookPath.c_str());
  if (!target.createSectionFile(params)) {
    LOG_ERR("BOP", "Section %d prebuild failed for %s", resumeSpine, bookPath.c_str());
    actedFor_ = false;  // transient failure: let a later settle retry
  }
}
