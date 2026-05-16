#pragma once

#include <string>

#include "activities/Activity.h"

// SD-card bootloader update. Picks a .bin, delegates validate + flash to
// bootloader_flash::flashFromSdPath, ESP.restart on success. Single
// confirmation prompt covers the whole flash; the copy from staging into
// the live bootloader region is the only bricking window, and the progress
// screen carries the DO-NOT-POWER-OFF label throughout.
class SdBootloaderUpdateActivity : public Activity {
 public:
  enum class State {
    PICKING,
    VALIDATING,
    CONFIRMING,
    UPDATING,
    SUCCESS,
    FAILED,
  };

  explicit SdBootloaderUpdateActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("SdBootloaderUpdate", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == State::UPDATING || state == State::VALIDATING; }
  bool skipLoopDelay() override { return state == State::UPDATING; }

 private:
  State state = State::PICKING;

  std::string bootloaderPath;
  size_t bootloaderSize = 0;
  size_t writtenBytes = 0;
  unsigned int lastRenderedPercent = 101;
  std::string errorMessage;

  void launchPicker();
  void onPickerResult(const ActivityResult& result);
  bool validateCandidate();
  void promptConfirmation();
  void onConfirmationResult(const ActivityResult& result);
  void performFlash();
};
