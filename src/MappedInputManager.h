#pragma once

#include <HalGPIO.h>

class GfxRenderer;

class MappedInputManager {
 public:
  enum class Button {
    Back,
    Confirm,
    Left,
    Right,
    Up,
    Down,
    Power,
    PageBack,
    PageForward,
    NavNext,
    NavPrevious,
    ScreenLeft,
    ScreenRight,
    ScreenUp,
    ScreenDown
  };
  enum class SwipeDir { None, Left, Right, Up, Down };

  struct Labels {
    const char* btn1;
    const char* btn2;
    const char* btn3;
    const char* btn4;
  };

  MappedInputManager(HalGPIO& gpio, const GfxRenderer& renderer) : gpio(gpio), renderer(renderer) {}

  // Samples the hardware and latches any edge it reports.
  //
  // InputManager rebuilds pressedEvents/releasedEvents from scratch on every
  // update() and holds nothing, so an edge that happens between two calls is
  // simply never seen. That is fine at a few milliseconds apart, but a background
  // chapter build blinds the loop for up to a second at a time, and a press and
  // release completing inside that window used to vanish entirely. Latching here
  // means the edge survives until something asks for it.
  void update() const;
  // True when the hardware reports the edge this frame, or when one was latched
  // earlier and has not been consumed yet. Consuming clears the latch.
  bool wasPressed(Button button) const;
  bool wasReleased(Button button) const;
  // Same answer as wasReleased(), but leaves the latch in place.
  //
  // For guards only — "did this other button also go up, so skip the action" —
  // where the edge belongs to whatever handles it next and must still be there
  // when it looks. Ordinary input handling must use wasReleased(): an edge that is
  // tested but never consumed would fire again on the following frame.
  bool wasReleasedPeek(Button button) const;
  // Deliberately not latched: this reports whether the button is held *now*, and
  // answering from a stale edge would be a different question.
  bool isPressed(Button button) const;
  // Drops every latch. Called when the screen changes: a press aimed at the
  // previous activity must not fire on the one that replaced it.
  void clearLatches() const;
  bool hasTouch() const;
  bool wasScreenTapped(int& x, int& y) const;
  bool wasScreenTouchDown(int& x, int& y) const;
  bool isScreenTouchHeld(int& x, int& y) const;
  bool wasTapInRect(int x, int y, int width, int height) const;
  bool wasListItemTapped(int& index, int itemCount, int selectedIndex, int listTop, int listHeight,
                         bool hasSubtitle) const;
  bool wasListItemTouchedDown(int& index, int itemCount, int selectedIndex, int listTop, int listHeight,
                              bool hasSubtitle) const;

  // Combined touch interaction for a band of equal rows with caller-supplied
  // geometry — the shared hit-test for lists the theme helpers above do not
  // cover (custom row heights, option prompts, menus). Down = a held
  // tap-candidate is on a row (update the selection highlight); Tap = a tap
  // released on one (activate). rowHeight limits the hit to the top rowHeight
  // px of each step (0 = the full step, no gap band).
  enum class RowTouch : uint8_t { None, Down, Tap };
  RowTouch rowTouch(int& row, int top, int rowStep, int rowCount, int xStart = 0, int xEnd = INT32_MAX,
                    int rowHeight = 0) const;
  // Horizontal variant for side-by-side button pairs (confirmation prompts).
  RowTouch colTouch(int& col, int left, int colStep, int colCount, int yStart, int yEnd, int colWidth = 0) const;

  SwipeDir wasSwipe() const;
  bool wasHomeGesture() const;
  bool wasMenuGesture() const;
  bool wasAnyPressed() const;
  bool wasAnyReleased() const;
  unsigned long getHeldTime() const;
  const GfxRenderer& getRenderer() const { return renderer; }
  Labels mapLabels(const char* back, const char* confirm, const char* previous, const char* next) const;
  // Maps four screen-direction labels onto the two physical front-button roles
  // using the same live-orientation transform as ScreenLeft/Right/Up/Down.
  Labels mapDirectionalLabels(const char* back, const char* confirm, const char* left, const char* right,
                              const char* up, const char* down) const;
  // Returns the raw front button index that was pressed this frame (or -1 if none).
  int getPressedFrontButton() const;

  // True when the control axis is flipped relative to the physical buttons: the user opted into
  // orientation-following front buttons AND the screen is *currently rendered* rotated (INVERTED /
  // LANDSCAPE_CCW). Keyed on the live renderer orientation rather than the persisted reader setting,
  // so portrait UI (home, settings) never swaps while the reader and its menus do.
  [[nodiscard]] bool isNavDirectionSwapped() const;

 private:
  HalGPIO& gpio;
  // Logical-to-physical button mapping depends on what the user is actually looking at: when the
  // screen is rendered rotated, the directional buttons must flip to match. The renderer is the only
  // authority on the *live* orientation (the reader rotates it and restores portrait on exit), so we
  // read it here instead of CrossPointSettings.orientation, which is just the persisted reader
  // preference and stays "rotated" even while portrait UI like home/settings is on screen.
  const GfxRenderer& renderer;

  Button mapScreenDirection(Button button) const;
  Labels mapFrontLabels(const char* back, const char* confirm, const char* left, const char* right) const;
  bool mapButton(Button button, bool (HalGPIO::*fn)(uint8_t) const) const;
  // The physical pin a logical button resolves to, or false when it maps to none
  // (side buttons disabled) or to more than one (the Nav* pair). Shares its rules
  // with mapButton() so the two can never disagree.
  bool resolvePin(Button button, uint8_t& pin) const;
  // Shared body of wasPressed/wasReleased/wasReleasedPeek: composite buttons
  // decompose, single buttons check the live edge then the latch. `consume`
  // clears the latch on a hit; peeking leaves it for the next reader.
  bool consumeEdge(Button button, bool pressed, bool consume) const;
  bool wasBackGesture() const;
  // Fetch the pending swipe (if any) and map both endpoints to logical screen coords
  bool decodeSwipe(int& sx, int& sy, int& ex, int& ey) const;
  bool listItemFromPoint(int x, int y, int& index, int itemCount, int selectedIndex, int listTop, int listHeight,
                         bool hasSubtitle) const;
  void rememberTouchHeldTime() const;

  mutable bool touchHeldOverrideValid = false;
  mutable unsigned long touchHeldOverrideMs = 0;
  mutable unsigned long touchHeldOverrideAt = 0;

  // Latches are kept per physical pin, not per logical Button: the same pin backs
  // different logical buttons depending on the remap and side-layout settings, so
  // keying on the logical side would double-count one press and lose another.
  static constexpr uint8_t LATCH_PIN_COUNT = HalGPIO::BTN_POWER + 1;
  // Long enough to bridge the worst measured blind window (957 ms while a chapter
  // builds), short enough that a press cannot outlive the screen it was meant for.
  static constexpr unsigned long LATCH_MAX_AGE_MS = 1500;

  mutable bool pressLatched[LATCH_PIN_COUNT] = {};
  mutable bool releaseLatched[LATCH_PIN_COUNT] = {};
  mutable unsigned long pressLatchedAt[LATCH_PIN_COUNT] = {};
  mutable unsigned long releaseLatchedAt[LATCH_PIN_COUNT] = {};
};
