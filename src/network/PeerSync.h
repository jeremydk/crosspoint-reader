#pragma once

#include <esp_now.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <cstdint>
#include <vector>

/**
 * Connectionless peer presence + reading-position exchange over ESP-NOW.
 *
 * No association, no lwIP, no server: the radio comes up in STA mode only to
 * pump broadcast frames on a fixed channel, then drops on end(). Each device on
 * the Peer Sync screen broadcasts a ping carrying {docHash, percentage, title,
 * xpath} ~1Hz; every peer builds a table keyed by sender MAC and shows it. A
 * single frame carries both the list-display fields and the data needed to sync
 * a position, so there is no request/response round-trip.
 *
 * The ESP-NOW receive callback runs in WiFi-task context (not an ISR): it only
 * copies the frame into a FreeRTOS queue. Deserialization and table updates
 * happen in update(), driven from the activity loop. Wire fields are pulled out
 * with field-by-field memcpy; the frame is never cast to a struct (RISC-V
 * unaligned-access fault hazard).
 */
class PeerSync {
 public:
  static constexpr uint8_t CHANNEL = 1;        // fixed: no AP to coordinate channel with
  static constexpr size_t HASH_LEN = 32;       // KOReaderDocumentId hex length
  static constexpr size_t MAX_TITLE = 64;      // bytes on the wire (UTF-8, truncated)
  static constexpr size_t MAX_XPATH = 128;     // synthetic xpaths are short; this is headroom
  static constexpr uint32_t PEER_TTL_MS = 6000;  // drop a peer after this long without a ping

  struct Peer {
    uint8_t mac[6] = {};
    char docHash[HASH_LEN + 1] = {};
    float percentage = 0.0f;
    char title[MAX_TITLE + 1] = {};
    char xpath[MAX_XPATH + 1] = {};
    uint32_t lastSeenMs = 0;
  };

  bool begin();
  void end();
  bool active() const { return active_; }

  // Broadcast our presence + position. No-op if !active. Truncates title/xpath.
  void sendPing(const char* docHash, float percentage, const char* title, const char* xpath);

  // Drain the RX queue into the peer table and expire stale entries.
  void update(uint32_t nowMs);

  const std::vector<Peer>& peers() const { return peers_; }

#ifdef DEV_KEEP_AWAKE
  // Single-device testing: synthesize a peer so the list/conflict path is exercisable.
  void devInjectPeer(const char* docHash, float percentage, const char* title, const char* xpath);
  // Headless transport check (no UI/Epub): begin -> serialize/parse roundtrip -> broadcast
  // -> teardown, logging heap deltas. Verifies ESP-NOW up/down + heap cleanliness on device.
  void devSelfTest();
#endif

 private:
  struct RxMsg {
    uint8_t mac[6];
    uint8_t len;
    uint8_t buf[ESP_NOW_MAX_DATA_LEN];
  };

  static void onRecvStatic(const esp_now_recv_info_t* info, const uint8_t* data, int len);
  void ingest(const RxMsg& msg, uint32_t nowMs);
  Peer& peerForMac(const uint8_t mac[6]);  // find-or-append

  // Serialize a ping into out (capacity ESP_NOW_MAX_DATA_LEN); returns byte count.
  static size_t buildPing(uint8_t* out, const char* docHash, float percentage, const char* title, const char* xpath);

  bool active_ = false;
  QueueHandle_t rxQueue_ = nullptr;
  std::vector<Peer> peers_;
};
