#pragma once

#include "EfuseInoculation.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class EfuseInspectorActivity final : public Activity {
 public:
  explicit EfuseInspectorActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("EfuseInspector", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool skipLoopDelay() override { return true; }
  void render(RenderLock&&) override;

 private:
  enum class State { Inspecting, WarnConfirm, Running, Done };

  ButtonNavigator buttonNavigator;
  State state = State::Inspecting;
  int selectedIndex = 0;  // 0..N-1 = field rows, N = "Inoculate" action
  efuse_inoculation::ScanResult scanResult;
  efuse_inoculation::InoculateResult inoculateResult;

  int actionRowIndex() const { return static_cast<int>(scanResult.fields.size()); }
  // TEMP read-only build: action row hidden so navigation only spans the field list.
  int rowCount() const { return actionRowIndex(); }

  void doInoculate();
  void renderInspecting();
  void renderWarning();
  void renderRunning();
  void renderDone();
};
