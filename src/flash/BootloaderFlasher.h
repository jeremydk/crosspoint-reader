#pragma once

#include <cstddef>
#include <cstdint>

// Flash a 2nd-stage bootloader image (crossboot) from SD into 0x0..0x8000.
// Stages through the inactive OTA app slot, TOCTOU-compares the staged
// bytes against a SHA captured at validate time, then writes the region
// via libbootloader_support primitives. Refuses early if secure boot or
// flash encryption is enabled (we don't re-sign or re-encrypt), or if the
// running partition isn't ota_0/ota_1 (staging slot may be load-bearing).

namespace bootloader_flash {

constexpr size_t SHA_LEN = 32;

enum class Result {
  OK,
  OPEN_FAIL,
  TOO_SMALL,
  TOO_LARGE,
  BAD_MAGIC,
  NOT_BOOTLOADER,             // byte[32] != 0x50, likely an app .bin
  NOT_THIS_CHIP,              // header[12] doesn't match this chip
  BAD_SEGMENTS,
  BAD_CHECKSUM,
  BAD_SHA,
  BAD_SIZE,
  NO_PARTITION,
  OOM,
  READ_FAIL,
  ERASE_FAIL,
  WRITE_FAIL,
  REGISTER_FAIL,
  COPY_FAIL,                  // staging -> bootloader region (bricking window)
  VERIFY_FAIL,                // post-copy read-back SHA mismatch
  STAGING_TOCTOU,             // SD returned different bytes between validate and stage
  SECURE_BOOT_ENABLED,        // refuse: would brick a secure-boot device
  FLASH_ENCRYPTION_ENABLED,   // refuse: would brick a flash-encryption device
  UNSUPPORTED_PT_LAYOUT,      // refuse: not running from an OTA slot, can't safely pick staging
};

// Fires after each staging chunk and once after the final copy.
using ProgressCb = void (*)(size_t written, size_t total, void* ctx);

// On OK, fills out_sha with the SHA-256 of body bytes [0, fileSize - 32).
// Pass a SHA_LEN-byte buffer. Contents undefined on non-OK return.
Result validateBootloaderFile(const char* sdPath, uint8_t out_sha[SHA_LEN]);

// Caller restarts on OK.
Result flashFromSdPath(const char* sdPath, ProgressCb onProgress, void* ctx);

const char* resultName(Result r);

}  // namespace bootloader_flash
