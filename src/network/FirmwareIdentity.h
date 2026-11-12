#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// Is the firmware on the card the one already running?
//
// Asked so a reflash of the same image can be confirmed rather than done by
// mistake. Deliberately *only* answers "same", "different" or "do not know",
// and the caller must treat "do not know" exactly like "different": the one
// thing this must never do is take away a way to update the device. Every
// failure -- a short read, a missing descriptor, a magic that does not match --
// lands on Unknown, and Unknown means flash.
//
// ── Why the ELF hash and not the version string ──────────────────────────────
// Measured on two real builds (D:/dev/crosspoint-reader-jp and a worktree at
// e14f93e4, five hours and many commits apart, different sizes):
//
//   esp_app_desc_t.version       v1.5.0-85-ge14f93e4   IDENTICAL in both
//   esp_app_desc_t.project_name  cp-e14f93e4           IDENTICAL in both
//   esp_app_desc_t.date / .time  Nov 11 2026 22:59:08  IDENTICAL in both
//   esp_app_desc_t.app_elf_sha256                      DIFFERENT
//
// The first three are baked by the build system and were not regenerated
// between those builds, so comparing them would call two different firmwares
// the same -- which is the one wrong answer that matters here. The ELF hash is
// the only field that tracked the change, so it is the only field compared.
//
// The project's own version string (1.5.0-dev-<branch>-<sha>) does distinguish
// them, but it lives wherever the linker put it, so reading it out of a file at
// a fixed offset would break the moment the code around it moves. It is not
// read: if the hashes match, the version strings are identical by definition,
// so showing the *running* side's string is enough for a person to check.
namespace firmware_identity {

// esp_image_header_t (24) + esp_image_segment_header_t (8). Verified against a
// real firmware.bin: the magic sits at exactly this offset.
constexpr size_t DESC_OFFSET = 32;
constexpr uint32_t DESC_MAGIC = 0xABCD5432;
// app_elf_sha256 is at +128 inside esp_app_desc_t, 32 bytes long.
constexpr size_t SHA_OFFSET_IN_DESC = 128;
constexpr size_t SHA_BYTES = 32;
// Everything above fits in the first 192 bytes, so a 5.6 MB image costs one
// short read to identify.
constexpr size_t HEAD_BYTES = DESC_OFFSET + SHA_OFFSET_IN_DESC + SHA_BYTES;

enum class Verdict : uint8_t {
  Same,       // the card holds the running image
  Different,  // it holds another one
  Unknown,    // could not tell -- treat as Different and flash
};

// `head` is the first `headLen` bytes of the candidate image; `runningSha256`
// is esp_app_get_description()->app_elf_sha256 of the running app, 32 bytes.
// Either may be null, and headLen may be short; that is Unknown, not a crash.
[[nodiscard]] inline Verdict compare(const uint8_t* head, const size_t headLen, const uint8_t* runningSha256) {
  if (head == nullptr || runningSha256 == nullptr) return Verdict::Unknown;
  if (headLen < HEAD_BYTES) return Verdict::Unknown;
  uint32_t magic = 0;
  // memcpy, not a cast: the ESP32-C3 faults on an unaligned 32-bit load and
  // `head` is a read buffer.
  memcpy(&magic, head + DESC_OFFSET, sizeof(magic));
  if (magic != DESC_MAGIC) return Verdict::Unknown;
  const uint8_t* candidate = head + DESC_OFFSET + SHA_OFFSET_IN_DESC;
  // An all-zero hash means the field was never filled in, which is no more an
  // identity than a missing descriptor is.
  bool allZero = true;
  for (size_t i = 0; i < SHA_BYTES; i++) {
    if (candidate[i] != 0) {
      allZero = false;
      break;
    }
  }
  if (allZero) return Verdict::Unknown;
  return memcmp(candidate, runningSha256, SHA_BYTES) == 0 ? Verdict::Same : Verdict::Different;
}

}  // namespace firmware_identity
