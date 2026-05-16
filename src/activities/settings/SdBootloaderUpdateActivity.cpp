#include "SdBootloaderUpdateActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include "MappedInputManager.h"
#include "activities/home/FileBrowserActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "flash/BootloaderFlasher.h"
#include "fontIds.h"

void SdBootloaderUpdateActivity::onEnter() {
  Activity::onEnter();
  LOG_INF("BLUP", "SdBootloaderUpdateActivity build=%s %s", __DATE__, __TIME__);
  state = State::PICKING;
  launchPicker();
}

void SdBootloaderUpdateActivity::launchPicker() {
  // PickFirmware mode shows any .bin; the validator catches "this is a
  // firmware.bin, not a bootloader" via the byte-32 desc-magic check.
  startActivityForResult(
      std::make_unique<FileBrowserActivity>(renderer, mappedInput, "/", FileBrowserActivity::Mode::PickFirmware),
      [this](const ActivityResult& result) { onPickerResult(result); });
}

void SdBootloaderUpdateActivity::onPickerResult(const ActivityResult& result) {
  if (result.isCancelled) {
    finish();
    return;
  }

  const auto* path = std::get_if<FilePathResult>(&result.data);
  if (!path) {
    LOG_ERR("BLUP", "Picker returned no path");
    finish();
    return;
  }
  bootloaderPath = path->path;
  LOG_DBG("BLUP", "Selected: %s", bootloaderPath.c_str());

  {
    RenderLock lock(*this);
    state = State::VALIDATING;
  }
  requestUpdateAndWait();

  if (!validateCandidate()) {
    RenderLock lock(*this);
    state = State::FAILED;
    requestUpdate();
    return;
  }

  promptConfirmation();
}

bool SdBootloaderUpdateActivity::validateCandidate() {
  HalFile file;
  if (!Storage.openFileForRead("BLUP", bootloaderPath.c_str(), file) || !file) {
    errorMessage = tr(STR_FIRMWARE_FILE_OPEN_FAILED);
    return false;
  }
  bootloaderSize = file.fileSize();

  uint8_t sha[bootloader_flash::SHA_LEN];
  const auto vr = bootloader_flash::validateBootloaderFile(bootloaderPath.c_str(), sha);
  if (vr != bootloader_flash::Result::OK) {
    LOG_ERR("BLUP", "validation failed: %s", bootloader_flash::resultName(vr));
    switch (vr) {
      case bootloader_flash::Result::TOO_LARGE:
        errorMessage = tr(STR_BOOTLOADER_TOO_LARGE);
        break;
      case bootloader_flash::Result::TOO_SMALL:
        errorMessage = tr(STR_BOOTLOADER_TOO_SMALL);
        break;
      case bootloader_flash::Result::NOT_BOOTLOADER:
        errorMessage = tr(STR_BOOTLOADER_NOT_RECOGNIZED);
        break;
      case bootloader_flash::Result::NOT_THIS_CHIP:
        errorMessage = tr(STR_BOOTLOADER_WRONG_CHIP);
        break;
      default:
        errorMessage = tr(STR_INVALID_BOOTLOADER);
        break;
    }
    return false;
  }
  return true;
}

void SdBootloaderUpdateActivity::promptConfirmation() {
  {
    RenderLock lock(*this);
    state = State::CONFIRMING;
  }
  std::string heading = tr(STR_BOOTLOADER_UPDATE_PROMPT);
  std::string body = bootloaderPath;
  const auto pos = body.find_last_of('/');
  if (pos != std::string::npos) body = body.substr(pos + 1);

  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, heading, body),
                         [this](const ActivityResult& result) { onConfirmationResult(result); });
}

void SdBootloaderUpdateActivity::onConfirmationResult(const ActivityResult& result) {
  if (result.isCancelled) {
    finish();
    return;
  }

  {
    RenderLock lock(*this);
    state = State::UPDATING;
    writtenBytes = 0;
    lastRenderedPercent = 101;
  }
  requestUpdateAndWait();
  performFlash();
}

void SdBootloaderUpdateActivity::performFlash() {
  LOG_INF("BLUP", "flash: %s (%u bytes)", bootloaderPath.c_str(), static_cast<unsigned>(bootloaderSize));

  auto progressCb = +[](size_t written, size_t total, void* ctx) {
    auto* self = static_cast<SdBootloaderUpdateActivity*>(ctx);
    self->writtenBytes = written;
    self->bootloaderSize = total;
    self->requestUpdate(true);
  };

  const auto result = bootloader_flash::flashFromSdPath(bootloaderPath.c_str(), progressCb, this);
  if (result != bootloader_flash::Result::OK) {
    LOG_ERR("BLUP", "flash failed: %s", bootloader_flash::resultName(result));
    switch (result) {
      case bootloader_flash::Result::VERIFY_FAIL:
        errorMessage = tr(STR_BOOTLOADER_VERIFY_FAILED);
        break;
      case bootloader_flash::Result::STAGING_TOCTOU:
        errorMessage = tr(STR_BOOTLOADER_SD_TOCTOU);
        break;
      case bootloader_flash::Result::SECURE_BOOT_ENABLED:
      case bootloader_flash::Result::FLASH_ENCRYPTION_ENABLED:
        errorMessage = tr(STR_BOOTLOADER_LOCKED_DEVICE);
        break;
      case bootloader_flash::Result::UNSUPPORTED_PT_LAYOUT:
        errorMessage = tr(STR_BOOTLOADER_BAD_PT_LAYOUT);
        break;
      default:
        errorMessage = tr(STR_BOOTLOADER_WRITE_FAILED);
        break;
    }
    RenderLock lock(*this);
    state = State::FAILED;
    requestUpdate();
    return;
  }

  LOG_INF("BLUP", "bootloader flash complete, restarting");
  {
    RenderLock lock(*this);
    state = State::SUCCESS;
  }
  requestUpdateAndWait();
  delay(1500);
  // Clear the EPD before handing off to the new bootloader. Otherwise the
  // "Update complete" frame sticks until the next app render, which is several
  // seconds of confusing stale UI after the reboot.
  renderer.clearScreen();
  renderer.displayBuffer();
  ESP.restart();
}

void SdBootloaderUpdateActivity::loop() {
  if (state == State::FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      finish();
    }
  }
}

void SdBootloaderUpdateActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 tr(STR_SD_BOOTLOADER_UPDATE));

  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - lineHeight) / 2;

  if (state == State::VALIDATING) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_VALIDATING_BOOTLOADER));
  } else if (state == State::UPDATING) {
    const unsigned int pct = bootloaderSize > 0 ? static_cast<unsigned int>((writtenBytes * 100) / bootloaderSize) : 0;
    if (pct == lastRenderedPercent) return;
    lastRenderedPercent = pct;

    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_UPDATING), true, EpdFontFamily::BOLD);
    int y = top + lineHeight + metrics.verticalSpacing;
    GUI.drawProgressBar(
        renderer,
        Rect{metrics.contentSidePadding, y, pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight},
        static_cast<int>(pct), 100);
    y += metrics.progressBarHeight + metrics.verticalSpacing;
    y += lineHeight + metrics.verticalSpacing;
    renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_FIRMWARE_UPDATE_DO_NOT_POWER_OFF));
  } else if (state == State::SUCCESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_UPDATE_COMPLETE), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, top + lineHeight + metrics.verticalSpacing,
                              tr(STR_BOOTLOADER_RESTART_HINT));
  } else if (state == State::FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_UPDATE_FAILED), true, EpdFontFamily::BOLD);
    if (!errorMessage.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, top + lineHeight + metrics.verticalSpacing, errorMessage.c_str());
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
