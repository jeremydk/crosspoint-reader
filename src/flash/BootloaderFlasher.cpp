#include "BootloaderFlasher.h"

#include <HalStorage.h>
#include <Logging.h>
#include <esp_bootloader_desc.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <cstring>
#include <memory>

#include <Arduino.h>
#include <bootloader_flash_priv.h>
#include <esp_flash_encrypt.h>
#include <esp_flash_partitions.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_secure_boot.h>
#include <spi_flash_mmap.h>

namespace bootloader_flash {

namespace {

constexpr uint8_t  BL_ESP_IMAGE_MAGIC      = 0xE9;
constexpr size_t   BL_HEADER_SIZE          = 24;
constexpr size_t   BL_SEG_HEADER_SIZE      = 8;
constexpr size_t   BL_SHA_TRAILER          = 32;
constexpr uint8_t  BL_CHECKSUM_SEED        = 0xEF;
constexpr size_t   BL_HASH_APPENDED_OFFSET = 23;
// Byte 12 is the low byte of esp_image_header_t::chip_id. ESP_CHIP_ID_ESP32C3
// is 0x0005 (high byte always 0 in IDF 5.x), so a 1-byte compare is enough.
// Hardcoded because the validator runs in host tests too (no sdkconfig there).
constexpr size_t   BL_CHIP_ID_OFFSET       = 12;
constexpr uint8_t  BL_CHIP_ID_THIS         = 5;
// header (24) + first seg header (8); esp_bootloader_desc_t lives here.
constexpr size_t   BL_DESC_OFFSET          = BL_HEADER_SIZE + BL_SEG_HEADER_SIZE;
// 4 KB is the smallest possible erase sector; anything below isn't a real
// flash dump. Real bootloaders are ~28 KB.
constexpr size_t   BL_MIN_SIZE             = 4 * 1024;
constexpr size_t   BL_CHUNK                = 4096;
constexpr size_t   BL_FLASH_SECTOR         = SPI_FLASH_SEC_SIZE;
constexpr size_t   BL_FLASH_BLOCK          = 64 * 1024;
constexpr uint32_t BL_REGION_OFFSET        = ESP_PRIMARY_BOOTLOADER_OFFSET;
constexpr uint32_t BL_REGION_SIZE          = ESP_BOOTLOADER_SIZE;

constexpr const char* TAG = "BLFLASH";

// Set inside flashFromSdPath() around the bootloader-region erase and writes.
// __wrap_esp_partition_main_flash_region_safe (below) bypasses the IDF's
// CONFIG_SPI_FLASH_DANGEROUS_WRITE_ABORTS guard for the bootloader region
// only while this is true; every other write path keeps the normal guard.
// Single-core, single-threaded flash op, so plain volatile is enough.
volatile bool g_allow_bootloader_write = false;

}  // namespace

extern "C" {
bool __real_esp_partition_main_flash_region_safe(size_t addr, size_t size);
bool __wrap_esp_partition_main_flash_region_safe(size_t addr, size_t size) {
  if (g_allow_bootloader_write) return true;
  return __real_esp_partition_main_flash_region_safe(addr, size);
}
}

const char* resultName(Result r) {
  switch (r) {
    case Result::OK:                       return "OK";
    case Result::OPEN_FAIL:                return "OPEN_FAIL";
    case Result::TOO_SMALL:                return "TOO_SMALL";
    case Result::TOO_LARGE:                return "TOO_LARGE";
    case Result::BAD_MAGIC:                return "BAD_MAGIC";
    case Result::NOT_BOOTLOADER:           return "NOT_BOOTLOADER";
    case Result::NOT_THIS_CHIP:            return "NOT_THIS_CHIP";
    case Result::BAD_SEGMENTS:             return "BAD_SEGMENTS";
    case Result::BAD_CHECKSUM:             return "BAD_CHECKSUM";
    case Result::BAD_SHA:                  return "BAD_SHA";
    case Result::BAD_SIZE:                 return "BAD_SIZE";
    case Result::NO_PARTITION:             return "NO_PARTITION";
    case Result::OOM:                      return "OOM";
    case Result::READ_FAIL:                return "READ_FAIL";
    case Result::ERASE_FAIL:               return "ERASE_FAIL";
    case Result::WRITE_FAIL:               return "WRITE_FAIL";
    case Result::REGISTER_FAIL:            return "REGISTER_FAIL";
    case Result::COPY_FAIL:                return "COPY_FAIL";
    case Result::VERIFY_FAIL:              return "VERIFY_FAIL";
    case Result::STAGING_TOCTOU:           return "STAGING_TOCTOU";
    case Result::SECURE_BOOT_ENABLED:      return "SECURE_BOOT_ENABLED";
    case Result::FLASH_ENCRYPTION_ENABLED: return "FLASH_ENCRYPTION_ENABLED";
    case Result::UNSUPPORTED_PT_LAYOUT:    return "UNSUPPORTED_PT_LAYOUT";
  }
  return "?";
}

namespace {

Result finishSha(mbedtls_sha256_context* sha, Result r) {
  mbedtls_sha256_free(sha);
  return r;
}

Result feedHashAndChecksum(HalFile& file, size_t length, uint8_t* xorAccum,
                            mbedtls_sha256_context* sha, uint8_t* buf) {
  size_t remaining = length;
  while (remaining > 0) {
    const size_t want = std::min<size_t>(BL_CHUNK, remaining);
    const int got = file.read(buf, want);
    if (got <= 0 || static_cast<size_t>(got) != want) return Result::READ_FAIL;
    if (sha) mbedtls_sha256_update(sha, buf, want);
    if (xorAccum) {
      uint8_t acc = *xorAccum;
      for (size_t i = 0; i < want; i++) acc ^= buf[i];
      *xorAccum = acc;
    }
    remaining -= want;
  }
  return Result::OK;
}

Result hashPartitionRange(const esp_partition_t* part, size_t offset, size_t length,
                          mbedtls_sha256_context* sha, uint8_t* buf) {
  size_t remaining = length;
  size_t cursor    = offset;
  while (remaining > 0) {
    const size_t want = std::min<size_t>(BL_CHUNK, remaining);
    if (esp_partition_read(part, cursor, buf, want) != ESP_OK) return Result::READ_FAIL;
    mbedtls_sha256_update(sha, buf, want);
    cursor    += want;
    remaining -= want;
  }
  return Result::OK;
}

}  // namespace

Result validateBootloaderFile(const char* sdPath, uint8_t out_sha[SHA_LEN]) {
  HalFile file;
  if (!Storage.openFileForRead(TAG, sdPath, file) || !file) {
    LOG_ERR(TAG, "validate: open failed: %s", sdPath);
    return Result::OPEN_FAIL;
  }

  const size_t fileSize = file.fileSize();
  if (fileSize < BL_MIN_SIZE) {
    LOG_ERR(TAG, "validate: too small: %u", static_cast<unsigned>(fileSize));
    return Result::TOO_SMALL;
  }
  // Primary "you picked an app .bin" filter: app images are MB-scale.
  if (fileSize > BL_REGION_SIZE) {
    LOG_ERR(TAG, "validate: too large: %u > %u (likely an app .bin)",
            static_cast<unsigned>(fileSize), static_cast<unsigned>(BL_REGION_SIZE));
    return Result::TOO_LARGE;
  }

  uint8_t header[BL_HEADER_SIZE];
  if (file.read(header, BL_HEADER_SIZE) != static_cast<int>(BL_HEADER_SIZE)) {
    return Result::READ_FAIL;
  }
  if (header[0] != BL_ESP_IMAGE_MAGIC) {
    LOG_ERR(TAG, "validate: bad magic 0x%02X", header[0]);
    return Result::BAD_MAGIC;
  }
  if (header[BL_CHIP_ID_OFFSET] != BL_CHIP_ID_THIS) {
    LOG_ERR(TAG, "validate: chip_id 0x%02X != 0x%02X (wrong-chip image)",
            header[BL_CHIP_ID_OFFSET], BL_CHIP_ID_THIS);
    return Result::NOT_THIS_CHIP;
  }
  const uint8_t segCount     = header[1];
  const bool    hashAppended = header[BL_HASH_APPENDED_OFFSET] != 0;

  // Bootloader vs. app discriminator at byte 32. Bootloader =
  // ESP_BOOTLOADER_DESC_MAGIC_BYTE (0x50); app's esp_app_desc_t starts
  // with 0xABCD5432 (LE), so byte[32] == 0x32 there.
  if (!file.seek(BL_DESC_OFFSET)) return Result::READ_FAIL;
  uint8_t descMagic = 0;
  if (file.read(&descMagic, 1) != 1) return Result::READ_FAIL;
  if (descMagic != ESP_BOOTLOADER_DESC_MAGIC_BYTE) {
    LOG_ERR(TAG, "validate: byte[32]=0x%02X != 0x%02X (likely an app .bin)",
            descMagic, ESP_BOOTLOADER_DESC_MAGIC_BYTE);
    return Result::NOT_BOOTLOADER;
  }
  if (!file.seek(BL_HEADER_SIZE)) return Result::READ_FAIL;

  auto buf = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[BL_CHUNK]);
  if (!buf) return Result::OOM;

  mbedtls_sha256_context shaCtx;
  mbedtls_sha256_init(&shaCtx);
  mbedtls_sha256_starts(&shaCtx, /*is224=*/0);
  mbedtls_sha256_update(&shaCtx, header, BL_HEADER_SIZE);

  uint8_t xorAccum = BL_CHECKSUM_SEED;
  size_t  pos      = BL_HEADER_SIZE;

  for (uint8_t i = 0; i < segCount; i++) {
    // Headroom-first (`fileSize - pos < N`) instead of `pos + N > fileSize`:
    // a segment with dataLen = 0xFFFFFFFF would pass the addition form via
    // uint32 wrap and we'd read 4 GB.
    if ((fileSize - pos) < BL_SEG_HEADER_SIZE) {
      LOG_ERR(TAG, "validate: seg %u header overruns EOF at %u", i, static_cast<unsigned>(pos));
      return finishSha(&shaCtx, Result::BAD_SEGMENTS);
    }
    uint8_t segHdr[BL_SEG_HEADER_SIZE];
    if (file.read(segHdr, BL_SEG_HEADER_SIZE) != static_cast<int>(BL_SEG_HEADER_SIZE)) {
      return finishSha(&shaCtx, Result::READ_FAIL);
    }
    mbedtls_sha256_update(&shaCtx, segHdr, BL_SEG_HEADER_SIZE);
    pos += BL_SEG_HEADER_SIZE;

    uint32_t dataLen;
    std::memcpy(&dataLen, segHdr + 4, sizeof(dataLen));  // RISC-V: no unaligned u32 load
    if (dataLen > (fileSize - pos)) {
      LOG_ERR(TAG, "validate: seg %u data overruns EOF", i);
      return finishSha(&shaCtx, Result::BAD_SEGMENTS);
    }
    const Result feed = feedHashAndChecksum(file, dataLen, &xorAccum, &shaCtx, buf.get());
    if (feed != Result::OK) return finishSha(&shaCtx, feed);
    pos += dataLen;
  }

  // Padding to next 16-byte boundary; (pos + 16) & ~15 guarantees padLen in
  // [1, 16], and the XOR checksum byte sits at padBuf[padLen - 1].
  const size_t padEnd        = (pos + 16) & ~static_cast<size_t>(15);
  const size_t expectedTotal = padEnd + (hashAppended ? BL_SHA_TRAILER : 0);
  if (expectedTotal != fileSize) {
    LOG_ERR(TAG, "validate: size mismatch expected=%u actual=%u",
            static_cast<unsigned>(expectedTotal), static_cast<unsigned>(fileSize));
    return finishSha(&shaCtx, Result::BAD_SIZE);
  }

  uint8_t padBuf[16];
  const size_t padLen = padEnd - pos;
  if (padLen > sizeof(padBuf)) return finishSha(&shaCtx, Result::BAD_SIZE);
  if (padLen > 0 && file.read(padBuf, padLen) != static_cast<int>(padLen)) {
    return finishSha(&shaCtx, Result::READ_FAIL);
  }
  mbedtls_sha256_update(&shaCtx, padBuf, padLen);
  if ((xorAccum & 0xFF) != padBuf[padLen - 1]) {
    LOG_ERR(TAG, "validate: checksum mismatch computed=0x%02X stored=0x%02X",
            xorAccum, padBuf[padLen - 1]);
    return finishSha(&shaCtx, Result::BAD_CHECKSUM);
  }

  // Default ESP-IDF emits hash_appended=1. A crossboot.bin without it is
  // either truncated or built with a non-stock config; refuse either way.
  if (!hashAppended) {
    LOG_ERR(TAG, "validate: hash_appended bit not set");
    return finishSha(&shaCtx, Result::BAD_SHA);
  }
  uint8_t computed[BL_SHA_TRAILER];
  mbedtls_sha256_finish(&shaCtx, computed);
  uint8_t stored[BL_SHA_TRAILER];
  if (file.read(stored, BL_SHA_TRAILER) != static_cast<int>(BL_SHA_TRAILER)) {
    mbedtls_sha256_free(&shaCtx);
    return Result::READ_FAIL;
  }
  if (std::memcmp(computed, stored, BL_SHA_TRAILER) != 0) {
    LOG_ERR(TAG, "validate: SHA-256 mismatch");
    mbedtls_sha256_free(&shaCtx);
    return Result::BAD_SHA;
  }
  mbedtls_sha256_free(&shaCtx);
  // computed == stored == SHA-256 over body bytes [0, fileSize - 32). This is
  // what flashFromSdPath re-computes from the staged bytes to close the SD
  // TOCTOU window.
  std::memcpy(out_sha, computed, BL_SHA_TRAILER);
  return Result::OK;
}

Result flashFromSdPath(const char* sdPath, ProgressCb onProgress, void* ctx) {
  // Refuse on secure-boot / flash-encryption devices: we don't re-sign or
  // re-encrypt, so the write would brick the device on next boot.
  if (esp_secure_boot_enabled()) {
    LOG_ERR(TAG, "secure boot enabled; refusing bootloader flash");
    return Result::SECURE_BOOT_ENABLED;
  }
  if (esp_flash_encryption_enabled()) {
    LOG_ERR(TAG, "flash encryption enabled; refusing bootloader flash");
    return Result::FLASH_ENCRYPTION_ENABLED;
  }
  // Our partitions.csv has no factory slot, so this branch only fires under a
  // custom PT. With a factory slot in play, esp_ota_get_next_update_partition
  // would hand us an ota slot the user may be relying on for their app.
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (!running || (running->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_0 &&
                   running->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_1)) {
    LOG_ERR(TAG, "running partition is not ota_0/ota_1; refusing bootloader flash");
    return Result::UNSUPPORTED_PT_LAYOUT;
  }

  uint8_t validatedSha[BL_SHA_TRAILER];
  {
    const Result vr = validateBootloaderFile(sdPath, validatedSha);
    if (vr != Result::OK) return vr;
  }

  HalFile file;
  if (!Storage.openFileForRead(TAG, sdPath, file) || !file) {
    LOG_ERR(TAG, "open failed: %s", sdPath);
    return Result::OPEN_FAIL;
  }
  const size_t fileSize = file.fileSize();
  const size_t bodyEnd  = fileSize - BL_SHA_TRAILER;  // validate ensured this

  const esp_partition_t* staging = esp_ota_get_next_update_partition(nullptr);
  if (!staging) {
    LOG_ERR(TAG, "no staging partition");
    return Result::NO_PARTITION;
  }

  const esp_partition_t* primary = nullptr;
  if (esp_partition_register_external(nullptr, BL_REGION_OFFSET, BL_REGION_SIZE,
                                       "PrimaryBTLDR",
                                       ESP_PARTITION_TYPE_BOOTLOADER,
                                       ESP_PARTITION_SUBTYPE_BOOTLOADER_PRIMARY,
                                       &primary) != ESP_OK || !primary) {
    LOG_ERR(TAG, "register_external failed");
    return Result::REGISTER_FAIL;
  }

  LOG_INF(TAG, "src=%s size=%u staging=%s@0x%x target=0x%x size=%u",
          sdPath, static_cast<unsigned>(fileSize),
          staging->label, static_cast<unsigned>(staging->address),
          static_cast<unsigned>(primary->address), static_cast<unsigned>(primary->size));

  auto buf = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[BL_CHUNK]);
  if (!buf) {
    esp_partition_deregister_external(primary);
    return Result::OOM;
  }

  // Hash [0, bodyEnd) only. The trailer IS SHA([0, bodyEnd)); feeding it
  // back into shaCtx would mangle the value validate gave us. Trailer still
  // gets written to staging so the image stays byte-identical.
  mbedtls_sha256_context shaCtx;
  mbedtls_sha256_init(&shaCtx);
  mbedtls_sha256_starts(&shaCtx, /*is224=*/0);

  size_t streamPos  = 0;
  size_t erasedUpto = 0;
  while (streamPos < fileSize) {
    if (streamPos >= erasedUpto) {
      size_t eraseLen = std::min<size_t>(BL_FLASH_BLOCK, staging->size - streamPos);
      eraseLen = (eraseLen + BL_FLASH_SECTOR - 1) & ~(BL_FLASH_SECTOR - 1);
      eraseLen = std::min<size_t>(eraseLen, staging->size - streamPos);
      if (esp_partition_erase_range(staging, streamPos, eraseLen) != ESP_OK) {
        LOG_ERR(TAG, "staging erase @%u failed", static_cast<unsigned>(streamPos));
        mbedtls_sha256_free(&shaCtx);
        esp_partition_deregister_external(primary);
        return Result::ERASE_FAIL;
      }
      erasedUpto = streamPos + eraseLen;
    }

    const size_t want = std::min<size_t>(BL_CHUNK, fileSize - streamPos);
    const int got = file.read(buf.get(), want);
    if (got <= 0 || static_cast<size_t>(got) != want) {
      LOG_ERR(TAG, "read @%u: got=%d want=%u",
              static_cast<unsigned>(streamPos), got, static_cast<unsigned>(want));
      mbedtls_sha256_free(&shaCtx);
      esp_partition_deregister_external(primary);
      return Result::READ_FAIL;
    }
    // Hash only the body portion of this chunk; the last 32 bytes of the
    // file are the trailer and aren't covered by validatedSha.
    if (streamPos < bodyEnd) {
      const size_t hashLen = std::min<size_t>(want, bodyEnd - streamPos);
      mbedtls_sha256_update(&shaCtx, buf.get(), hashLen);
    }
    if (esp_partition_write(staging, streamPos, buf.get(), want) != ESP_OK) {
      LOG_ERR(TAG, "staging write @%u failed", static_cast<unsigned>(streamPos));
      mbedtls_sha256_free(&shaCtx);
      esp_partition_deregister_external(primary);
      return Result::WRITE_FAIL;
    }
    streamPos += want;
    if (onProgress) onProgress(streamPos, fileSize, ctx);
    delay(1);
  }

  uint8_t streamSha[BL_SHA_TRAILER];
  mbedtls_sha256_finish(&shaCtx, streamSha);
  mbedtls_sha256_free(&shaCtx);

  // TOCTOU gate: bytes that landed in staging must hash to the same SHA the
  // validate pass saw. Mismatch means the SD card returned different bytes
  // between reads. Abort BEFORE touching the bootloader region.
  if (std::memcmp(validatedSha, streamSha, BL_SHA_TRAILER) != 0) {
    LOG_ERR(TAG, "staging SHA != validate SHA (SD TOCTOU)");
    esp_partition_deregister_external(primary);
    return Result::STAGING_TOCTOU;
  }

  // Bricking step. From here until verify completes, power loss leaves the
  // bootloader region indeterminate. The IDF's DANGEROUS_WRITE guard aborts
  // esp_partition_write below PARTITION_TABLE_OFFSET; libbootloader_support's
  // primitives plus the --wrap on esp_partition_main_flash_region_safe
  // (gated by g_allow_bootloader_write) bypass it.
  LOG_INF(TAG, "erasing bootloader region (0x0..0x%x)", static_cast<unsigned>(BL_REGION_SIZE));
  g_allow_bootloader_write = true;
  if (bootloader_flash_erase_range(BL_REGION_OFFSET, BL_REGION_SIZE) != ESP_OK) {
    LOG_ERR(TAG, "bootloader erase failed");
    g_allow_bootloader_write = false;
    esp_partition_deregister_external(primary);
    return Result::COPY_FAIL;
  }
  {
    size_t pos = 0;
    while (pos < fileSize) {
      const size_t want = std::min<size_t>(BL_CHUNK, fileSize - pos);
      if (esp_partition_read(staging, pos, buf.get(), want) != ESP_OK) {
        LOG_ERR(TAG, "staging read @%u failed", static_cast<unsigned>(pos));
        g_allow_bootloader_write = false;
        esp_partition_deregister_external(primary);
        return Result::COPY_FAIL;
      }
      if (bootloader_flash_write(BL_REGION_OFFSET + pos, buf.get(), want, false) != ESP_OK) {
        LOG_ERR(TAG, "bootloader write @%u failed", static_cast<unsigned>(pos));
        g_allow_bootloader_write = false;
        esp_partition_deregister_external(primary);
        return Result::COPY_FAIL;
      }
      pos += want;
    }
  }
  g_allow_bootloader_write = false;
  if (onProgress) onProgress(fileSize, fileSize, ctx);

  // Read-back verify: bootloader-region body bytes must hash to the same SHA
  // the validate pass and the staging pass both saw.
  mbedtls_sha256_context verifyCtx;
  mbedtls_sha256_init(&verifyCtx);
  mbedtls_sha256_starts(&verifyCtx, /*is224=*/0);
  const Result hr = hashPartitionRange(primary, 0, bodyEnd, &verifyCtx, buf.get());
  if (hr != Result::OK) {
    mbedtls_sha256_free(&verifyCtx);
    esp_partition_deregister_external(primary);
    return Result::VERIFY_FAIL;
  }
  uint8_t verifySha[BL_SHA_TRAILER];
  mbedtls_sha256_finish(&verifyCtx, verifySha);
  mbedtls_sha256_free(&verifyCtx);
  if (std::memcmp(validatedSha, verifySha, BL_SHA_TRAILER) != 0) {
    LOG_ERR(TAG, "read-back SHA mismatch");
    esp_partition_deregister_external(primary);
    return Result::VERIFY_FAIL;
  }

  // Poison staging's first sector so any later rollback flipping otadata at
  // this slot fails fast at the bootloader stage instead of jumping into
  // bootloader bytes as if they were an app.
  if (esp_partition_erase_range(staging, 0, BL_FLASH_SECTOR) != ESP_OK) {
    LOG_ERR(TAG, "staging poison failed (non-fatal)");
  }

  esp_partition_deregister_external(primary);
  LOG_INF(TAG, "bootloader flash complete");
  return Result::OK;
}

}  // namespace bootloader_flash
