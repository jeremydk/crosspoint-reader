#include "EfuseInoculation.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <esp_efuse.h>
#include <esp_efuse_table.h>
#include <esp_mac.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <memory>

namespace efuse_inoculation {
namespace {

constexpr const char* kCrosspointDir = "/.crosspoint";
constexpr const char* kAuditFile = "/.crosspoint/efuse_inoculation_audit.txt";
constexpr const char* kLogTag = "EFUSE";
constexpr size_t kAuditBufBytes = 4096;
constexpr size_t kMaxFieldBytes = 32;  // BLOCK_KEY width — biggest field we read

constexpr const char* kProtNewly = "newly_protected";
constexpr const char* kProtAlready = "already_protected";
constexpr const char* kProtValidateFail = "validate_fail";

// Burning any one field's WR_DIS bit locks every field that shares the bit.
// Listed individually so the inspector and audit log show them all.
struct B0Field {
  const char* name;
  const esp_efuse_desc_t** wrDis;
  const esp_efuse_desc_t** field;
};

const B0Field kB0Fields[] = {
    // Group A: also covers DIS_USB_SERIAL_JTAG, DIS_PAD_JTAG, DIS_FORCE_DOWNLOAD,
    // DIS_ICACHE, DIS_DOWNLOAD_ICACHE, DIS_TWAI, DIS_DOWNLOAD_MANUAL_ENCRYPT
    {"DIS_USB_JTAG", ESP_EFUSE_WR_DIS_DIS_USB_JTAG, ESP_EFUSE_DIS_USB_JTAG},
    {"SOFT_DIS_JTAG", ESP_EFUSE_WR_DIS_SOFT_DIS_JTAG, ESP_EFUSE_SOFT_DIS_JTAG},
    // Group B: also covers DIS_DOWNLOAD_MODE, DIS_USB_SERIAL_JTAG_DOWNLOAD_MODE, +6
    {"SECURE_VERSION", ESP_EFUSE_WR_DIS_SECURE_VERSION, ESP_EFUSE_SECURE_VERSION},
    {"SECURE_BOOT_EN", ESP_EFUSE_WR_DIS_SECURE_BOOT_EN, ESP_EFUSE_SECURE_BOOT_EN},
    {"SECURE_BOOT_AGGRESSIVE_REVOKE", ESP_EFUSE_WR_DIS_SECURE_BOOT_AGGRESSIVE_REVOKE,
     ESP_EFUSE_SECURE_BOOT_AGGRESSIVE_REVOKE},
    {"SECURE_BOOT_KEY_REVOKE0", ESP_EFUSE_WR_DIS_SECURE_BOOT_KEY_REVOKE0, ESP_EFUSE_SECURE_BOOT_KEY_REVOKE0},
    {"SECURE_BOOT_KEY_REVOKE1", ESP_EFUSE_WR_DIS_SECURE_BOOT_KEY_REVOKE1, ESP_EFUSE_SECURE_BOOT_KEY_REVOKE1},
    {"SECURE_BOOT_KEY_REVOKE2", ESP_EFUSE_WR_DIS_SECURE_BOOT_KEY_REVOKE2, ESP_EFUSE_SECURE_BOOT_KEY_REVOKE2},
    {"SPI_BOOT_CRYPT_CNT", ESP_EFUSE_WR_DIS_SPI_BOOT_CRYPT_CNT, ESP_EFUSE_SPI_BOOT_CRYPT_CNT},
    {"KEY_PURPOSE_0", ESP_EFUSE_WR_DIS_KEY_PURPOSE_0, ESP_EFUSE_KEY_PURPOSE_0},
    {"KEY_PURPOSE_1", ESP_EFUSE_WR_DIS_KEY_PURPOSE_1, ESP_EFUSE_KEY_PURPOSE_1},
    {"KEY_PURPOSE_2", ESP_EFUSE_WR_DIS_KEY_PURPOSE_2, ESP_EFUSE_KEY_PURPOSE_2},
    {"KEY_PURPOSE_3", ESP_EFUSE_WR_DIS_KEY_PURPOSE_3, ESP_EFUSE_KEY_PURPOSE_3},
    {"KEY_PURPOSE_4", ESP_EFUSE_WR_DIS_KEY_PURPOSE_4, ESP_EFUSE_KEY_PURPOSE_4},
    {"KEY_PURPOSE_5", ESP_EFUSE_WR_DIS_KEY_PURPOSE_5, ESP_EFUSE_KEY_PURPOSE_5},
    {"RD_DIS", ESP_EFUSE_WR_DIS_RD_DIS, ESP_EFUSE_RD_DIS},
};

struct KeyBlock {
  const char* name;
  esp_efuse_block_t blk;
  const esp_efuse_desc_t** wrDis;
};

const KeyBlock kKeyBlocks[] = {
    {"BLOCK_KEY0", EFUSE_BLK_KEY0, ESP_EFUSE_WR_DIS_KEY0}, {"BLOCK_KEY1", EFUSE_BLK_KEY1, ESP_EFUSE_WR_DIS_KEY1},
    {"BLOCK_KEY2", EFUSE_BLK_KEY2, ESP_EFUSE_WR_DIS_KEY2}, {"BLOCK_KEY3", EFUSE_BLK_KEY3, ESP_EFUSE_WR_DIS_KEY3},
    {"BLOCK_KEY4", EFUSE_BLK_KEY4, ESP_EFUSE_WR_DIS_KEY4}, {"BLOCK_KEY5", EFUSE_BLK_KEY5, ESP_EFUSE_WR_DIS_KEY5},
};

bool isWriteProtected(const esp_efuse_desc_t** wrDis) {
  uint8_t bit = 0;
  return esp_efuse_read_field_blob(wrDis, &bit, 1) == ESP_OK && bit != 0;
}

// True iff the field is non-zero (already burned against us).
bool describeField(const esp_efuse_desc_t** field, char* out, size_t outSize) {
  if (!field) {
    snprintf(out, outSize, "default");
    return false;
  }
  const size_t bits = esp_efuse_get_field_size(field);
  if (bits == 0) {
    snprintf(out, outSize, "empty");
    return false;
  }
  const size_t nbytes = (bits + 7) / 8;
  if (nbytes > kMaxFieldBytes) {
    snprintf(out, outSize, "too_large");
    return false;
  }
  uint8_t buf[kMaxFieldBytes] = {0};
  if (esp_err_t err = esp_efuse_read_field_blob(field, buf, bits); err != ESP_OK) {
    snprintf(out, outSize, "read_err_%s", esp_err_to_name(err));
    return false;
  }
  if (std::all_of(buf, buf + nbytes, [](uint8_t b) { return b == 0; })) {
    snprintf(out, outSize, "default");
    return false;
  }
  // Big-endian hex, matches espefuse output.
  size_t off = static_cast<size_t>(snprintf(out, outSize, "nondefault_0x"));
  for (size_t i = nbytes; i > 0 && off + 2 < outSize; i--) {
    off += static_cast<size_t>(snprintf(out + off, outSize - off, "%02X", buf[i - 1]));
  }
  return true;
}

// Heap-allocated growable string. Stack would blow the 256B locals budget.
struct AuditBuf {
  std::unique_ptr<char[]> data{new (std::nothrow) char[kAuditBufBytes]};
  size_t off = 0;
  bool ok() const { return data != nullptr; }
};

void appendf(AuditBuf& buf, const char* fmt, ...) {
  if (!buf.ok() || buf.off + 1 >= kAuditBufBytes) return;
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(buf.data.get() + buf.off, kAuditBufBytes - buf.off, fmt, ap);
  va_end(ap);
  if (n > 0) buf.off = std::min(buf.off + static_cast<size_t>(n), kAuditBufBytes - 1);
}

void writeAudit(const AuditBuf& buf) {
  if (!buf.ok()) return;
  Storage.ensureDirectoryExists(kCrosspointDir);
  HalFile file;
  if (!Storage.openFileForWrite(kLogTag, kAuditFile, file) || !file) {
    LOG_ERR(kLogTag, "Failed to open %s", kAuditFile);
    return;
  }
  const size_t got = file.write(reinterpret_cast<const uint8_t*>(buf.data.get()), buf.off);
  if (got != buf.off) LOG_ERR(kLogTag, "Audit short write: %zu/%zu", got, buf.off);
  file.close();
}

const char* protectResult(esp_err_t err, const char* name, InoculateResult& r) {
  if (err == ESP_OK) {
    LOG_INF(kLogTag, "%s newly write-protected", name);
    r.newly++;
    return kProtNewly;
  }
  if (err == ESP_ERR_EFUSE_REPEATED_PROG) {
    // Sibling field already burned the shared WR_DIS bit.
    r.already++;
    return kProtAlready;
  }
  LOG_ERR(kLogTag, "%s write-protect failed: %s", name, esp_err_to_name(err));
  r.writeFail++;
  return esp_err_to_name(err);
}

}  // namespace

ScanResult scan() {
  ScanResult r;
  r.fields.reserve(sizeof(kB0Fields) / sizeof(kB0Fields[0]) + sizeof(kKeyBlocks) / sizeof(kKeyBlocks[0]));

  for (const auto& f : kB0Fields) {
    FieldStatus s{};
    s.name = f.name;
    s.nonDefault = describeField(f.field, s.state, sizeof(s.state));
    s.writeProtected = isWriteProtected(f.wrDis);
    if (s.nonDefault) r.nonDefault++;
    if (s.writeProtected) r.alreadyProtected++;
    r.fields.push_back(s);
  }
  for (const auto& k : kKeyBlocks) {
    FieldStatus s{};
    s.name = k.name;
    snprintf(s.state, sizeof(s.state), "(key block)");
    s.nonDefault = false;
    s.writeProtected = isWriteProtected(k.wrDis);
    if (s.writeProtected) r.alreadyProtected++;
    r.fields.push_back(s);
  }
  return r;
}

InoculateResult inoculate() {
  LOG_INF(kLogTag, "Starting eFuse inoculation");

  AuditBuf audit;
  if (!audit.ok()) LOG_ERR(kLogTag, "Audit alloc failed; running without audit log");

  // Forensic header: lets the team correlate audits across a fleet.
  char mac[18];
  formatDefaultMac(mac, sizeof(mac));
  appendf(audit, "CrossPoint eFuse inoculation v1\n");
  appendf(audit, "firmware=%s\n", CROSSPOINT_VERSION);
  appendf(audit, "mac=%s\n", mac);
  appendf(audit, "uptime_ms=%lu\n", static_cast<unsigned long>(millis()));
  appendf(audit, "---\n\n");

  InoculateResult r;

  appendf(audit, "[block0_fields]\n");
  for (const auto& f : kB0Fields) {
    char state[64];
    if (describeField(f.field, state, sizeof(state))) {
      LOG_ERR(kLogTag, "%s in non-default state (%s); locking anyway", f.name, state);
      r.badState++;
    }
    const char* prot;
    if (isWriteProtected(f.wrDis)) {
      r.already++;
      prot = kProtAlready;
    } else {
      prot = protectResult(esp_efuse_write_field_bit(f.wrDis), f.name, r);
    }
    appendf(audit, "  %-32s state=%-26s prot=%s\n", f.name, state, prot);
  }

  appendf(audit, "\n[key_blocks]\n");
  for (const auto& k : kKeyBlocks) {
    const char* prot;
    if (isWriteProtected(k.wrDis)) {
      r.already++;
      prot = kProtAlready;
    } else {
      prot = protectResult(esp_efuse_set_write_protect(k.blk), k.name, r);
    }
    appendf(audit, "  %-32s prot=%s\n", k.name, prot);
  }

  appendf(audit, "\n[validation]\n");
  auto check = [&](const esp_efuse_desc_t** wrDis, const char* name) {
    if (!isWriteProtected(wrDis)) {
      LOG_ERR(kLogTag, "%s NOT write-protected", name);
      appendf(audit, "  %-32s %s\n", name, kProtValidateFail);
      r.validateFail++;
    }
  };
  for (const auto& f : kB0Fields) check(f.wrDis, f.name);
  for (const auto& k : kKeyBlocks) check(k.wrDis, k.name);
  if (r.validateFail == 0) appendf(audit, "  all fields verified write-protected\n");

  appendf(audit,
          "\n[summary]\n"
          "  newly_protected     %zu\n"
          "  already_protected   %zu\n"
          "  bad_state_findings  %zu\n"
          "  write_failures      %zu\n"
          "  validation_failures %zu\n"
          "  overall             %s\n"
          "\n"
          "note: 'already_protected' reflects WR_DIS state on entry. On a device's\n"
          "first inoculation run, those fields were locked by someone other than\n"
          "this firmware — i.e. vendor-lockdown evidence.\n",
          r.newly, r.already, r.badState, r.writeFail, r.validateFail, r.ok() ? "ok" : "partial");

  LOG_INF(kLogTag, "Done: newly=%zu already=%zu bad=%zu fail=%zu vfail=%zu (%s)", r.newly, r.already, r.badState,
          r.writeFail, r.validateFail, r.ok() ? "OK" : "PARTIAL");

  writeAudit(audit);
  return r;
}

void formatDefaultMac(char* out, size_t outSize) {
  uint8_t mac[6] = {0};
  esp_efuse_mac_get_default(mac);
  snprintf(out, outSize, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

}  // namespace efuse_inoculation
