#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Epub.h"

class Page;
class GfxRenderer;
class ChapterHtmlSlimParser;

// The 10 fields that fully identify a rendered section cache entry. Section
// uses these as the cache key and writes them into the file header; callers
// pass the same snapshot through load / create / startBuild so the cache is
// hit only when every field matches the one that produced it.
struct SectionBuildParams {
  int fontId;
  float lineCompression;
  bool extraParagraphSpacing;
  uint8_t paragraphAlignment;
  uint16_t viewportWidth;
  uint16_t viewportHeight;
  bool hyphenationEnabled;
  bool embeddedStyle;
  uint8_t imageRendering;
  bool focusReadingEnabled;
};

class Section {
  std::shared_ptr<Epub> epub;
  const int spineIndex;
  GfxRenderer& renderer;
  std::string filePath;
  FsFile file;

  // Per-build state for the resumable createSectionFile path. Lives on the
  // heap, owned across startBuild / stepBuild / finalizeBuild calls so the
  // reader can drive a build a chunk at a time interleaved with renders.
  // Null when no build is in flight.
  struct Build;
  std::unique_ptr<Build> build_;
  bool finalizeBuild();
  // Tear down an in-flight build: close `file`, remove the partial section
  // cache + tmp HTML, clear borrowed CSS state, drop `build_`. Idempotent and
  // safe at any error path or during destruction.
  void abortBuild();

  void writeSectionFileHeader(const SectionBuildParams& p);
  uint32_t onPageComplete(std::unique_ptr<Page> page);

 public:
  // Step result for the resumable build. `InProgress` means call again;
  // `Done` and `Failed` are both terminal (the build state is torn down).
  // The two terminals are kept distinct so the synchronous wrapper can
  // surface parse / IO failures instead of silently treating them as success.
  enum class StepResult { InProgress, Done, Failed };

  uint16_t pageCount = 0;
  int currentPage = 0;

  // Defined out-of-line in Section.cpp because the inline default-init of
  // `build_` (std::unique_ptr<Build>) needs the deleter complete, and Build
  // is intentionally forward-declared here to keep the visitor + LUT types
  // out of Section's public header.
  explicit Section(const std::shared_ptr<Epub>& epub, int spineIndex, GfxRenderer& renderer);
  ~Section();

  bool loadSectionFile(const SectionBuildParams& p);
  bool clearCache() const;
  // Synchronous build: equivalent to startBuild + stepBuild-to-completion.
  // Surfaces the INDEXING popup if the temp HTML exceeds the popup threshold.
  bool createSectionFile(const SectionBuildParams& p, const std::function<void()>& popupFn = nullptr);

  // Resumable build. Caller pattern:
  //   if (!s.startBuild(p)) return error;
  //   for (;;) {
  //     auto r = s.stepBuild(N);                // yield between calls
  //     if (r == StepResult::InProgress) continue;
  //     if (r == StepResult::Failed) handle_error();
  //     break;                                  // Done or Failed: terminal
  //   }
  // `maxChunks` caps PARSE_BUFFER_SIZE iterations per call (each ~1KB of
  // HTML). No popupFn here: the resumable caller runs the build as background
  // work and never wants a foreground popup.
  bool startBuild(const SectionBuildParams& p);
  StepResult stepBuild(uint32_t maxChunks);
  bool buildIsActive() const { return build_ != nullptr; }

  std::unique_ptr<Page> loadPageFromSectionFile();

  // Look up the page number for an anchor id from the section cache file.
  std::optional<uint16_t> getPageForAnchor(const std::string& anchor) const;

  // Look up the page number for a synthetic paragraph index from XPath p[N].
  std::optional<uint16_t> getPageForParagraphIndex(uint16_t pIndex) const;

  // Look up the page number for a running list-item index from the li LUT.
  std::optional<uint16_t> getPageForListItemIndex(uint16_t liIndex) const;

  // Look up the synthetic paragraph index for the given rendered page.
  std::optional<uint16_t> getParagraphIndexForPage(uint16_t page) const;
};
