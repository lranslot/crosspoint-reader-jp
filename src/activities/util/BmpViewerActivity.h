#pragma once

#include <functional>
#include <string>

#include "MappedInputManager.h"
#include "activities/Activity.h"

class BmpViewerActivity final : public Activity {
 public:
  BmpViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string filePath);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  // Confirm here is "set as sleep cover", which is the wrong thing to reach by
  // accident: the standby image changes, nothing says it happened, and undoing it
  // means picking another image. Under BY_SCREEN the power button steps to the
  // next image instead — there is a previous and a next, so it behaves like a
  // page turn. Handled in loop().
  bool powerButtonActsAsConfirm() const override { return false; }

 private:
  void loadSiblingImages();
  void doSetSleepCover();

  std::string filePath;
  std::vector<std::string> siblingImages;
  int currentImageIndex = -1;
};