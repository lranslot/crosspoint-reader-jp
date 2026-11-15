#pragma once
#include "activities/Activity.h"

class CrashActivity final : public Activity {
  std::string panicMessage;
  // Empty unless the panic happened in a different firmware than this one,
  // i.e. unless the reboot crossed an update. See HalSystem::panickedVersion().
  std::string crashedVersion;

 public:
  explicit CrashActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Crash", renderer, mappedInput) {}
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
