#pragma once

#include <string>
#include <vector>

#include "FontInstaller.h"
#include "SdCardFont.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// JSON schema version of the fonts.json manifest. The canonical version for
// the build tooling lives in lib/EpdFont/scripts/cpfont_version.py. This
// firmware-side copy must be bumped manually when the firmware is updated to
// support a new manifest schema.
#define FONTS_MANIFEST_VERSION 1

#ifndef FONT_MANIFEST_URL
// Manifest + .cpfont assets are published by .github/workflows/release-fonts.yml
// to the crosspoint-fonts repo under the "sd-fonts-m<META>-b<BIN>" tag. The tag
// pattern must stay in sync with the workflow; it derives its version numbers
// from lib/EpdFont/scripts/cpfont_version.py.
#define FONT_MANIFEST_URL_STRINGIFY_INNER(x) #x
#define FONT_MANIFEST_URL_STRINGIFY(x) FONT_MANIFEST_URL_STRINGIFY_INNER(x)
#define FONT_MANIFEST_URL                                                                                           \
  "https://github.com/crosspoint-reader/crosspoint-fonts/releases/download/sd-fonts-m" FONT_MANIFEST_URL_STRINGIFY( \
      FONTS_MANIFEST_VERSION) "-b" FONT_MANIFEST_URL_STRINGIFY(CPFONT_VERSION) "/fonts.json"
#endif

class FontDownloadActivity : public Activity {
 public:
  explicit FontDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override {
    return state_ == LOADING_MANIFEST || state_ == DOWNLOADING ||
           // The download is synchronous and blocks the main loop until it
           // completes, so activityManager.preventAutoSleep() is never polled
           // during downloading.
           state_ == COMPLETE || state_ == ERROR;
  }
  bool skipLoopDelay() override { return true; }

 private:
  enum State {
    WIFI_SELECTION,
    LOADING_MANIFEST,
    FAMILY_LIST,
    DOWNLOADING,
    COMPLETE,
    ERROR,
  };

  struct ManifestFile {
    std::string name;
    size_t size = 0;
    uint32_t crc32 = 0;
  };

  struct ManifestFamily {
    std::string name;
    std::string description;
    // No styles vector. The manifest carries one per family and nothing in
    // this tree ever read it -- the only five lines that mentioned it were the
    // declaration and the code filling it. Upstream reached the same
    // conclusion and removed it in PR #3398. On the current release it cost 35
    // allocations (one reserve per family) to build something no screen,
    // installer or registry consulted.
    std::vector<ManifestFile> files;
    size_t totalSize = 0;
    bool installed = false;
    bool hasUpdate = false;
  };

  State state_ = WIFI_SELECTION;
  FontInstaller fontInstaller_;
  ButtonNavigator buttonNavigator_;

  // Manifest data
  std::string baseUrl_;
  std::vector<ManifestFamily> families_;
  int selectedIndex_ = 0;

  // Download progress
  size_t currentFileIndex_ = 0;
  size_t currentFileTotal_ = 0;
  size_t fileProgress_ = 0;
  size_t fileTotal_ = 0;
  int downloadingFamilyIndex_ = 0;
  std::string errorMessage_;
  bool cancelRequested_ = false;

  // Cooldown probe. Tests one specific hypothesis: that the contiguous heap a
  // burst of downloads loses is held by lwip's TIME-WAIT PCBs, which this build
  // keeps for CONFIG_LWIP_TCP_MSL = 60,000 ms. Every download in burn 14
  // happened inside one such minute, so nothing had been observed after one
  // elapsed. If the largest block recovers at the 65s mark and not before,
  // TIME-WAIT is confirmed; if it never recovers, something is leaked and the
  // hypothesis is wrong. Either answer is worth a burn.
  //
  // Samples are taken from loop() rather than a task: no stack to size, and the
  // screen is idle anyway once the downloads have finished.
  uint32_t cooldownStartedAt_ = 0;  // 0 = not armed
  uint8_t cooldownStep_ = 0;
  void serviceCooldownProbe();

  void onWifiSelectionComplete(bool success);
  bool fetchAndParseManifest();
  void downloadFamily(ManifestFamily& family);
  void downloadAll();
  void updateAll();
  static bool computeFileCrc32(const char* path, uint32_t& outCrc);
  bool showDownloadAllRow() const;
  bool showUpdateAllRow() const;
  int specialRowCount() const;
  bool isDownloadAllRow(int index) const;
  bool isUpdateAllRow(int index) const;
  bool isSelectedFamilyDeletable() const;
  void promptDeleteSelectedFamily();
  void onDeleteConfirmationResult(const ActivityResult& result);
  int familyIndexFromList(int listIndex) const { return listIndex - specialRowCount(); }
  int listItemCount() const;
  size_t totalDownloadSize() const;
  size_t totalUpdateSize() const;
  static std::string formatSize(size_t bytes);
};
