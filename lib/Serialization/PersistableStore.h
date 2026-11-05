#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Logging.h>

#include <mutex>
#include <string>

/**
 * @brief Non-template core of PersistableStore.
 *
 * All ArduinoJson parse/serialize machinery is instantiated once here (in
 * PersistableStore.cpp) instead of in every store's translation unit. GCC
 * emits the JSON serializer/parser templates as local .isra clones per TU
 * (~0.5KB each), so keeping serializeJson/deserializeJson out of the stores
 * is what makes the abstraction flash-neutral.
 */
class PersistableStoreBase {
 protected:
  PersistableStoreBase() = default;
  ~PersistableStoreBase() = default;

  // Serializes saveToFile/loadFromFile against each other across FreeRTOS
  // tasks, so the JSON snapshot cannot tear mid-serialize and two concurrent
  // saves cannot write their documents out of order. Concurrent saves are
  // reachable: the web server task saves settings while the main task can too.
  //
  // It is deliberately held across the SD write. That is safe only because the
  // read path does NOT take it — derived stores build their snapshots (e.g.
  // CrossPointSettings::statusBarSpec) unlocked. If you ever lock this mutex on
  // a read path, you put it on the render path and stall rendering behind SD
  // I/O, and you create a storeMutex/storageMutex ordering hazard. Don't.
  mutable std::mutex storeMutex;

  // fromJson() implementations call this (instead of saveToFile()) when the
  // on-disk JSON used a legacy shape that was upgraded in memory.
  // loadFromFile() performs the save after releasing storeMutex; calling
  // saveToFile() from inside fromJson() would deadlock on storeMutex.
  void requestResave() { resaveRequested = true; }

  bool resaveRequested = false;

  // Set when the file was present but could not be read, cleared by the next
  // successful load. Guards saveToFile(); see the comment there.
  bool loadUnreadable = false;

 public:
  // Public so non-store JSON files (e.g. per-book bookmarks) can reuse them
  // instead of instantiating serializeJson/deserializeJson in their own TU —
  // that per-TU duplication is exactly what this class exists to prevent.

  // Serializes doc and writes it to path (ensures /.crosspoint exists). Logs on failure.
  static bool writeDocToFile(const char* path, const JsonDocument& doc);

  // Why a file could not be turned into a document.
  //   Absent      nothing there. Ordinary on first boot.
  //   Unreadable  the file IS there and we failed to get it. On this hardware
  //               that is a transient SD read far more often than a corrupt
  //               file, and it must not be mistaken for Absent: the difference
  //               is "the user has no settings yet" versus "the user has
  //               settings and we just lost sight of them".
  enum class ReadFailure { None, Absent, Unreadable };

  // Reads path and parses it into doc. Retries a few times before giving up —
  // these files are a few hundred bytes and losing one costs the user every
  // setting they have. Returns false silently when the file does not exist
  // (expected on first boot); logs on read/parse failure. `why` reports which.
  static bool readDocFromFile(const char* path, JsonDocument& doc, ReadFailure* why = nullptr);

 protected:
  /**
   * Helper function for extracting an obfuscated password from a JSON value.
   * Accepts JsonVariantConst so callers can pass either a whole JsonDocument
   * or a JsonObject element (e.g. inside an array iteration).
   * If the decoded password requires a resave (e.g. from plaintext fallback), `needsResave` is set to true.
   */
  static std::string extractPassword(JsonVariantConst doc, bool& needsResave);
  static std::string extractPassword(JsonVariantConst doc, bool& needsResave, size_t maxLength, bool& valid);
};

/**
 * @brief Base class for persistable singletons using CRTP.
 *
 * Derived classes must provide:
 * - A private default constructor
 * - friend class PersistableStore<Derived>;
 * - static const char* getFilePath();
 * - void toJson(JsonDocument& doc) const;
 * - bool fromJson(JsonVariantConst doc);
 *
 * Note for implementers: read string values as `const char*` (e.g.
 * `obj["name"] | ""`), never as `| std::string("")` — ArduinoJson's
 * std::string converter drags a per-TU copy of the whole JSON serializer
 * into flash via its serializeJson fallback.
 *
 * Concurrency: saveToFile/loadFromFile lock storeMutex, so toJson/fromJson
 * always run under it. fromJson must signal legacy-shape upgrades with
 * requestResave(), never by calling saveToFile() directly (deadlock).
 */
template <typename T>
class PersistableStore : public PersistableStoreBase {
 protected:
  PersistableStore() = default;
  ~PersistableStore() = default;

 public:
  // Delete copy constructor and assignment
  PersistableStore(const PersistableStore&) = delete;
  PersistableStore& operator=(const PersistableStore&) = delete;

  static T& getInstance() {
    static T instance;
    return instance;
  }

  bool saveToFile() const {
    // The in-memory document is all defaults when the load failed, and toJson()
    // writes every field, so saving here replaces a file we know exists with
    // defaults — turning one bad read into the permanent loss of every setting
    // the user has. That is the chain behind "vertical writing keeps resetting":
    // a failed read left verticalWriting at 0, and the next save made it stick.
    //
    // Cost of this guard: a genuinely corrupt file cannot be overwritten either,
    // so settings stay unsaveable until it is removed. That is why the message
    // names the file — deleting it on a PC restores a working first-boot state.
    if (loadUnreadable) {
      LOG_ERR("PERSIST", "Refusing to save %s: existing file unreadable, would overwrite it with defaults",
              T::getFilePath());
      return false;
    }
    std::lock_guard<std::mutex> lock(storeMutex);
    JsonDocument doc;
    static_cast<const T*>(this)->toJson(doc);
    return writeDocToFile(T::getFilePath(), doc);
  }

  bool loadFromFile() {
    bool ok;
    bool doResave;
    {
      std::lock_guard<std::mutex> lock(storeMutex);
      resaveRequested = false;
      JsonDocument doc;
      ReadFailure why = ReadFailure::None;
      if (!readDocFromFile(T::getFilePath(), doc, &why)) {
        // Absent leaves saving enabled: that really is first boot, and the
        // defaults in memory are the right thing to write.
        loadUnreadable = (why == ReadFailure::Unreadable);
        return false;
      }
      loadUnreadable = false;
      ok = static_cast<T*>(this)->fromJson(doc.as<JsonVariantConst>());
      // Read the flag under the lock that guards the fromJson() that set it.
      doResave = resaveRequested;
      resaveRequested = false;
    }
    // Deliberately outside the lock: saveToFile() takes storeMutex itself.
    if (ok && doResave && !saveToFile()) {
      LOG_ERR("PERSIST", "Failed to resave %s after format update", T::getFilePath());
    }
    return ok;
  }
};
