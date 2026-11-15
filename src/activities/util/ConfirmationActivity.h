#pragma once
#include <functional>
#include <string>

#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "fontIds.h"

class ConfirmationActivity : public Activity {
 private:
  // Input data
  std::string heading;
  std::string body;

  const int margin = 20;
  const int spacing = 30;
  const int fontId = UI_10_FONT_ID;

  std::string safeHeading;
  std::string safeBody;
  const char* cancelLabel_ = nullptr;
  const char* confirmLabel_ = nullptr;
  OptionPopup confirmPopup;
  int startY = 0;
  int lineHeight = 0;

 public:
  // `cancelLabel` / `confirmLabel` default to the generic pair. Pass them when
  // the generic words do not say what will happen: "Cancel / Confirm" under a
  // heading that is itself a question leaves the reader to work out which button
  // does what, and a firmware write is not a good place to guess.
  ConfirmationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& heading,
                       const std::string& body, const char* cancelLabel = nullptr, const char* confirmLabel = nullptr);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&& lock) override;
};