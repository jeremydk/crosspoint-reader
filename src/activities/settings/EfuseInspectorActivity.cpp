#include "EfuseInspectorActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <cstdio>
#include <string>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

void EfuseInspectorActivity::onEnter() {
  Activity::onEnter();
  scanResult = efuse_inoculation::scan();
  // TEMP read-only build: action row disabled, start at top of field list.
  selectedIndex = 0;
  state = State::Inspecting;
  requestUpdate();
}

void EfuseInspectorActivity::onExit() { Activity::onExit(); }

void EfuseInspectorActivity::doInoculate() {
  {
    RenderLock lock(*this);
    state = State::Running;
  }
  requestUpdateAndWait();
  inoculateResult = efuse_inoculation::inoculate();
  scanResult = efuse_inoculation::scan();  // refresh so Back from Done returns to an updated list
  state = State::Done;
  requestUpdate();
}

void EfuseInspectorActivity::loop() {
  if (state == State::Inspecting) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
      return;
    }
    // TEMP read-only build: Confirm is a no-op; inoculate path is disabled.
    // if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    //   if (selectedIndex == actionRowIndex()) {
    //     state = State::WarnConfirm;
    //     requestUpdate();
    //   }
    //   return;
    // }
    buttonNavigator.onNextRelease([this] {
      selectedIndex = ButtonNavigator::nextIndex(selectedIndex, rowCount());
      requestUpdate();
    });
    buttonNavigator.onPreviousRelease([this] {
      selectedIndex = ButtonNavigator::previousIndex(selectedIndex, rowCount());
      requestUpdate();
    });
    buttonNavigator.onNextContinuous([this] {
      selectedIndex = ButtonNavigator::nextIndex(selectedIndex, rowCount());
      requestUpdate();
    });
    buttonNavigator.onPreviousContinuous([this] {
      selectedIndex = ButtonNavigator::previousIndex(selectedIndex, rowCount());
      requestUpdate();
    });
    return;
  }

  if (state == State::WarnConfirm) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      doInoculate();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      state = State::Inspecting;
      requestUpdate();
    }
    return;
  }

  if (state == State::Done) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      state = State::Inspecting;
      requestUpdate();
    }
    return;
  }
}

void EfuseInspectorActivity::render(RenderLock&&) {
  switch (state) {
    case State::Inspecting:
      renderInspecting();
      break;
    case State::WarnConfirm:
      renderWarning();
      break;
    case State::Running:
      renderRunning();
      break;
    case State::Done:
      renderDone();
      break;
  }
}

void EfuseInspectorActivity::renderInspecting() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  char subtitle[48];
  snprintf(subtitle, sizeof(subtitle), "%zu locked / %zu non-default", scanResult.alreadyProtected,
           scanResult.nonDefault);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_EFUSE_INSPECTOR),
                 subtitle);

  const int listTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int listHeight = pageHeight - listTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  // TEMP read-only build: action row hidden, list shows fields only.
  const int fieldCount = static_cast<int>(scanResult.fields.size());
  GUI.drawList(
      renderer, Rect{0, listTop, pageWidth, listHeight}, fieldCount, selectedIndex,
      [this](int i) -> std::string { return std::string(scanResult.fields[i].name); }, nullptr, nullptr,
      [this](int i) -> std::string {
        const auto& f = scanResult.fields[i];
        std::string v = f.state;
        v += " · ";
        v += f.writeProtected ? tr(STR_EFUSE_PROTECTED) : tr(STR_EFUSE_UNPROTECTED);
        return v;
      },
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void EfuseInspectorActivity::renderWarning() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_EFUSE_INOC_TITLE));

  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 60, tr(STR_EFUSE_INOC_WARN_1), true);
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 30, tr(STR_EFUSE_INOC_WARN_2), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, tr(STR_EFUSE_INOC_WARN_3), true);
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 30, tr(STR_EFUSE_INOC_WARN_4), true);

  const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_EFUSE_INOCULATE_BTN), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void EfuseInspectorActivity::renderRunning() {
  const auto pageHeight = renderer.getScreenHeight();
  renderer.clearScreen();
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_EFUSE_INOC_RUNNING));
  renderer.displayBuffer();
}

void EfuseInspectorActivity::renderDone() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_EFUSE_INOC_TITLE));

  const char* heading = inoculateResult.ok() ? tr(STR_EFUSE_INOC_DONE) : tr(STR_EFUSE_INOC_PARTIAL);
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 60, heading, true, EpdFontFamily::BOLD);

  char line[96];
  snprintf(line, sizeof(line), "newly=%zu  already=%zu  bad=%zu", inoculateResult.newly, inoculateResult.already,
           inoculateResult.badState);
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 30, line);

  snprintf(line, sizeof(line), "write_fail=%zu  validate_fail=%zu", inoculateResult.writeFail,
           inoculateResult.validateFail);
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, line);

  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 30, tr(STR_EFUSE_AUDIT_PATH));

  // Forensic stamp on-screen so the user can read it off without pulling the SD.
  char mac[18];
  efuse_inoculation::formatDefaultMac(mac, sizeof(mac));
  char idLine[96];
  snprintf(idLine, sizeof(idLine), "%s · %s", CROSSPOINT_VERSION, mac);
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 60, idLine);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
