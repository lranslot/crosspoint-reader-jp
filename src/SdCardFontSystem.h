#pragma once

#include <SdCardFontManager.h>
#include <SdCardFontRegistry.h>

#include <atomic>

class GfxRenderer;

/// Facade that owns the SD card font registry, manager, and resolver logic.
/// Hides implementation details behind a single begin() + ensureLoaded() API.
class SdCardFontSystem {
 public:
  SdCardFontSystem() = default;
  SdCardFontSystem(const SdCardFontSystem&) = delete;
  SdCardFontSystem& operator=(const SdCardFontSystem&) = delete;
  /// Discover SD card fonts and load user's saved selection. Call once during setup.
  void begin(GfxRenderer& renderer);

  /// Ensure the correct SD font family is loaded for the current settings.
  /// Call before entering the reader or after settings change.
  /// Also re-discovers if the registry has been marked dirty (e.g. by web upload).
  void ensureLoaded(GfxRenderer& renderer);

  /// Resolve an SD card font ID from family name + reader point size.
  /// Returns 0 if not found. Used by CrossPointSettings::getReaderFontId().
  int resolveFontId(const char* familyName, uint8_t pointSize) const;

  /// Access the registry (e.g. for settings UI to enumerate available fonts).
  const SdCardFontRegistry& registry() const { return registry_; }

  /// Non-const access to the registry (for FontInstaller).
  SdCardFontRegistry& registry() { return registry_; }

  /// The family actually resident right now, "" when none is. This is not the
  /// same as SETTINGS.sdFontFamilyName, which is what the user chose: the two
  /// differ exactly when a load failed, which is what keepSelectionForNextTry()
  /// now leaves standing instead of erasing.
  const char* loadedFamilyName() const { return manager_.currentFamilyName().c_str(); }

  /// Mark the registry as needing re-discovery.
  /// Thread-safe: can be called from the web server task.
  void markRegistryDirty() { registryDirty_.store(true, std::memory_order_release); }

  /// If the registry is dirty, re-scan the SD card now and clear the flag.
  /// Used by the web UI so uploaded/deleted fonts appear in the list
  /// without waiting for the reader activity to run ensureLoaded().
  void refreshIfDirty() {
    if (registryDirty_.exchange(false, std::memory_order_acquire)) {
      registry_.discover();
    }
  }

 private:
  // Load the active SD family at the built-in UI point sizes and register each
  // as a size-matched CJK fallback for the corresponding UI font, so CJK book
  // titles/list rows render at the same size as the surrounding Latin UI text.
  // No-op when no SD family is loaded. Safe to call repeatedly (sizes already
  // loaded are reused).
  void setupUiFallbacks(GfxRenderer& renderer);

 public:
  /// Font id to draw ruby with, for a reader running at `readerPointSize`.
  ///
  /// Ruby is set at half the body size by convention, but a family ships whole
  /// point sizes and nothing on the card is half of 12 or 14. The installed size
  /// nearest half the reader size is used instead (findNearestSize, ties to the
  /// smaller), which on a 8/10/12/14/16/18 family is the 8 pt file for every
  /// reader size it offers: 64% of the body at 12 pt, 43% at 18 pt, rather than
  /// the 50% the notation assumes.
  ///
  /// Reuses an already-loaded size when there is one, so this is free whenever
  /// the choice lands on a UI fallback size. Returns 0 when no SD family is
  /// loaded or nothing could be read, in which case no ruby is drawn.
  int loadRubyFont(GfxRenderer& renderer, uint8_t readerPointSize);

 private:
  SdCardFontRegistry registry_;
  SdCardFontManager manager_;
  std::atomic<bool> registryDirty_{false};
};

// Global SD card font system instance (defined in main.cpp).
extern SdCardFontSystem sdFontSystem;
