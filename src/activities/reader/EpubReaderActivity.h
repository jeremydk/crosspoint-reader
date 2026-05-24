#pragma once
#include <Epub.h>
#include <Epub/FootnoteEntry.h>
#include <Epub/Page.h>
#include <Epub/Section.h>

#include <memory>
#include <optional>

#include "ChapterPrebuilder.h"
#include "EpubReaderMenuActivity.h"
#include "activities/Activity.h"

class EpubReaderActivity final : public Activity {
  std::shared_ptr<Epub> epub;
  std::unique_ptr<Section> section = nullptr;
  int currentSpineIndex = 0;
  int nextPageNumber = 0;
  std::optional<uint16_t> pendingPageJump;
  std::string pendingAnchor;
  int pagesUntilFullRefresh = 0;
  int cachedSpineIndex = 0;
  int cachedChapterTotalPageCount = 0;
  ChapterPrebuilder prebuilder_;
  // Viewport + settings snapshot captured each render so subactivities
  // (chapter selection hover-prebuild) use cache keys matching the reader.
  SectionBuildParams lastBuildParams_ = {};
  unsigned long lastPageTurnTime = 0UL;
  unsigned long pageTurnDuration = 0UL;
  bool pendingPercentJump = false;
  float pendingSpineProgress = 0.0f;
  bool pendingScreenshot = false;
  bool pendingSyncSaveError = false;
  bool skipNextButtonCheck = false;
  bool automaticPageTurnActive = false;
  bool recentsEntryRemoved = false;
  bool pendingReadFolderMove = false;

  std::vector<FootnoteEntry> currentPageFootnotes;
  struct SavedPosition {
    int spineIndex;
    int pageNumber;
  };
  static constexpr int MAX_FOOTNOTE_DEPTH = 3;
  SavedPosition savedPositions[MAX_FOOTNOTE_DEPTH] = {};
  int footnoteDepth = 0;

  // Next page staged into the framebuffer at the tail of a render, layout cached here. A forward
  // turn then skips the SD load and BW raster and goes straight to flush. Touched only under
  // RenderLock: prerenderNextPage() writes it, pageTurn() reads it.
  struct PreRenderedPage {
    bool ready = false;
    int spineIndex = -1;
    int pageIndex = -1;
    std::unique_ptr<Page> page;  // reused by the consume path's grayscale pass
  };
  PreRenderedPage preRendered;

  // Paint one page to the panel: optional prewarm + BW raster, status bar, flush, grayscale pass.
  // contentAlreadyInFb skips the BW raster (the framebuffer already holds it, a consumed
  // pre-render); the prewarm still runs under AA because the gray pass re-renders the glyphs.
  void paintPage(Page& page, int orientedMarginTop, int orientedMarginLeft, bool contentAlreadyInFb);
  // Rasterize a page's BW content into the framebuffer (prewarm + render), no status bar, no
  // flush. Used by prerenderNextPage to stage the next page while the reader is idle.
  void renderPageContentOnly(Page& page, int orientedMarginTop, int orientedMarginLeft);
  // Stage the next text page into the framebuffer so the next forward turn is a flush only. No-op
  // when there is no next page, it has images, heap headroom is low, or it is already staged.
  void prerenderNextPage(int orientedMarginTop, int orientedMarginLeft);
  // Drop any staged pre-render. Call on every navigation that is not a simple forward turn.
  void invalidatePreRender();
  // Reset section state for any navigation that changes spine, viewport, or
  // cache contents. Always invalidates the staged pre-render -- it was layout
  // for the old (spine, viewport) tuple and is stale by construction. Caller
  // must hold a RenderLock; both writes are protected by it.
  void resetSectionForNavigation();
  // Allocate `section` for currentSpineIndex, draining any in-flight prebuild
  // for this spine, loading from cache or building (with INDEXING popup), then
  // resolving the initial currentPage from any of the pending-jump sources
  // (pageJump / anchor / cached chapter progress / percent jump). On OOM or
  // build failure, leaves `section` null; caller should bail.
  void loadSectionForCurrentSpine();
  // Surface the deferred save-progress error popup (and clear the flag) the
  // first time render reaches an exit point after the failure. Idempotent.
  void showPendingSyncSaveError();
  void renderStatusBar() const;

  bool saveProgress(int spineIndex, int currentPage, int pageCount);
  void jumpToPercent(int percent);
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
  void applyOrientation(uint8_t orientation);
  void toggleAutoPageTurn(uint8_t selectedPageTurnOption);
  void pageTurn(bool isForwardTurn);
  void navigateToHref(const std::string& href, bool savePosition = false);
  void restoreSavedPosition();

 public:
  explicit EpubReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Epub> epub)
      : Activity("EpubReader", renderer, mappedInput), epub(std::move(epub)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  bool isReaderActivity() const override { return true; }
  ScreenshotInfo getScreenshotInfo() const override;
};
