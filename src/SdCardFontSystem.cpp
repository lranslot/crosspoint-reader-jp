#include "SdCardFontSystem.h"

#include <GfxRenderer.h>
#include <Logging.h>

#include <iterator>

#include "CrossPointSettings.h"
#include "ReaderFontSizes.h"
#include "fontIds.h"

namespace {

// Point the reader font size at a size the given family actually ships, and
// persist the change so the settings UI and the loaded font never disagree.
// Guarded by the value-change check: a no-op snap must not write SPIFFS.
void snapFontPointSizeTo(const uint8_t availablePointSize) {
  if (availablePointSize == 0 || availablePointSize == SETTINGS.fontPointSize) return;
  LOG_DBG("SDFS", "Font size %u unavailable, snapping to %u", SETTINGS.fontPointSize, availablePointSize);
  SETTINGS.fontPointSize = availablePointSize;
  SETTINGS.saveToFile();
}

// A font that cannot be read right now is not a font the user stopped wanting.
// Keeping the selection is what makes the failure recoverable: the next
// ensureLoaded() — on the next book opened, or the next boot — tries again, so
// re-seating the card is enough. Nothing is gained for this boot by dropping it,
// because getReaderFontId() already falls through to the built-in family while no
// SD family is loaded, and snaps the point size there without persisting it.
//
// Upstream clears the selection here AND saves it (CrossPointSettings.cpp
// clearSdFontFamily; see spec section 9.2). One failed SD read then permanently
// forgets the user's CJK font, and every Japanese glyph is a tofu box until they
// pick it again by hand. A transient read must not spend the user's settings.
// Same reasoning as PersistableStoreBase::readDocFromFile: a .cpfont that fails
// to load is usually a bad read, not a bad file, and the cost of believing the
// first failure is that the user's Japanese font goes away. Each pass is a real
// attempt; the first success returns.
bool loadFamilyWithRetry(SdCardFontManager& manager, const SdCardFontFamilyInfo& family, GfxRenderer& renderer,
                         const uint8_t pointSize) {
  static constexpr int ATTEMPTS = 3;
  for (int attempt = 0; attempt < ATTEMPTS; attempt++) {
    if (attempt > 0) delay(20);  // let the card settle, not a wait for readiness
    if (manager.loadFamily(family, renderer, pointSize)) return true;
    LOG_ERR("SDFS", "loadFamily failed [%d/%d]", attempt + 1, ATTEMPTS);
  }
  return false;
}

// The UI fallback sizes go through the same card and fail the same way. Losing
// one of these is quieter than losing the reader font — the UI falls back to a
// Latin-only built-in and the menus show tofu — but it is the same transient
// read, so it gets the same answer. Returns 0 when every attempt failed.
//
// Unlike the reader font, 0 here is also the ordinary answer for "this family
// has no file at that size", and that case now pays the full retry: two extra
// delays per missing size, ~40 ms, once per font load. Bounded and off the
// reading path, which is why it is not worth a way to tell the two apart.
int loadExtraSizeWithRetry(SdCardFontManager& manager, const SdCardFontFamilyInfo& family, GfxRenderer& renderer,
                           const uint8_t pointSize) {
  static constexpr int ATTEMPTS = 3;
  for (int attempt = 0; attempt < ATTEMPTS; attempt++) {
    if (attempt > 0) delay(20);  // let the card settle, not a wait for readiness
    const int id = manager.loadFamilyExtraSize(family, renderer, pointSize);
    if (id != 0) return id;
  }
  return 0;
}

void keepSelectionForNextTry(const char* why, const char* family) {
  LOG_ERR("SDFS", "SD font unavailable (%s): %s - built-in font for now, selection kept", why, family);
}

// Built-in UI fonts and their physical point sizes (at 150 DPI, matching the
// SD-font converter). Each is paired with a same-size SD fallback so CJK UI
// text matches the surrounding Latin. See SdCardFontSystem::setupUiFallbacks.
struct UiFontSize {
  int fontId;
  uint8_t pointSize;
};
constexpr UiFontSize kUiFontSizes[] = {
    {SMALL_FONT_ID, 8},
    {UI_10_FONT_ID, 10},
    {UI_12_FONT_ID, 12},
};

}  // namespace

void SdCardFontSystem::begin(GfxRenderer& renderer) {
  registry_.discover();

  // Register this system as the SD font ID resolver in settings.
  // Uses a static trampoline since CrossPointSettings stores a plain function pointer.
  SETTINGS.sdFontIdResolver = [](void* ctx, const char* familyName, uint8_t pointSize) -> int {
    return static_cast<SdCardFontSystem*>(ctx)->resolveFontId(familyName, pointSize);
  };
  SETTINGS.sdFontResolverCtx = this;

  // If user has a saved SD font selection, load it
  if (SETTINGS.sdFontFamilyName[0] != '\0') {
    const auto* family = registry_.findFamily(SETTINGS.sdFontFamilyName);
    if (family) {
      if (loadFamilyWithRetry(manager_, *family, renderer, SETTINGS.fontPointSize)) {
        snapFontPointSizeTo(manager_.currentPointSize());
        setupUiFallbacks(renderer);
        LOG_DBG("SDFS", "Loaded SD card font family: %s", SETTINGS.sdFontFamilyName);
      } else {
        keepSelectionForNextTry("read failed", SETTINGS.sdFontFamilyName);
      }
    } else {
      keepSelectionForNextTry("not on card", SETTINGS.sdFontFamilyName);
    }
  }

  LOG_DBG("SDFS", "SD font system ready (%d families discovered)", registry_.getFamilyCount());
}

void SdCardFontSystem::ensureLoaded(GfxRenderer& renderer) {
  // If the web server (or another task) installed/deleted fonts, re-discover.
  // Track whether we just re-discovered so we can force a reload below even
  // when the wanted family/size still maps to the same point size — the file
  // contents on disk may have changed (e.g. user re-uploaded a new build).
  const bool registryWasDirty = registryDirty_.exchange(false, std::memory_order_acquire);
  if (registryWasDirty) {
    LOG_DBG("SDFS", "Registry dirty — re-discovering fonts");
    registry_.discover();
  }

  const char* wantedFamily = SETTINGS.sdFontFamilyName;
  const std::string& currentFamily = manager_.currentFamilyName();

  if (wantedFamily[0] == '\0') {
    if (!currentFamily.empty()) {
      manager_.unloadAll(renderer);
    }
    // Back on a built-in family, which exists only at BUILTIN_READER_POINT_SIZES:
    // a size inherited from an SD family has to come back into that set.
    snapFontPointSizeTo(snapToNearestPointSize(BUILTIN_READER_POINT_SIZES, std::size(BUILTIN_READER_POINT_SIZES),
                                               SETTINGS.fontPointSize));
    return;
  }

  // Reload if family changed OR if the user-selected size maps to a
  // different file than what's currently loaded OR if the registry was
  // just rediscovered (file may have been replaced on disk).
  bool familyMatches = (currentFamily == wantedFamily);
  if (familyMatches) {
    const auto* family = registry_.findFamily(wantedFamily);
    if (!family) {
      manager_.unloadAll(renderer);
      keepSelectionForNextTry("disappeared", wantedFamily);
      return;
    }
    const auto* selected = family->findNearestSize(SETTINGS.fontPointSize);
    const uint8_t wantedPt = selected ? selected->pointSize : 0;
    // Snap before the early return: the wanted size can already be loaded while
    // the setting still names a size this family does not ship.
    snapFontPointSizeTo(wantedPt);
    if (!registryWasDirty && wantedPt == manager_.currentPointSize()) return;
    LOG_DBG("SDFS", "Reloading %s: size %u -> %u%s", wantedFamily, manager_.currentPointSize(), wantedPt,
            registryWasDirty ? " [registry dirty]" : "");
  }

  if (!currentFamily.empty()) {
    manager_.unloadAll(renderer);
  }

  const auto* family = registry_.findFamily(wantedFamily);
  if (family) {
    if (loadFamilyWithRetry(manager_, *family, renderer, SETTINGS.fontPointSize)) {
      snapFontPointSizeTo(manager_.currentPointSize());
      setupUiFallbacks(renderer);
      LOG_DBG("SDFS", "Loaded SD font family: %s", wantedFamily);
    } else {
      keepSelectionForNextTry("read failed", wantedFamily);
    }
  } else {
    keepSelectionForNextTry("not on card", wantedFamily);
  }
}

void SdCardFontSystem::setupUiFallbacks(GfxRenderer& renderer) {
  const std::string& familyName = manager_.currentFamilyName();
  if (familyName.empty()) return;  // no SD family loaded — nothing to fall back to

  const auto* family = registry_.findFamily(familyName);
  if (!family) return;

  // Probe the already-loaded reader-size font before paying for the UI sizes:
  // resolveTextFontId only redirects on CJK codepoints, so a Latin-only family
  // can never act as a fallback and its UI sizes would be dead weight in RAM.
  const auto readerIt = renderer.getFontMap().find(manager_.getFontId(familyName));
  if (readerIt == renderer.getFontMap().end()) return;
  // One representative codepoint per script: Han, Hiragana, Katakana, Hangul.
  static constexpr uint32_t kCjkProbes[] = {0x4E00, 0x3042, 0x30A2, 0xAC00};
  bool hasCjk = false;
  for (const uint32_t cp : kCjkProbes) {
    if (readerIt->second.hasCodepoint(cp)) {
      hasCjk = true;
      break;
    }
  }
  if (!hasCjk) {
    LOG_DBG("SDFS", "%s has no CJK coverage - skipping UI fallback sizes", familyName.c_str());
    return;
  }

  for (const auto& ui : kUiFontSizes) {
    const int sdFontId = loadExtraSizeWithRetry(manager_, *family, renderer, ui.pointSize);
    if (sdFontId != 0) {
      renderer.setFallbackFont(ui.fontId, sdFontId);
    } else {
      LOG_DBG("SDFS", "No %u pt SD glyphs for UI fallback in %s", ui.pointSize, familyName.c_str());
    }
  }
}

int SdCardFontSystem::loadRubyFont(GfxRenderer& renderer, const uint8_t readerPointSize) {
  const std::string& familyName = manager_.currentFamilyName();
  if (familyName.empty()) return 0;
  const auto* family = registry_.findFamily(familyName.c_str());
  if (!family) return 0;

  const uint8_t wanted = readerPointSize / 2;
  const auto* file = family->findNearestSize(wanted);
  if (!file) return 0;

  return loadExtraSizeWithRetry(manager_, *family, renderer, file->pointSize);
}

int SdCardFontSystem::resolveFontId(const char* familyName, uint8_t /*pointSize*/) const {
  // The manager holds exactly one reader-size font, already selected for
  // SETTINGS.fontPointSize, so the size argument is implicit — always return
  // that font's ID. ensureLoaded() must have run for the current settings first.
  return manager_.getFontId(familyName);
}
