#pragma once
#include <string>

class GfxRenderer;

// Synchronous hover-prebuild for book-open. Held by activities that list
// books (HomeActivity, RecentBooksActivity). The activity calls noteHover()
// every loop tick with the currently-highlighted book path (or empty when
// no book is hovered, e.g., menu item is selected). When the same path
// settles for SETTLE_MS, this loads the Epub, reads progress.bin to find
// the spine the reader will resume at, and builds that section's cache
// inline on the activity loop -- same pattern as
// EpubReaderChapterSelectionActivity's hover prebuild.
//
// Building the resume spine rather than spine 0 matters: recent books are
// almost always mid-book (progress.bin holds e.g. spine=30 page=5), so a
// spine-0 prebuild would do no work the reader actually needs.
//
// Sync rather than idle-hook-amortized because:
//   - For recent books (v1 only target), book.bin is cached so Epub::load
//     is ~50 ms. Section 0 is also typically cached, making the whole
//     sequence a quick cache-validation no-op (~100 ms).
//   - Blocking the loop briefly is the same trade-off the chapter-selection
//     hover prebuild already makes: the user has settled, we'd be blocking
//     them at book-open otherwise.
//
// Skips entirely when the renderer's current orientation doesn't match
// SETTINGS.orientation -- the prebuild's cache key would not match what the
// reader produces, so we'd waste SD I/O. Rotated-mode users still pay the
// indexing cost at book-open like before; not worse than today.
class BookOpenPrebuilder {
 public:
  void attach(GfxRenderer& renderer);
  void detach();

  // Notify of the currently-highlighted book path each loop tick. Pass empty
  // when nothing is hovered (menu item selected, list empty, etc.).
  void noteHover(const std::string& bookPath);

 private:
  static constexpr unsigned long SETTLE_MS = 500;

  GfxRenderer* renderer_ = nullptr;
  std::string hoveredPath_;
  unsigned long hoverChangedMs_ = 0;
  bool actedFor_ = false;  // suppresses repeated work for the same hover

  void tryPrebuild(const std::string& bookPath);
};
