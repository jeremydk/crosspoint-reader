// Per-page rendering for EpubReaderActivity: BW raster + status bar + flush,
// optional grayscale pass (tiled strip path on supporting controllers, full-
// frame fallback otherwise), and the pre-render-next-page staging that backs
// the fast-display path. Split out of EpubReaderActivity.cpp to keep that
// file under the size threshold; these are still members of EpubReaderActivity
// so they retain private state access via out-of-line definition.

#include "EpubReaderActivity.h"

#include <Epub/Page.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_system.h>

#include <optional>

#include "CrossPointSettings.h"
#include "ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"

void EpubReaderActivity::paintPage(Page& page, const int orientedMarginTop, const int orientedMarginLeft,
                                   const bool contentAlreadyInFb) {
  const auto t0 = millis();

  // Prewarm before the BW raster, which needs decoded glyphs. The fast path has no raster, so it
  // defers prewarm until after the flush (below): the glyphs are only needed by the grayscale
  // pass, which runs once the page is already visible. The perceived turn is the flush, so prewarm
  // stays off the pre-display path.
  auto* fcm = renderer.getFontCacheManager();
  std::optional<FontCacheManager::PrewarmScope> scope;
  const auto prewarm = [&]() {
    scope.emplace(fcm->createPrewarmScope());
    page.render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);  // scan pass
    scope->endScanAndPrewarm();
  };
  if (!contentAlreadyInFb) {
    prewarm();
  }
  const auto tPrewarm = millis();

  // Force special handling for pages with images when anti-aliasing is on. Staged pages are
  // text-only (image pages are excluded from pre-rendering), so this only fires on the normal path.
  bool imagePageWithAA = page.hasImages() && SETTINGS.textAntiAliasing;

  if (!contentAlreadyInFb) {
    page.render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
  }
  renderStatusBar();
  const auto tBwRender = millis();

  if (imagePageWithAA) {
    // Double FAST_REFRESH with selective image blanking (pablohc's technique):
    // HALF_REFRESH sets particles too firmly for the grayscale LUT to adjust.
    int16_t imgX, imgY, imgW, imgH;
    if (page.getImageBoundingBox(imgX, imgY, imgW, imgH)) {
      renderer.fillRect(imgX + orientedMarginLeft, imgY + orientedMarginTop, imgW, imgH, false);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);

      // Re-render page content to restore images into the blanked area
      // Status bar is not re-rendered here to avoid reading stale dynamic values (e.g. battery %)
      page.render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    } else {
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
  } else {
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
  }
  const auto tDisplay = millis();

  // Fast path: prewarm now, after the flush, so the grayscale pass below has its glyphs. The page
  // is already on screen; this work no longer delays the perceived turn.
  if (contentAlreadyInFb && SETTINGS.textAntiAliasing) {
    prewarm();
  }

  // Tiled grayscale: render each plane band-by-band into a small scratch and
  // stream straight to the controller, leaving the BW framebuffer intact so no
  // full-frame storeBwBuffer is needed; controller RAM is re-synced from the
  // live framebuffer afterward. The page is re-rendered ceil(H/STRIP_ROWS) times
  // per plane, but renderCharImpl culls out-of-band glyphs before decode so the
  // cost stays close to one render. Both text (drawPixel) and images
  // (DirectPixelWriter) honor the active strip target.
  if (SETTINGS.textAntiAliasing && renderer.supportsStripGrayscale()) {
    constexpr int STRIP_ROWS = 80;
    const int gh = renderer.getDisplayHeight();
    const int gwBytes = renderer.getDisplayWidthBytes();

    auto scratch = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(gwBytes) * STRIP_ROWS);
    if (!scratch) {
      LOG_ERR("ERS", "OOM: grayscale strip scratch (%d bytes); skipping AA this page", gwBytes * STRIP_ROWS);
    } else {
      // Bands may be streamed in any order: X4 windows each via setRamArea, X3
      // via PTL.
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
      for (int y = 0; y < gh; y += STRIP_ROWS) {
        const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
        renderer.beginStripTarget(scratch.get(), y, rows);
        renderer.clearScreen(0x00);
        page.render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
        renderer.endStripTarget();
        renderer.writeGrayscalePlaneStrip(true, scratch.get(), y, rows);
      }
      const auto tGrayLsb = millis();

      // MSB plane.
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
      for (int y = 0; y < gh; y += STRIP_ROWS) {
        const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
        renderer.beginStripTarget(scratch.get(), y, rows);
        renderer.clearScreen(0x00);
        page.render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
        renderer.endStripTarget();
        renderer.writeGrayscalePlaneStrip(false, scratch.get(), y, rows);
      }
      const auto tGrayMsb = millis();

      renderer.setRenderMode(GfxRenderer::BW);
      renderer.displayGrayBuffer();
      const auto tGrayDisplay = millis();

      // BW framebuffer is intact; re-sync controller RAM for the next
      // differential page turn directly from it.
      renderer.cleanupGrayscaleWithFrameBuffer();
      const auto tCleanup = millis();

      const auto tEnd = millis();
      LOG_DBG("ERS",
              "Page render (tiled): prewarm=%lums bw_render=%lums display=%lums gray_lsb=%lums "
              "gray_msb=%lums gray_display=%lums cleanup=%lums total=%lums",
              tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tGrayLsb - tDisplay, tGrayMsb - tGrayLsb,
              tGrayDisplay - tGrayMsb, tCleanup - tGrayDisplay, tEnd - t0);
    }
  } else {
    // Fallback path for a controller without strip support. grayscale rendering
    // TODO: Only do this if font supports it
    if (SETTINGS.textAntiAliasing) {
      // Save the BW frame before the grayscale passes overwrite it, restore
      // after. Only needed when grayscale actually renders.
      renderer.storeBwBuffer();
      const auto tBwStore = millis();

      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
      page.render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
      renderer.copyGrayscaleLsbBuffers();
      const auto tGrayLsb = millis();

      // Render and copy to MSB buffer
      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
      page.render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
      renderer.copyGrayscaleMsbBuffers();
      const auto tGrayMsb = millis();

      // display grayscale part
      renderer.displayGrayBuffer();
      const auto tGrayDisplay = millis();
      renderer.setRenderMode(GfxRenderer::BW);
      renderer.restoreBwBuffer();
      const auto tBwRestore = millis();

      const auto tEnd = millis();
      LOG_DBG("ERS",
              "Page render: prewarm=%lums bw_render=%lums display=%lums bw_store=%lums "
              "gray_lsb=%lums gray_msb=%lums gray_display=%lums bw_restore=%lums total=%lums",
              tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tBwStore - tDisplay, tGrayLsb - tBwStore,
              tGrayMsb - tGrayLsb, tGrayDisplay - tGrayMsb, tBwRestore - tGrayDisplay, tEnd - t0);
    } else {
      // No anti-aliasing: BW frame already displayed above, no grayscale to
      // render, so no save/restore.
      const auto tEnd = millis();
      LOG_DBG("ERS", "Page render: prewarm=%lums bw_render=%lums display=%lums total=%lums", tPrewarm - t0,
              tBwRender - tPrewarm, tDisplay - tBwRender, tEnd - t0);
    }
  }
}

void EpubReaderActivity::renderPageContentOnly(Page& page, const int orientedMarginTop, const int orientedMarginLeft) {
  // Stage BW content into the framebuffer: prewarm then real render. No status bar (superimposed
  // with live values at display time), no flush. The scope frees its glyph page buffer on return;
  // the consume path re-prewarms for its grayscale pass.
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  page.render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);  // scan pass
  scope.endScanAndPrewarm();
  renderer.clearScreen();
  page.render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
}

void EpubReaderActivity::prerenderNextPage(const int orientedMarginTop, const int orientedMarginLeft) {
  // Conservative gates; tune after measuring [FDC] pageBuf= and the held Page on device. A staged
  // page holds its layout (~2-5 KB) plus a transient prewarm during the raster; never trade a page
  // turn for an OOM.
  static constexpr uint32_t PRERENDER_MIN_FREE_HEAP_BYTES = 60000;
  static constexpr uint32_t PRERENDER_MIN_CONTIG_HEAP_BYTES = 40000;

  if (!section) return;
  const int next = section->currentPage + 1;
  if (next >= section->pageCount) return;  // no next page in this section
  if (preRendered.ready && preRendered.spineIndex == currentSpineIndex && preRendered.pageIndex == next) {
    return;  // already staged
  }

  if (ESP.getFreeHeap() < PRERENDER_MIN_FREE_HEAP_BYTES || ESP.getMaxAllocHeap() < PRERENDER_MIN_CONTIG_HEAP_BYTES) {
    invalidatePreRender();
    return;
  }

  const int saved = section->currentPage;
  section->currentPage = next;
  auto p = section->loadPageFromSectionFile();
  section->currentPage = saved;
  if (!p || p->hasImages()) {
    // Image pages take the normal (non-staged) path so the image-blanking refresh runs correctly.
    invalidatePreRender();
    return;
  }

  renderPageContentOnly(*p, orientedMarginTop, orientedMarginLeft);  // framebuffer now holds page `next`
  preRendered.ready = true;
  preRendered.spineIndex = currentSpineIndex;
  preRendered.pageIndex = next;
  preRendered.page = std::move(p);
  LOG_DBG("ERS", "Pre-rendered page %d/%d", next, section->pageCount - 1);
}

void EpubReaderActivity::invalidatePreRender() {
  preRendered.ready = false;
  preRendered.spineIndex = -1;
  preRendered.pageIndex = -1;
  preRendered.page.reset();
}

void EpubReaderActivity::renderStatusBar() const {
  const int currentPage = section->currentPage + 1;
  const float pageCount = section->pageCount;
  const float sectionChapterProg = (pageCount > 0) ? (static_cast<float>(currentPage) / pageCount) : 0;
  const float bookProgress = epub->calculateProgress(currentSpineIndex, sectionChapterProg) * 100;

  std::string title;
  int textYOffset = 0;

  if (automaticPageTurnActive) {
    title = tr(STR_AUTO_TURN_ENABLED) + std::to_string(60 * 1000 / pageTurnDuration);

    const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
    if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
      textYOffset += UITheme::getInstance().getMetrics().statusBarVerticalMargin;
    }

  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    title = tr(STR_UNNAMED);
    const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
    if (tocIndex != -1) {
      title = epub->getTocItem(tocIndex).title;
    }

  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE) {
    title = epub->getTitle();
  }

  GUI.drawStatusBar(renderer, bookProgress, currentPage, pageCount, title, 0, textYOffset);
}
