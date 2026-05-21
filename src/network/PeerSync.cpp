#include "PeerSync.h"

#include <Arduino.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include <cstring>

namespace {
PeerSync* g_active = nullptr;  // esp_now recv cb carries no user ctx; only one screen is live at a time
constexpr uint8_t kBroadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Wire layout: magic0, magic1, version, hash[32], percentage(float, raw), titleLen, title, xpathLen, xpath
constexpr uint8_t MAGIC0 = 'X';
constexpr uint8_t MAGIC1 = 'P';
constexpr uint8_t VERSION = 1;

size_t clampedLen(const char* s, size_t max) {
  size_t n = 0;
  if (!s) return 0;
  while (n < max && s[n] != '\0') ++n;
  return n;
}
}  // namespace

bool PeerSync::begin() {
  if (active_) return true;

  rxQueue_ = xQueueCreate(8, sizeof(RxMsg));
  if (!rxQueue_) {
    LOG_ERR("PSYNC", "OOM: rx queue");
    return false;
  }
  peers_.reserve(8);

  // STA mode brings up esp_wifi without connecting; we never call WiFi.begin(), so
  // there is no association, no DHCP, no lwIP sockets — only the radio + ESP-NOW.
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, false);  // never associate; a saved AP would pull us off the fixed channel
  esp_wifi_set_channel(CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_ps(WIFI_PS_NONE);  // RX must stay awake to hear pings while on this screen

  if (esp_now_init() != ESP_OK) {
    LOG_ERR("PSYNC", "esp_now_init failed");
    WiFi.mode(WIFI_OFF);
    vQueueDelete(rxQueue_);
    rxQueue_ = nullptr;
    return false;
  }
  g_active = this;
  esp_now_register_recv_cb(onRecvStatic);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, kBroadcast, 6);
  peer.channel = CHANNEL;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    LOG_ERR("PSYNC", "add broadcast peer failed");
  }

  active_ = true;
  uint8_t actualCh = 0;
  wifi_second_chan_t sec;
  esp_wifi_get_channel(&actualCh, &sec);
  LOG_INF("PSYNC", "begin want_ch=%u actual_ch=%u wifi_status=%d heap=%u", CHANNEL, actualCh, (int)WiFi.status(),
          (unsigned)ESP.getFreeHeap());
  return true;
}

void PeerSync::end() {
  if (!active_) return;
  esp_now_unregister_recv_cb();
  esp_now_del_peer(kBroadcast);
  esp_now_deinit();
  g_active = nullptr;
  WiFi.mode(WIFI_OFF);
  if (rxQueue_) {
    vQueueDelete(rxQueue_);
    rxQueue_ = nullptr;
  }
  peers_.clear();
  active_ = false;
  LOG_DBG("PSYNC", "end heap=%u", (unsigned)ESP.getFreeHeap());
}

void PeerSync::onRecvStatic(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  PeerSync* self = g_active;
  if (!self || !self->rxQueue_ || !info || len <= 0 || len > (int)sizeof(RxMsg::buf)) return;
  const uint8_t* m = info->src_addr;
  LOG_INF("PSYNC", "rx %d bytes from %02X:%02X:%02X:%02X:%02X:%02X", len, m[0], m[1], m[2], m[3], m[4], m[5]);
  RxMsg msg;
  memcpy(msg.mac, info->src_addr, 6);
  msg.len = static_cast<uint8_t>(len);
  memcpy(msg.buf, data, len);
  // WiFi-task context (not ISR): plain queue send. Drop if full; the next ping recovers.
  xQueueSend(self->rxQueue_, &msg, 0);
}

size_t PeerSync::buildPing(uint8_t* out, const char* docHash, float percentage, const char* title, const char* xpath) {
  size_t off = 0;
  out[off++] = MAGIC0;
  out[off++] = MAGIC1;
  out[off++] = VERSION;

  const size_t hl = clampedLen(docHash, HASH_LEN);
  for (size_t i = 0; i < HASH_LEN; ++i) out[off++] = i < hl ? static_cast<uint8_t>(docHash[i]) : 0;

  memcpy(out + off, &percentage, sizeof(percentage));
  off += sizeof(percentage);

  size_t tl = clampedLen(title, MAX_TITLE);
  // Back off a UTF-8 continuation byte so a truncated title doesn't split a glyph.
  while (tl > 0 && (static_cast<uint8_t>(title[tl]) & 0xC0) == 0x80) --tl;
  out[off++] = static_cast<uint8_t>(tl);
  memcpy(out + off, title, tl);
  off += tl;

  const size_t xl = clampedLen(xpath, MAX_XPATH);
  out[off++] = static_cast<uint8_t>(xl);
  memcpy(out + off, xpath, xl);
  off += xl;
  return off;
}

void PeerSync::sendPing(const char* docHash, float percentage, const char* title, const char* xpath) {
  if (!active_) return;
  uint8_t buf[ESP_NOW_MAX_DATA_LEN];
  const size_t len = buildPing(buf, docHash, percentage, title, xpath);
  const esp_err_t err = esp_now_send(kBroadcast, buf, len);
  static uint32_t lastTxLog = 0;
  if (err != ESP_OK || millis() - lastTxLog > 3000) {
    lastTxLog = millis();
    LOG_INF("PSYNC", "tx len=%u err=%d", (unsigned)len, (int)err);
  }
}

PeerSync::Peer& PeerSync::peerForMac(const uint8_t mac[6]) {
  for (auto& p : peers_)
    if (memcmp(p.mac, mac, 6) == 0) return p;
  peers_.emplace_back();
  return peers_.back();
}

void PeerSync::ingest(const RxMsg& msg, uint32_t nowMs) {
  const uint8_t* b = msg.buf;
  const size_t len = msg.len;
  if (len < 3 + HASH_LEN + sizeof(float) + 2) return;  // shortest valid frame
  if (b[0] != MAGIC0 || b[1] != MAGIC1 || b[2] != VERSION) return;

  size_t off = 3;
  char hash[HASH_LEN + 1];
  memcpy(hash, b + off, HASH_LEN);
  hash[HASH_LEN] = '\0';
  off += HASH_LEN;

  float pct;
  memcpy(&pct, b + off, sizeof(pct));  // RISC-V: never deref a cast; copy out
  off += sizeof(pct);

  if (off >= len) return;
  const uint8_t tl = b[off++];
  if (tl > MAX_TITLE || off + tl > len) return;
  char title[MAX_TITLE + 1];
  memcpy(title, b + off, tl);
  title[tl] = '\0';
  off += tl;

  if (off >= len) return;
  const uint8_t xl = b[off++];
  if (xl > MAX_XPATH || off + xl > len) return;
  char xpath[MAX_XPATH + 1];
  memcpy(xpath, b + off, xl);
  xpath[xl] = '\0';

  Peer& p = peerForMac(msg.mac);
  memcpy(p.mac, msg.mac, 6);
  memcpy(p.docHash, hash, sizeof(hash));
  p.percentage = pct;
  memcpy(p.title, title, sizeof(title));
  memcpy(p.xpath, xpath, sizeof(xpath));
  p.lastSeenMs = nowMs;
}

void PeerSync::update(uint32_t nowMs) {
  if (rxQueue_) {
    RxMsg msg;
    while (xQueueReceive(rxQueue_, &msg, 0) == pdTRUE) ingest(msg, nowMs);
  }
  for (size_t i = peers_.size(); i-- > 0;) {
    if (nowMs - peers_[i].lastSeenMs > PEER_TTL_MS) peers_.erase(peers_.begin() + i);
  }
}

#ifdef DEV_KEEP_AWAKE
void PeerSync::devInjectPeer(const char* docHash, float percentage, const char* title, const char* xpath) {
  const uint8_t fakeMac[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01};
  Peer& p = peerForMac(fakeMac);
  memcpy(p.mac, fakeMac, 6);
  strncpy(p.docHash, docHash ? docHash : "", HASH_LEN);
  p.docHash[HASH_LEN] = '\0';
  p.percentage = percentage;
  strncpy(p.title, title ? title : "", MAX_TITLE);
  p.title[MAX_TITLE] = '\0';
  strncpy(p.xpath, xpath ? xpath : "", MAX_XPATH);
  p.xpath[MAX_XPATH] = '\0';
  p.lastSeenMs = millis();
}

void PeerSync::devSelfTest() {
  // Refuse if a PeerSyncActivity already owns the radio: this temp's begin() would
  // hit esp_now_init-already-done, and its cleanup path would WIFI_OFF the live one.
  if (g_active) {
    LOG_ERR("PSYNC", "selftest skipped: radio already in use");
    return;
  }
  LOG_INF("PSYNC", "selftest start heap=%u", (unsigned)ESP.getFreeHeap());

  // 1. Wire-format roundtrip, headless: serialize a ping then parse it back via ingest().
  const char* hash = "0123456789abcdef0123456789abcdef";
  const char* xp = "/body/DocFragment[3]/body/p[42]/text().17";
  uint8_t buf[ESP_NOW_MAX_DATA_LEN];
  const size_t len = buildPing(buf, hash, 0.42f, "Self Test Book", xp);
  RxMsg msg;
  const uint8_t fakeMac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x99};
  memcpy(msg.mac, fakeMac, 6);
  msg.len = static_cast<uint8_t>(len);
  memcpy(msg.buf, buf, len);
  peers_.clear();
  ingest(msg, 1000);
  if (peers_.size() == 1) {
    const Peer& p = peers_[0];
    const bool ok = strcmp(p.docHash, hash) == 0 && p.percentage > 0.41f && p.percentage < 0.43f &&
                    strcmp(p.title, "Self Test Book") == 0 && strcmp(p.xpath, xp) == 0;
    LOG_INF("PSYNC", "selftest roundtrip len=%u title='%s' pct=%.3f xpathlen=%u %s", (unsigned)len, p.title,
            p.percentage, (unsigned)strlen(p.xpath), ok ? "OK" : "MISMATCH");
  } else {
    LOG_ERR("PSYNC", "selftest roundtrip FAILED peers=%u", (unsigned)peers_.size());
  }
  peers_.clear();

  // 2. Real radio up/down, repeated: per-cycle heap delta distinguishes a one-time
  //    WiFi-driver init cost (acceptable) from a per-entry leak (would OOM the activity).
  for (int i = 0; i < 3; ++i) {
    const uint32_t before = ESP.getFreeHeap();
    if (!begin()) {
      LOG_ERR("PSYNC", "selftest begin() FAILED cycle=%d", i);
      break;
    }
    sendPing(hash, 0.42f, "Self Test Book", xp);
    update(millis());
    end();
    const uint32_t after = ESP.getFreeHeap();
    LOG_INF("PSYNC", "selftest cycle=%d before=%u after=%u delta=%d", i, (unsigned)before, (unsigned)after,
            static_cast<int>(after) - static_cast<int>(before));
  }
  LOG_INF("PSYNC", "selftest done heap=%u", (unsigned)ESP.getFreeHeap());
}
#endif
