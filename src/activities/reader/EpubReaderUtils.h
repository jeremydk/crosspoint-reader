#pragma once

#include <CrossPointSettings.h>
#include <Epub.h>
#include <Epub/Section.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>

#include "components/UITheme.h"

namespace EpubReaderUtils {

// Compute the Section cache key from current SETTINGS + renderer state.
// Caller must have the renderer at the reader's intended orientation already
// (the reader applies it in onEnter; BookOpenPrebuilder skips if the renderer
// orientation doesn't match SETTINGS.orientation, so its cache key matches the
// reader's).
//
// `autoPageTurnBottomMargin` is true only when the reader is rendering with
// auto-page-turn active AND the bar slot is small enough that the indicator
// claims its own slot. Always false for a fresh-open prebuild.
inline SectionBuildParams computeBuildParams(GfxRenderer& renderer, bool autoPageTurnBottomMargin) {
  int top, right, bottom, left;
  renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
  top += SETTINGS.screenMargin;
  left += SETTINGS.screenMargin;
  right += SETTINGS.screenMargin;
  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  if (autoPageTurnBottomMargin &&
      (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight())) {
    bottom += std::max(SETTINGS.screenMargin,
                       static_cast<uint8_t>(statusBarHeight +
                                            UITheme::getInstance().getMetrics().statusBarVerticalMargin));
  } else {
    bottom += std::max(SETTINGS.screenMargin, statusBarHeight);
  }
  return {
      SETTINGS.getReaderFontId(),
      SETTINGS.getReaderLineCompression(),
      SETTINGS.extraParagraphSpacing,
      SETTINGS.paragraphAlignment,
      static_cast<uint16_t>(renderer.getScreenWidth() - left - right),
      static_cast<uint16_t>(renderer.getScreenHeight() - top - bottom),
      SETTINGS.hyphenationEnabled,
      SETTINGS.embeddedStyle,
      SETTINGS.imageRendering,
      SETTINGS.focusReadingEnabled,
  };
}

// Persists reader progress for an EPUB to its cache directory. Returns true on success.
inline bool saveProgress(Epub& epub, int spineIndex, int pageNumber, int pageCount) {
  if (spineIndex < 0 || spineIndex > 0xFFFF || pageNumber < 0 || pageNumber > 0xFFFF || pageCount < 0 ||
      pageCount > 0xFFFF) {
    LOG_ERR("ERS", "Progress values out of range: spine=%d page=%d count=%d", spineIndex, pageNumber, pageCount);
    return false;
  }
  FsFile f;
  if (!Storage.openFileForWrite("ERS", epub.getCachePath() + "/progress.bin", f)) {
    LOG_ERR("ERS", "Could not open progress file for write!");
    return false;
  }
  uint8_t data[6];
  data[0] = spineIndex & 0xFF;
  data[1] = (spineIndex >> 8) & 0xFF;
  data[2] = pageNumber & 0xFF;
  data[3] = (pageNumber >> 8) & 0xFF;
  data[4] = pageCount & 0xFF;
  data[5] = (pageCount >> 8) & 0xFF;
  const size_t written = f.write(data, sizeof(data));
  if (written != sizeof(data)) {
    LOG_ERR("ERS", "Short write saving progress: %u/%u bytes", (unsigned)written, (unsigned)sizeof(data));
    return false;
  }
  LOG_DBG("ERS", "Progress saved: spine=%d page=%d", spineIndex, pageNumber);
  return true;
}

}  // namespace EpubReaderUtils
