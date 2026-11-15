#include "CrashActivity.h"

#include <GfxRenderer.h>
#include <cstring>
#include <HalSystem.h>
#include <I18n.h>

#include "components/UITheme.h"
#include "fontIds.h"

void CrashActivity::onEnter() {
  Activity::onEnter();

  panicMessage = HalSystem::getPanicInfo(false);
  if (panicMessage.empty()) {
    panicMessage = tr(STR_CRASH_NO_REASON);
  }
  // Read this BEFORE clearPanic(), which drops the RTC state it comes from.
  const char* faulted = HalSystem::panickedVersion();
  if (faulted[0] != '\0' && strcmp(faulted, CROSSPOINT_VERSION) != 0) {
    crashedVersion = faulted;
  }
  HalSystem::clearPanic();

  requestUpdateAndWait();
}

void CrashActivity::loop() {
  int x = 0;
  int y = 0;
  if (mappedInput.wasPressed(MappedInputManager::Button::Back) || mappedInput.wasScreenTapped(x, y)) {
    finish();
  }
}

void CrashActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto contentWidth = pageWidth - 2 * metrics.contentSidePadding;
  const auto x = metrics.contentSidePadding;
  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_CRASH_TITLE));

  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  auto descLines = renderer.wrappedText(UI_10_FONT_ID, tr(STR_CRASH_DESCRIPTION), contentWidth, 10);
  for (const auto& line : descLines) {
    renderer.drawText(UI_10_FONT_ID, x, y, line.c_str());
    y += lineHeight;
  }

  y += metrics.verticalSpacing * 2;
  renderer.drawText(UI_10_FONT_ID, x, y, tr(STR_CRASH_REASON));
  y += lineHeight + metrics.verticalSpacing;

  auto panicLines = renderer.wrappedText(UI_10_FONT_ID, panicMessage.c_str(), contentWidth, 5);
  for (const auto& line : panicLines) {
    renderer.drawText(UI_10_FONT_ID, x, y, line.c_str());
    y += lineHeight;
  }

  // Only when the two differ. Printing "crashed in X, running X" on every
  // crash would be noise, and noise on a screen shown once per fault is how a
  // real warning stops being read.
  if (!crashedVersion.empty()) {
    y += metrics.verticalSpacing * 2;
    auto warnLines = renderer.wrappedText(UI_10_FONT_ID, tr(STR_CRASH_VERSION_MISMATCH), contentWidth, 4);
    for (const auto& line : warnLines) {
      renderer.drawText(UI_10_FONT_ID, x, y, line.c_str());
      y += lineHeight;
    }
    y += metrics.verticalSpacing;
    char buf[96];
    snprintf(buf, sizeof(buf), "%s %s", tr(STR_CRASH_VERSION_CRASHED), crashedVersion.c_str());
    renderer.drawText(UI_10_FONT_ID, x, y, buf);
    y += lineHeight;
    snprintf(buf, sizeof(buf), "%s %s", tr(STR_CRASH_VERSION_RUNNING), CROSSPOINT_VERSION);
    renderer.drawText(UI_10_FONT_ID, x, y, buf);
    y += lineHeight;
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
