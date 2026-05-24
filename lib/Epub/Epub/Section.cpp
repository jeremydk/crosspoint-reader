#include "Section.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

#include "Epub/css/CssParser.h"
#include "Page.h"
#include "hyphenation/Hyphenator.h"
#include "parsers/ChapterHtmlSlimParser.h"

namespace {
constexpr uint8_t SECTION_FILE_VERSION = 23;
constexpr uint32_t HEADER_SIZE = sizeof(uint8_t) + sizeof(int) + sizeof(float) + sizeof(bool) + sizeof(uint8_t) +
                                 sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint16_t) + sizeof(bool) + sizeof(bool) +
                                 sizeof(uint8_t) + sizeof(bool) + sizeof(uint32_t) + sizeof(uint32_t) +
                                 sizeof(uint32_t) + sizeof(uint32_t);

struct PageLutEntry {
  uint32_t fileOffset;
  uint16_t paragraphIndex;
  uint16_t listItemIndex;
};
}  // namespace

uint32_t Section::onPageComplete(std::unique_ptr<Page> page) {
  if (!file) {
    LOG_ERR("SCT", "File not open for writing page %d", pageCount);
    return 0;
  }

  const uint32_t position = file.position();
  if (!page->serialize(file)) {
    LOG_ERR("SCT", "Failed to serialize page %d", pageCount);
    return 0;
  }
  LOG_DBG("SCT", "Page %d processed", pageCount);

  pageCount++;
  return position;
}

void Section::writeSectionFileHeader(const SectionBuildParams& p) {
  if (!file) {
    LOG_DBG("SCT", "File not open for writing header");
    return;
  }
  static_assert(HEADER_SIZE == sizeof(SECTION_FILE_VERSION) + sizeof(p.fontId) + sizeof(p.lineCompression) +
                                   sizeof(p.extraParagraphSpacing) + sizeof(p.paragraphAlignment) +
                                   sizeof(p.viewportWidth) + sizeof(p.viewportHeight) + sizeof(pageCount) +
                                   sizeof(p.hyphenationEnabled) + sizeof(p.embeddedStyle) + sizeof(p.imageRendering) +
                                   sizeof(p.focusReadingEnabled) + sizeof(uint32_t) + sizeof(uint32_t) +
                                   sizeof(uint32_t) + sizeof(uint32_t),
                "Header size mismatch");
  serialization::writePod(file, SECTION_FILE_VERSION);
  serialization::writePod(file, p.fontId);
  serialization::writePod(file, p.lineCompression);
  serialization::writePod(file, p.extraParagraphSpacing);
  serialization::writePod(file, p.paragraphAlignment);
  serialization::writePod(file, p.viewportWidth);
  serialization::writePod(file, p.viewportHeight);
  serialization::writePod(file, p.hyphenationEnabled);
  serialization::writePod(file, p.embeddedStyle);
  serialization::writePod(file, p.imageRendering);
  serialization::writePod(file, p.focusReadingEnabled);
  // Placeholders patched by finalizeBuild: pageCount, LUT offset, anchor-map
  // offset, paragraph-LUT offset, li-LUT offset.
  serialization::writePod(file, pageCount);
  serialization::writePod(file, static_cast<uint32_t>(0));
  serialization::writePod(file, static_cast<uint32_t>(0));
  serialization::writePod(file, static_cast<uint32_t>(0));
  serialization::writePod(file, static_cast<uint32_t>(0));
}

bool Section::loadSectionFile(const SectionBuildParams& p) {
  if (!Storage.openFileForRead("SCT", filePath, file)) {
    return false;
  }

  uint8_t version;
  serialization::readPod(file, version);
  if (version != SECTION_FILE_VERSION) {
    file.close();  // member FsFile -- explicit close before clearCache reopen
    LOG_ERR("SCT", "Deserialization failed: Unknown version %u", version);
    clearCache();
    return false;
  }

  SectionBuildParams fp;
  serialization::readPod(file, fp.fontId);
  serialization::readPod(file, fp.lineCompression);
  serialization::readPod(file, fp.extraParagraphSpacing);
  serialization::readPod(file, fp.paragraphAlignment);
  serialization::readPod(file, fp.viewportWidth);
  serialization::readPod(file, fp.viewportHeight);
  serialization::readPod(file, fp.hyphenationEnabled);
  serialization::readPod(file, fp.embeddedStyle);
  serialization::readPod(file, fp.imageRendering);
  serialization::readPod(file, fp.focusReadingEnabled);

  if (p.fontId != fp.fontId || p.lineCompression != fp.lineCompression ||
      p.extraParagraphSpacing != fp.extraParagraphSpacing || p.paragraphAlignment != fp.paragraphAlignment ||
      p.viewportWidth != fp.viewportWidth || p.viewportHeight != fp.viewportHeight ||
      p.hyphenationEnabled != fp.hyphenationEnabled || p.embeddedStyle != fp.embeddedStyle ||
      p.imageRendering != fp.imageRendering || p.focusReadingEnabled != fp.focusReadingEnabled) {
    file.close();
    LOG_ERR("SCT", "Deserialization failed: Parameters do not match");
    clearCache();
    return false;
  }

  serialization::readPod(file, pageCount);
  file.close();
  LOG_DBG("SCT", "Deserialization succeeded: %d pages", pageCount);
  return true;
}

bool Section::clearCache() const {
  if (!Storage.exists(filePath.c_str())) {
    LOG_DBG("SCT", "Cache does not exist, no action needed");
    return true;
  }

  if (!Storage.remove(filePath.c_str())) {
    LOG_ERR("SCT", "Failed to clear cache");
    return false;
  }

  LOG_DBG("SCT", "Cache cleared successfully");
  return true;
}

// Heap-resident state for a resumable build. Owns the visitor (and through
// it, the expat parser handle and all in-progress layout state), the LUT
// that the visitor's per-page callback appends to, the tmp HTML path so
// finalize/cleanup can remove it, and a borrowed cssParser pointer so
// finalize can clear() it on completion or error.
struct Section::Build {
  std::string tmpHtmlPath;
  uint32_t tmpFileSize = 0;  // captured from the stream loop so the popup gate doesn't re-probe SD
  std::vector<PageLutEntry> lut;
  std::unique_ptr<ChapterHtmlSlimParser> visitor;
  CssParser* cssParser = nullptr;
};

Section::Section(const std::shared_ptr<Epub>& epub, const int spineIndex, GfxRenderer& renderer)
    : epub(epub),
      spineIndex(spineIndex),
      renderer(renderer),
      filePath(epub->getCachePath() + "/sections/" + std::to_string(spineIndex) + ".bin") {}

Section::~Section() { abortBuild(); }

void Section::abortBuild() {
  // Idempotent teardown of the in-flight build state: close `file`, drop the
  // partial section cache file (so it can't masquerade as valid on next load),
  // drop the tmp HTML, clear the borrowed CSS state. Called from every error
  // path in startBuild/finalizeBuild and from the dtor.
  if (!build_) return;
  if (file.isOpen()) file.close();
  Storage.remove(filePath.c_str());
  if (!build_->tmpHtmlPath.empty()) {
    Storage.remove(build_->tmpHtmlPath.c_str());
  }
  if (build_->cssParser) build_->cssParser->clear();
  build_.reset();
}

bool Section::createSectionFile(const SectionBuildParams& p, const std::function<void()>& popupFn) {
  // Sync wrapper: start the build, surface the INDEXING popup if the HTML is
  // large, then drain to completion. The drain loop captures the final
  // StepResult so a parse/IO failure inside finalizeBuild() is propagated;
  // build_ resets on every terminal path, so `!build_` alone would lose it.
  if (!startBuild(p)) return false;
  if (popupFn && build_->tmpFileSize >= 10 * 1024) {
    popupFn();
  }
  StepResult res = StepResult::InProgress;
  while (res == StepResult::InProgress) {
    res = stepBuild(UINT32_MAX);
  }
  return res == StepResult::Done;
}

bool Section::startBuild(const SectionBuildParams& p) {
  if (build_) {
    LOG_ERR("SCT", "startBuild called while another build is in flight for spine %d", spineIndex);
    return false;
  }
  const auto localPath = epub->getSpineItem(spineIndex).href;
  build_ = std::make_unique<Build>();
  build_->tmpHtmlPath = epub->getCachePath() + "/.tmp_" + std::to_string(spineIndex) + ".html";

  {
    const auto sectionsDir = epub->getCachePath() + "/sections";
    Storage.mkdir(sectionsDir.c_str());
  }

  // Retry-with-fresh-open dance for SD timing flakes (preserved verbatim).
  bool success = false;
  for (int attempt = 0; attempt < 3 && !success; attempt++) {
    if (attempt > 0) {
      LOG_DBG("SCT", "Retrying stream (attempt %d)...", attempt + 1);
      delay(50);
    }
    if (Storage.exists(build_->tmpHtmlPath.c_str())) Storage.remove(build_->tmpHtmlPath.c_str());
    FsFile tmpHtml;
    if (!Storage.openFileForWrite("SCT", build_->tmpHtmlPath, tmpHtml)) continue;
    success = epub->readItemContentsToStream(localPath, tmpHtml, 1024);
    build_->tmpFileSize = tmpHtml.size();
    tmpHtml.close();
    if (!success && Storage.exists(build_->tmpHtmlPath.c_str())) {
      Storage.remove(build_->tmpHtmlPath.c_str());
    }
  }
  if (!success) {
    LOG_ERR("SCT", "Failed to stream item contents to temp file after retries");
    abortBuild();
    return false;
  }
  LOG_DBG("SCT", "Streamed temp HTML to %s (%u bytes)", build_->tmpHtmlPath.c_str(),
          (unsigned)build_->tmpFileSize);

  if (!Storage.openFileForWrite("SCT", filePath, file)) {
    abortBuild();
    return false;
  }
  writeSectionFileHeader(p);

  size_t lastSlash = localPath.find_last_of('/');
  std::string contentBase = (lastSlash != std::string::npos) ? localPath.substr(0, lastSlash + 1) : "";
  std::string imageBasePath = epub->getCachePath() + "/img_" + std::to_string(spineIndex) + "_";

  if (p.embeddedStyle) {
    build_->cssParser = epub->getCssParser();
    if (build_->cssParser && !build_->cssParser->loadFromCache()) {
      LOG_ERR("SCT", "Failed to load CSS from cache");
    }
  }

  build_->visitor = std::make_unique<ChapterHtmlSlimParser>(
      epub, build_->tmpHtmlPath, renderer, p.fontId, p.lineCompression, p.extraParagraphSpacing, p.paragraphAlignment,
      p.viewportWidth, p.viewportHeight, p.hyphenationEnabled, p.focusReadingEnabled,
      // Capture `this` so the page-complete callback can route through Section
      // (onPageComplete writes the serialized page to `file`, returns the
      // offset, which we append to Build::lut for the LUT pass in finalize).
      [this](std::unique_ptr<Page> page, const uint16_t paragraphIndex, const uint16_t listItemIndex) {
        if (!build_) return;
        build_->lut.push_back({this->onPageComplete(std::move(page)), paragraphIndex, listItemIndex});
      },
      p.embeddedStyle, contentBase, imageBasePath, p.imageRendering, /*popupFn=*/nullptr, build_->cssParser);
  Hyphenator::setPreferredLanguage(epub->getLanguage());

  if (!build_->visitor->parseBegin()) {
    LOG_ERR("SCT", "Failed to initialize parser for spine %d", spineIndex);
    abortBuild();
    return false;
  }
  return true;
}

Section::StepResult Section::stepBuild(uint32_t maxChunks) {
  if (!build_) return StepResult::Failed;
  if (build_->visitor->parseStep(maxChunks)) return StepResult::InProgress;
  // Parser is terminal -- either done or errored. finalizeBuild() inspects
  // parseFinalize()'s result and decides which side won. Either way, build_
  // is null after this returns.
  return finalizeBuild() ? StepResult::Done : StepResult::Failed;
}

bool Section::finalizeBuild() {
  if (!build_) return false;

  if (!build_->visitor->parseFinalize()) {
    LOG_ERR("SCT", "Failed to parse XML and build pages for spine %d", spineIndex);
    abortBuild();
    return false;
  }

  // Write LUT, anchors, paragraph + list-item lookups, then patch the header
  // with the final pageCount + offset table.
  const uint32_t lutOffset = file.position();
  bool hasFailedLutRecords = false;
  for (const auto& entry : build_->lut) {
    if (entry.fileOffset == 0) {
      hasFailedLutRecords = true;
      break;
    }
    serialization::writePod(file, entry.fileOffset);
  }
  if (hasFailedLutRecords) {
    LOG_ERR("SCT", "Failed to write LUT due to invalid page positions");
    abortBuild();
    return false;
  }

  const uint32_t anchorMapOffset = file.position();
  const auto& anchors = build_->visitor->getAnchors();
  serialization::writePod(file, static_cast<uint16_t>(anchors.size()));
  for (const auto& [anchor, page] : anchors) {
    serialization::writeString(file, anchor);
    serialization::writePod(file, page);
  }

  const uint32_t paragraphLutOffset = file.position();
  serialization::writePod(file, static_cast<uint16_t>(build_->lut.size()));
  for (const auto& entry : build_->lut) {
    serialization::writePod(file, entry.paragraphIndex);
  }

  const uint32_t liLutFileOffset = static_cast<uint32_t>(file.position());
  for (const auto& entry : build_->lut) {
    serialization::writePod(file, entry.listItemIndex);
  }

  file.seek(HEADER_SIZE - sizeof(uint32_t) * 4 - sizeof(pageCount));
  serialization::writePod(file, pageCount);
  serialization::writePod(file, lutOffset);
  serialization::writePod(file, anchorMapOffset);
  serialization::writePod(file, paragraphLutOffset);
  serialization::writePod(file, liLutFileOffset);
  file.close();  // member FsFile -- explicit close before next reopen

  Storage.remove(build_->tmpHtmlPath.c_str());  // success: drop tmp HTML (error paths handled by abortBuild)
  if (build_->cssParser) build_->cssParser->clear();
  build_.reset();
  return true;
}

std::unique_ptr<Page> Section::loadPageFromSectionFile() {
  if (!Storage.openFileForRead("SCT", filePath, file)) {
    return nullptr;
  }

  file.seek(HEADER_SIZE - sizeof(uint32_t) * 4);
  uint32_t lutOffset;
  serialization::readPod(file, lutOffset);
  file.seek(lutOffset + sizeof(uint32_t) * currentPage);
  uint32_t pagePos;
  serialization::readPod(file, pagePos);
  file.seek(pagePos);

  auto page = Page::deserialize(file);
  // Explicit close() required: member variable persists beyond function scope
  file.close();
  return page;
}

std::optional<uint16_t> Section::getPageForAnchor(const std::string& anchor) const {
  FsFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  f.seek(HEADER_SIZE - sizeof(uint32_t) * 3);
  uint32_t anchorMapOffset;
  serialization::readPod(f, anchorMapOffset);
  if (anchorMapOffset == 0 || anchorMapOffset >= fileSize) {
    return std::nullopt;
  }

  f.seek(anchorMapOffset);
  uint16_t count;
  serialization::readPod(f, count);
  for (uint16_t i = 0; i < count; i++) {
    std::string key;
    uint16_t page;
    serialization::readString(f, key);
    serialization::readPod(f, page);
    if (key == anchor) {
      return page;
    }
  }

  return std::nullopt;
}

std::optional<uint16_t> Section::getPageForParagraphIndex(const uint16_t pIndex) const {
  FsFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  f.seek(HEADER_SIZE - sizeof(uint32_t) * 2);
  uint32_t paragraphLutOffset;
  serialization::readPod(f, paragraphLutOffset);
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  f.seek(paragraphLutOffset);
  uint16_t count;
  serialization::readPod(f, count);
  if (count == 0) {
    return std::nullopt;
  }

  const uint32_t lutEnd = paragraphLutOffset + sizeof(uint16_t) + count * sizeof(uint16_t);
  if (lutEnd > fileSize) {
    return std::nullopt;
  }

  uint16_t resultPage = count - 1;
  for (uint16_t i = 0; i < count; i++) {
    uint16_t pagePIdx;
    serialization::readPod(f, pagePIdx);
    if (pagePIdx >= pIndex) {
      resultPage = i;
      break;
    }
  }

  return resultPage;
}

std::optional<uint16_t> Section::getParagraphIndexForPage(const uint16_t page) const {
  FsFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  f.seek(HEADER_SIZE - sizeof(uint32_t) * 2);
  uint32_t paragraphLutOffset;
  serialization::readPod(f, paragraphLutOffset);
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  f.seek(paragraphLutOffset);
  uint16_t count;
  serialization::readPod(f, count);
  if (count == 0 || page >= count) {
    return std::nullopt;
  }

  const uint32_t entryEnd = paragraphLutOffset + sizeof(uint16_t) + (page + 1) * sizeof(uint16_t);
  if (entryEnd > fileSize) {
    return std::nullopt;
  }

  f.seek(paragraphLutOffset + sizeof(uint16_t) + page * sizeof(uint16_t));
  uint16_t pIdx;
  serialization::readPod(f, pIdx);
  return pIdx;
}

std::optional<uint16_t> Section::getPageForListItemIndex(const uint16_t liIndex) const {
  FsFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  f.seek(HEADER_SIZE - sizeof(uint32_t));
  uint32_t liLutOffset;
  serialization::readPod(f, liLutOffset);
  if (liLutOffset == 0 || liLutOffset >= fileSize) {
    return std::nullopt;
  }

  // The li LUT shares count with the paragraph LUT; read count from paragraphLutOffset
  f.seek(HEADER_SIZE - sizeof(uint32_t) * 2);
  uint32_t paragraphLutOffset;
  serialization::readPod(f, paragraphLutOffset);
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  f.seek(paragraphLutOffset);
  uint16_t count;
  serialization::readPod(f, count);
  if (count == 0) {
    return std::nullopt;
  }

  const uint32_t lutEnd = liLutOffset + count * sizeof(uint16_t);
  if (lutEnd > fileSize) {
    return std::nullopt;
  }

  f.seek(liLutOffset);
  uint16_t resultPage = count - 1;
  for (uint16_t i = 0; i < count; i++) {
    uint16_t pageLiIdx;
    serialization::readPod(f, pageLiIdx);
    if (pageLiIdx >= liIndex) {
      resultPage = i;
      break;
    }
  }

  return resultPage;
}
