#include "PeerSyncActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cmath>

#include "Epub/Section.h"
#include "EpubReaderUtils.h"
#include "KOReaderDocumentId.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "activities/ActivityManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr float SYNCED_EPSILON = 0.005f;  // within 0.5% counts as already in sync

void formatMacShort(const uint8_t mac[6], char* out, size_t n) {
  snprintf(out, n, "%02X:%02X:%02X", mac[3], mac[4], mac[5]);
}

// Cross-device book id from title+author, not file bytes: two people each have their
// own copy of the same book, so a content hash (KOReaderDocumentId) won't match. Spaces
// and case are normalized out so trivial metadata differences still pair. FNV-1a with two
// seeds fills the 32-hex docHash field; deterministic across devices on the same binary.
std::string bookIdFromMeta(const std::string& title, const std::string& author) {
  std::string s;
  s.reserve(title.size() + author.size() + 1);
  const auto appendNorm = [&](const std::string& in) {
    for (unsigned char c : in) {
      if (c == ' ') continue;
      s.push_back(static_cast<char>(tolower(c)));
    }
  };
  appendNorm(title);
  s.push_back('\x1f');
  appendNorm(author);

  const auto fnv = [&](uint64_t h) {
    for (unsigned char c : s) {
      h ^= c;
      h *= 1099511628211ULL;
    }
    return h;
  };
  const uint64_t a = fnv(1469598103934665603ULL);
  const uint64_t b = fnv(1099511628211ULL);
  char buf[33];
  snprintf(buf, sizeof(buf), "%016llx%016llx", static_cast<unsigned long long>(a), static_cast<unsigned long long>(b));
  return buf;
}
}  // namespace

void PeerSyncActivity::ensureEpubLoaded() {
  if (epub) return;
  epub = std::make_shared<Epub>(epubPath, "/.crosspoint");
  epub->setupCacheDir();
  if (!epub->load(false, true)) {  // metadata only, don't rebuild cache
    LOG_ERR("PSYNC", "Failed to load epub for mapping");
    epub.reset();
  }
}

std::vector<const PeerSync::Peer*> PeerSyncActivity::sameBookPeers() const {
  std::vector<const PeerSync::Peer*> out;
  for (const auto& p : peerSync.peers()) {
    if (docHash == p.docHash) out.push_back(&p);
  }
  return out;
}

uint32_t PeerSyncActivity::listSignature() const {
  // Cheap hash: peer count, MAC tails, and percentage buckets. Changes when the
  // visible list changes so we only trigger an e-ink refresh on real updates.
  uint32_t sig = 0;
  for (const auto& p : peerSync.peers()) {
    if (docHash != p.docHash) continue;
    sig = sig * 31 + p.mac[5] + (p.mac[4] << 8);
    sig = sig * 31 + static_cast<uint32_t>(p.percentage * 200.0f);
  }
  sig = sig * 31 + static_cast<uint32_t>(peerSync.peers().size());
  return sig;
}

void PeerSyncActivity::onEnter() {
  Activity::onEnter();
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  // Match on title+author, not file bytes: paired readers each have their own copy.
  docHash = bookIdFromMeta(localBookTitle, localBookAuthor);
  LOG_INF("PSYNC", "local docHash=%s pct=%.3f title='%s' author='%s'", docHash.c_str(), localProgress.percentage,
          localBookTitle.c_str(), localBookAuthor.c_str());

  if (!peerSync.begin()) {
    LOG_ERR("PSYNC", "begin failed; returning to reader");
    returnToReader();
    return;
  }

#ifdef DEV_PEERSYNC_FAKE_PEER
  // Single-device testing only: a synthetic same-book peer so the list + conflict path
  // are exercisable without a second radio. Off for real two-device testing (noise).
  peerSync.devInjectPeer(docHash.c_str(), localProgress.percentage > 0.5f ? 0.10f : 0.80f,
                         localBookTitle.c_str(), "/body/DocFragment[3]/body/p[42]/text().0");
#endif

  requestUpdate(true);
}

void PeerSyncActivity::onExit() {
  Activity::onExit();
  peerSync.end();
  // No silent reboot: ESP-NOW brought up no TLS/lwIP sockets to fragment the heap.
  // The free-heap log in PeerSync::end() is the check; restore silentRestartToReader
  // here (as KOReaderSyncActivity does) only if a regression leaks heap on exit.
  LOG_DBG("PSYNC", "onExit heap=%u", (unsigned)ESP.getFreeHeap());
}

void PeerSyncActivity::selectPeerAndMap(const PeerSync::Peer& peer) {
  // Snapshot before mapping; the peer table can churn while we load the Epub.
  remotePercentage = peer.percentage;
  remoteTitle = peer.title;
  formatMacShort(peer.mac, remoteMacStr, sizeof(remoteMacStr));
  const KOReaderPosition koPos = {peer.xpath, peer.percentage};

  ensureEpubLoaded();
  if (!epub) {
    returnToReader();
    return;
  }

  remotePosition = ProgressMapper::toCrossPoint(epub, koPos, currentSpineIndex, totalPagesInSpine);

  // Refine page using section cache LUTs: li index, anchor, or paragraph index.
  // (Mirrors KOReaderSyncActivity::performSync.)
  if (remotePosition.hasLiIndex || remotePosition.xpathAnchorId[0] != '\0' || remotePosition.hasParagraphIndex) {
    Section tempSection(epub, remotePosition.spineIndex, renderer);
    bool refined = false;
    if (remotePosition.hasLiIndex) {
      const auto liPage = tempSection.getPageForListItemIndex(remotePosition.liIndex);
      if (liPage.has_value()) {
        remotePosition.pageNumber = *liPage;
        refined = true;
      }
    }
    if (!refined && remotePosition.xpathAnchorId[0] != '\0') {
      const auto anchorPage = tempSection.getPageForAnchor(std::string(remotePosition.xpathAnchorId));
      if (anchorPage.has_value()) {
        remotePosition.pageNumber = *anchorPage;
        refined = true;
      }
    }
    if (!refined && remotePosition.hasParagraphIndex) {
      const auto paragraphPage = tempSection.getPageForParagraphIndex(remotePosition.paragraphIndex);
      const auto nextParagraphPage = tempSection.getPageForParagraphIndex(remotePosition.paragraphIndex + 1);
      if (paragraphPage.has_value()) {
        int refinedPage = std::max(remotePosition.pageNumber, static_cast<int>(*paragraphPage));
        if (nextParagraphPage.has_value()) {
          const int lutSpan = static_cast<int>(*nextParagraphPage) - static_cast<int>(*paragraphPage);
          if (lutSpan > 1 && refinedPage >= static_cast<int>(*nextParagraphPage)) {
            refinedPage = static_cast<int>(*nextParagraphPage) - 1;
          }
        }
        remotePosition.pageNumber = refinedPage;
      }
    }
  }

  alreadySynced = std::fabs(localProgress.percentage - remotePercentage) < SYNCED_EPSILON;
  conflictOption = (localProgress.percentage > remotePercentage) ? 1 : 0;  // furthest-progress default

  {
    RenderLock lock(*this);
    state = SHOWING_RESULT;
  }
  requestUpdate(true);
}

void PeerSyncActivity::applyRemoteAndReturn() {
  if (epub && EpubReaderUtils::saveProgress(*epub, remotePosition.spineIndex, remotePosition.pageNumber, 0)) {
    returnToReader();
    return;
  }
  LOG_ERR("PSYNC", "Failed to save remote progress");
  returnToReader();
}

void PeerSyncActivity::returnToReader() { activityManager.goToReader(epubPath); }

void PeerSyncActivity::loop() {
  const uint32_t now = millis();
  peerSync.update(now);

  if (now - lastPingMs >= 1000) {
    lastPingMs = now;
    peerSync.sendPing(docHash.c_str(), localProgress.percentage, localBookTitle.c_str(), localProgress.xpath.c_str());
  }

  if (state == LISTENING) {
    auto peers = sameBookPeers();
    if (!peers.empty()) {
      if (selectedIdx >= static_cast<int>(peers.size())) selectedIdx = 0;
      if (mappedInput.wasReleased(MappedInputManager::Button::Down) ||
          mappedInput.wasReleased(MappedInputManager::Button::Right)) {
        selectedIdx = (selectedIdx + 1) % static_cast<int>(peers.size());
        requestUpdate();
      } else if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                 mappedInput.wasReleased(MappedInputManager::Button::Left)) {
        selectedIdx = (selectedIdx + static_cast<int>(peers.size()) - 1) % static_cast<int>(peers.size());
        requestUpdate();
      } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        selectPeerAndMap(*peers[selectedIdx]);
        return;
      }
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      returnToReader();
      return;
    }
    // Refresh only when the visible list actually changes (e-ink: avoid refresh storms).
    const uint32_t sig = listSignature();
    if (sig != lastListSig) {
      lastListSig = sig;
      requestUpdate();
    }
    return;
  }

  // SHOWING_RESULT
  if (alreadySynced) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      returnToReader();
    }
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
      mappedInput.wasReleased(MappedInputManager::Button::Down) ||
      mappedInput.wasReleased(MappedInputManager::Button::Left) ||
      mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    conflictOption = (conflictOption + 1) % 2;
    requestUpdate();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (conflictOption == 0) {
      applyRemoteAndReturn();
    } else {
      returnToReader();
    }
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    returnToReader();
  }
}

void PeerSyncActivity::render(RenderLock&&) {
  renderer.clearScreen();
  auto metrics = UITheme::getInstance().getMetrics();
  Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_PEER_SYNC));

  if (state == LISTENING) {
    const int top = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    auto peers = sameBookPeers();

    if (peers.empty()) {
      UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, screen.y + screen.height / 2 - 20,
                                tr(STR_PEER_SEARCHING), true, EpdFontFamily::BOLD);
    } else {
      renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top, tr(STR_PEER_NEARBY), true,
                        EpdFontFamily::BOLD);
      const int rowH = 46;
      int y = top + 34;
      for (int i = 0; i < static_cast<int>(peers.size()); ++i) {
        const PeerSync::Peer* p = peers[i];
        const bool sel = (i == selectedIdx);
        if (sel) renderer.fillRect(screen.x, y - 4, screen.width - 1, rowH);
        char macStr[18];
        formatMacShort(p->mac, macStr, sizeof(macStr));
        char line1[96];
        snprintf(line1, sizeof(line1), "%s", p->title[0] ? p->title : macStr);
        renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, y, line1, !sel);
        char line2[64];
        snprintf(line2, sizeof(line2), tr(STR_PEER_ROW_FORMAT), p->percentage * 100, macStr);
        renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, y + 22, line2, !sel);
        y += rowH;
      }
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), peers.empty() ? "" : tr(STR_SELECT),
                                              peers.empty() ? "" : tr(STR_DIR_UP),
                                              peers.empty() ? "" : tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  // SHOWING_RESULT
  int top = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  if (alreadySynced) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, screen.y + screen.height / 2 - 20,
                              tr(STR_PEER_ALREADY_SYNCED), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  const int remoteTocIndex = epub->getTocIndexForSpineIndex(remotePosition.spineIndex);
  const std::string remoteChapter =
      (remoteTocIndex >= 0) ? epub->getTocItem(remoteTocIndex).title
                            : (std::string(tr(STR_SECTION_PREFIX)) + std::to_string(remotePosition.spineIndex + 1));
  const std::string localChapter =
      !localChapterName.empty() ? localChapterName
                                : (std::string(tr(STR_SECTION_PREFIX)) + std::to_string(currentSpineIndex + 1));

  renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top, tr(STR_PEER_FOUND), true,
                    EpdFontFamily::BOLD);

  renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 40, tr(STR_REMOTE_LABEL), true);
  char buf[128];
  snprintf(buf, sizeof(buf), "  %s", !remoteTitle.empty() ? remoteTitle.c_str() : remoteMacStr);
  renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 65, buf);
  snprintf(buf, sizeof(buf), "  %s", remoteChapter.c_str());
  renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 90, buf);
  snprintf(buf, sizeof(buf), tr(STR_PAGE_OVERALL_FORMAT), remotePosition.pageNumber + 1, remotePercentage * 100);
  renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 115, buf);

  renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 150, tr(STR_LOCAL_LABEL), true);
  snprintf(buf, sizeof(buf), "  %s", localChapter.c_str());
  renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 175, buf);
  snprintf(buf, sizeof(buf), tr(STR_PAGE_TOTAL_OVERALL_FORMAT), currentPage + 1, totalPagesInSpine,
           localProgress.percentage * 100);
  renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 200, buf);

  const int optionY = top + 230;
  const int optionHeight = 30;
  if (conflictOption == 0) renderer.fillRect(screen.x, optionY - 2, screen.width - 1, optionHeight);
  renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, optionY, tr(STR_PEER_GO_TO),
                    conflictOption != 0);
  if (conflictOption == 1) renderer.fillRect(screen.x, optionY + optionHeight - 2, screen.width - 1, optionHeight);
  renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, optionY + optionHeight, tr(STR_PEER_STAY),
                    conflictOption != 1);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
