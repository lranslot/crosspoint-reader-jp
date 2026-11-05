#include "PersistableStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

#include <cstring>
#include <limits>

std::atomic<bool> PersistableStoreBase::saveFailed_{false};

void PersistableStoreBase::noteSaveFailure() { saveFailed_.store(true, std::memory_order_release); }

bool PersistableStoreBase::consumeSaveFailure() { return saveFailed_.exchange(false, std::memory_order_acquire); }

bool PersistableStoreBase::writeDocToFile(const char* path, const JsonDocument& doc) {
  Storage.mkdir("/.crosspoint");
  String json;
  serializeJson(doc, json);

  // Written beside the real file and moved into place, because
  // SDCardManager::writeFile() removes the existing file BEFORE opening the new
  // one. A failure partway through that leaves either nothing or a truncated
  // file, and both come back as a loss of every setting: nothing reads as
  // ReadFailure::Absent (defaults, and saving still allowed, so the defaults
  // stick), truncated reads as Unreadable. Either way the user's settings are
  // gone through the write path — the mirror image of the read bug this was
  // found next to. See spec section 9.2-15.
  //
  // The temporary keeps the good bytes until the rename succeeds, so an
  // interrupted save costs the new value, never the old file.
  std::string tmp(path);
  tmp += ".tmp";

  if (!Storage.writeFile(tmp.c_str(), json)) {
    LOG_ERR("PERSIST", "Failed to write %s", tmp.c_str());
    Storage.remove(tmp.c_str());  // never leave a partial temporary behind
    noteSaveFailure();
    return false;
  }

  // rename() needs the destination gone. From here to the rename is the only
  // window where the real file is absent, and the data is safe in the temporary
  // throughout it.
  if (Storage.exists(path) && !Storage.remove(path)) {
    LOG_ERR("PERSIST", "Failed to replace %s", path);
    noteSaveFailure();
    return false;
  }
  if (!Storage.rename(tmp.c_str(), path)) {
    LOG_ERR("PERSIST", "Wrote %s but could not move it into place — recover it by hand", tmp.c_str());
    noteSaveFailure();
    return false;
  }
  return true;
}

bool PersistableStoreBase::readDocFromFile(const char* path, JsonDocument& doc, ReadFailure* why) {
  if (why) *why = ReadFailure::None;

  // Each pass is a whole new attempt, and the first success returns — this is a
  // retry, not a timed wait for something to become ready. A fixed delay would
  // only work on a card slow by exactly that much; retrying works whatever the
  // card did, and costs nothing when the first read succeeds, which is almost
  // always. 20 ms between passes is there to let the card settle, not to meet a
  // deadline.
  static constexpr int ATTEMPTS = 3;
  static constexpr uint32_t SETTLE_MS = 20;

  for (int attempt = 0; attempt < ATTEMPTS; attempt++) {
    if (attempt > 0) delay(SETTLE_MS);

    if (!Storage.exists(path)) {
      // A save that got as far as writing its temporary but not as far as the
      // rename left the new document there. Prefer it over falling back to
      // defaults: it is the value the user last asked for.
      std::string tmp(path);
      tmp += ".tmp";
      if (Storage.exists(tmp.c_str()) && Storage.rename(tmp.c_str(), path)) {
        LOG_ERR("PERSIST", "Recovered %s from an interrupted save", path);
        continue;
      }
      // exists() is an SD read too, so it can be the thing that glitched. Only
      // the last pass is allowed to conclude the file is genuinely not there.
      if (attempt + 1 < ATTEMPTS) continue;
      if (why) *why = ReadFailure::Absent;
      return false;  // Expected on first boot — not an error.
    }

    String json = Storage.readFile(path);
    if (json.isEmpty()) {
      LOG_ERR("PERSIST", "Failed to read %s (empty) [%d/%d]", path, attempt + 1, ATTEMPTS);
      continue;
    }

    auto error = deserializeJson(doc, json);
    if (error) {
      LOG_ERR("PERSIST", "JSON parse error in %s: %s [%d/%d]", path, error.c_str(), attempt + 1, ATTEMPTS);
      continue;
    }
    return true;
  }

  // The file is there and we could not read it. The caller must NOT treat this
  // as "no settings yet": see PersistableStore::saveToFile().
  LOG_ERR("PERSIST", "Giving up on %s after %d attempts — keeping defaults, saving disabled", path, ATTEMPTS);
  if (why) *why = ReadFailure::Unreadable;
  return false;
}

std::string PersistableStoreBase::extractPassword(JsonVariantConst doc, bool& needsResave) {
  bool valid = false;
  return extractPassword(doc, needsResave, std::numeric_limits<size_t>::max(), valid);
}

std::string PersistableStoreBase::extractPassword(JsonVariantConst doc, bool& needsResave, const size_t maxLength,
                                                  bool& valid) {
  valid = true;
  bool ok = false;
  bool tooLong = false;
  std::string pass = obfuscation::deobfuscateFromBase64(doc["password_obf"] | "", maxLength, &ok, &tooLong);
  if (tooLong) {
    valid = false;
    return "";
  }
  if (!ok) {
    // Deobfuscation failed — fall back to legacy plaintext password.
    const char* legacyPassword = doc["password"] | "";
    const size_t legacyLength = strlen(legacyPassword);
    if (legacyLength > maxLength) {
      valid = false;
      return "";
    }
    pass.assign(legacyPassword, legacyLength);
    if (!pass.empty()) needsResave = true;
  }
  // A successfully decoded empty string is a legitimate value; preserve as-is.
  return pass;
}
