#pragma once

// User-initiated eFuse inoculation against vendor lockdown.
//
// scan() inspects the defensive eFuse field set without writing anything.
// inoculate() burns the WR_DIS bit for every field that isn't already
// locked, then writes /.crosspoint/efuse_inoculation_audit.txt. Both are
// safe to call multiple times — the eFuse writes are idempotent.

#include <cstddef>
#include <vector>

namespace efuse_inoculation {

struct FieldStatus {
  const char* name;     // e.g. "DIS_USB_JTAG" or "BLOCK_KEY0"
  char state[48];       // "default", "nondefault_0xNN", "(key block)", "read_err_<long-name>"
  bool nonDefault;      // true if the field is already burned non-zero
  bool writeProtected;  // true if WR_DIS is already set for this field
};

struct ScanResult {
  std::vector<FieldStatus> fields;  // BLOCK0 fields followed by KEY blocks
  size_t alreadyProtected = 0;
  size_t nonDefault = 0;
};

struct InoculateResult {
  size_t newly = 0;
  size_t already = 0;
  size_t badState = 0;
  size_t writeFail = 0;
  size_t validateFail = 0;
  bool ok() const { return writeFail == 0 && validateFail == 0; }
};

ScanResult scan();
InoculateResult inoculate();  // Writes the audit file as a side effect.

// Renders the device's default eFuse MAC as "XX:XX:XX:XX:XX:XX" into `out`.
void formatDefaultMac(char* out, size_t outSize);

}  // namespace efuse_inoculation
