#pragma once
#include <Epub.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ProgressMapper.h"
#include "activities/Activity.h"
#include "network/PeerSync.h"

/**
 * Peer-to-peer reading-position sync over ESP-NOW, no server (cf. KOReaderSync).
 *
 * Both readers open this screen. Each broadcasts a presence ping (docHash +
 * percentage + title + xpath) ~1Hz and shows a live list of nearby peers keyed
 * by MAC. Selecting a peer reading the same book (matching docHash) and pressing
 * Confirm maps their position onto this device's pagination via ProgressMapper
 * and shows the same conflict prompt KOReaderSync uses (go to their spot / stay,
 * defaulting to the furthest progress).
 */
class PeerSyncActivity final : public Activity {
 public:
  PeerSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& epubPath,
                   int currentSpineIndex, int currentPage, int totalPagesInSpine, KOReaderPosition localKoPos,
                   std::string localChapterName, std::string localBookTitle, std::string localBookAuthor,
                   std::optional<uint16_t> currentParagraphIndex = std::nullopt)
      : Activity("PeerSync", renderer, mappedInput),
        epubPath(epubPath),
        localChapterName(std::move(localChapterName)),
        localBookTitle(std::move(localBookTitle)),
        localBookAuthor(std::move(localBookAuthor)),
        currentSpineIndex(currentSpineIndex),
        currentPage(currentPage),
        totalPagesInSpine(totalPagesInSpine),
        currentParagraphIndex(currentParagraphIndex),
        localProgress(std::move(localKoPos)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }  // radio is up for the whole screen

 private:
  enum State { LISTENING, SHOWING_RESULT };

  std::vector<const PeerSync::Peer*> sameBookPeers() const;
  uint32_t listSignature() const;  // cheap change-detector to avoid e-ink refresh storms
  void selectPeerAndMap(const PeerSync::Peer& peer);
  void ensureEpubLoaded();
  void applyRemoteAndReturn();
  void returnToReader();

  std::string epubPath;
  std::string localChapterName;
  std::string localBookTitle;
  std::string localBookAuthor;
  int currentSpineIndex;
  int currentPage;
  int totalPagesInSpine;
  std::optional<uint16_t> currentParagraphIndex;
  KOReaderPosition localProgress;
  std::string docHash;

  std::shared_ptr<Epub> epub;  // lazy: only loaded to map a chosen peer's position
  PeerSync peerSync;
  State state = LISTENING;
  int selectedIdx = 0;
  uint32_t lastPingMs = 0;
  uint32_t lastListSig = 0;

  // Snapshot of the chosen peer for the conflict screen (the table may churn after selection).
  CrossPointPosition remotePosition{};
  float remotePercentage = 0.0f;
  std::string remoteTitle;
  char remoteMacStr[18] = {};
  bool alreadySynced = false;
  int conflictOption = 0;  // 0 = go to peer, 1 = stay
};
