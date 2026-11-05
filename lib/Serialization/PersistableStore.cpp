#include "PersistableStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

#include <cstring>
#include <limits>

bool PersistableStoreBase::writeDocToFile(const char* path, const JsonDocument& doc) {
  Storage.mkdir("/.crosspoint");
  String json;
  serializeJson(doc, json);
  if (!Storage.writeFile(path, json)) {
    LOG_ERR("PERSIST", "Failed to write %s", path);
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
