// Is the firmware on the card the one already running?
//
// The case that matters most here is not "same" or "different" -- it is
// **broken**. A check that stands between the device and an update is worse
// than no check, so every failure has to fall towards flashing. Those are the
// tests this file exists for; the two happy answers are the easy half.
//
// The unit under test is the shipped header, included from src/, not a copy.

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "FirmwareIdentity.h"

namespace {

using firmware_identity::compare;
using firmware_identity::HEAD_BYTES;
using firmware_identity::Verdict;

// A hash that is not all-zero, so it counts as an identity.
std::vector<uint8_t> hashOf(const uint8_t seed) {
  std::vector<uint8_t> h(firmware_identity::SHA_BYTES);
  for (size_t i = 0; i < h.size(); i++) h[i] = static_cast<uint8_t>(seed + i);
  return h;
}

// The first 192 bytes of a plausible image: the ESP image and segment headers
// are not inspected, only the descriptor magic and the hash.
std::vector<uint8_t> headWith(const std::vector<uint8_t>& sha, const uint32_t magic = firmware_identity::DESC_MAGIC) {
  std::vector<uint8_t> head(HEAD_BYTES, 0xAA);
  memcpy(head.data() + firmware_identity::DESC_OFFSET, &magic, sizeof(magic));
  memcpy(head.data() + firmware_identity::DESC_OFFSET + firmware_identity::SHA_OFFSET_IN_DESC, sha.data(), sha.size());
  return head;
}

// --- the two answers that are easy -----------------------------------------

TEST(FirmwareIdentity, TheSameHashIsTheSameFirmware) {
  const auto sha = hashOf(1);
  const auto head = headWith(sha);
  EXPECT_EQ(compare(head.data(), head.size(), sha.data()), Verdict::Same);
}

TEST(FirmwareIdentity, ADifferentHashIsADifferentFirmware) {
  const auto onCard = hashOf(1);
  const auto running = hashOf(2);
  const auto head = headWith(onCard);
  EXPECT_EQ(compare(head.data(), head.size(), running.data()), Verdict::Different);
}

TEST(FirmwareIdentity, OneByteOfDifferenceIsEnough) {
  auto onCard = hashOf(1);
  const auto running = onCard;
  onCard.back() ^= 0x01;
  const auto head = headWith(onCard);
  EXPECT_EQ(compare(head.data(), head.size(), running.data()), Verdict::Different);
}

// --- ★ broken must fall towards flashing -----------------------------------

TEST(FirmwareIdentity, ANullHeadIsUnknown) {
  const auto sha = hashOf(1);
  EXPECT_EQ(compare(nullptr, HEAD_BYTES, sha.data()), Verdict::Unknown);
}

TEST(FirmwareIdentity, ANullRunningHashIsUnknown) {
  const auto head = headWith(hashOf(1));
  EXPECT_EQ(compare(head.data(), head.size(), nullptr), Verdict::Unknown);
}

TEST(FirmwareIdentity, AShortReadIsUnknownAtEveryLength) {
  // An SD read that returned fewer bytes than asked for, at every length it
  // could have stopped at. None of them may be read as an identity.
  const auto sha = hashOf(1);
  const auto head = headWith(sha);
  for (size_t len = 0; len < HEAD_BYTES; len++) {
    EXPECT_EQ(compare(head.data(), len, sha.data()), Verdict::Unknown) << "len " << len;
  }
  // The exact length is the first one that can answer.
  EXPECT_EQ(compare(head.data(), HEAD_BYTES, sha.data()), Verdict::Same);
}

TEST(FirmwareIdentity, AWrongMagicIsUnknownEvenWhenTheHashMatches) {
  // A file that happens to carry the running hash at the right offset but is
  // not an app descriptor. Unknown, not Same.
  const auto sha = hashOf(1);
  const auto head = headWith(sha, 0xDEADBEEF);
  EXPECT_EQ(compare(head.data(), head.size(), sha.data()), Verdict::Unknown);
}

TEST(FirmwareIdentity, AMagicOffByOneByteIsUnknown) {
  const auto sha = hashOf(1);
  auto head = headWith(sha);
  head[firmware_identity::DESC_OFFSET] ^= 0x01;
  EXPECT_EQ(compare(head.data(), head.size(), sha.data()), Verdict::Unknown);
}

TEST(FirmwareIdentity, AnAllZeroHashIsUnknownNotSame) {
  // A descriptor whose hash field was never filled in. Two such images would
  // compare equal to each other, which is not an identity.
  std::vector<uint8_t> zero(firmware_identity::SHA_BYTES, 0);
  const auto head = headWith(zero);
  EXPECT_EQ(compare(head.data(), head.size(), zero.data()), Verdict::Unknown);
}

TEST(FirmwareIdentity, AllZeroBytesEverywhereIsUnknown) {
  // An erased or empty file.
  const std::vector<uint8_t> head(HEAD_BYTES, 0);
  const auto sha = hashOf(1);
  EXPECT_EQ(compare(head.data(), head.size(), sha.data()), Verdict::Unknown);
}

TEST(FirmwareIdentity, GarbageIsUnknown) {
  std::vector<uint8_t> head(HEAD_BYTES, 0xFF);
  const auto sha = hashOf(1);
  EXPECT_EQ(compare(head.data(), head.size(), sha.data()), Verdict::Unknown);
}

// --- the one invariant the caller depends on --------------------------------

TEST(FirmwareIdentity, OnlyAnExactMatchEverSaysSame) {
  // Swept over every way this could be wrong at once: the verdict is Same for
  // exactly one of these inputs and never for a broken one.
  const auto sha = hashOf(7);
  int sameCount = 0;
  const std::vector<std::vector<uint8_t>> heads = {
      headWith(sha),
      headWith(hashOf(8)),
      headWith(sha, 0),
      headWith(std::vector<uint8_t>(firmware_identity::SHA_BYTES, 0)),
      std::vector<uint8_t>(HEAD_BYTES, 0),
      std::vector<uint8_t>(HEAD_BYTES, 0xFF),
  };
  for (const auto& h : heads) {
    if (compare(h.data(), h.size(), sha.data()) == Verdict::Same) sameCount++;
  }
  EXPECT_EQ(sameCount, 1);
}

TEST(FirmwareIdentity, TheHashIsReadFromTheFieldAndNotFromItsNeighbours) {
  // ★ The test that was missing, and the reason the check shipped broken.
  //
  // The old one asserted SHA_OFFSET_IN_DESC == 128 -- a constant against
  // itself, which cannot catch a wrong constant. This one fills every other
  // field of esp_app_desc_t with a distinct byte and puts the hash only in the
  // field, so reading one byte to either side fails. At 128 (the shipped bug)
  // the window is the tail of idf_ver plus half the hash, and this does not
  // pass.
  std::vector<uint8_t> head(HEAD_BYTES, 0x00);
  const uint32_t magic = firmware_identity::DESC_MAGIC;
  memcpy(head.data() + firmware_identity::DESC_OFFSET, &magic, sizeof(magic));
  const auto fill = [&](const size_t at, const size_t len, const uint8_t b) {
    for (size_t i = 0; i < len; i++) head[firmware_identity::DESC_OFFSET + at + i] = b;
  };
  fill(4, 4, 0x11);     // secure_version
  fill(8, 8, 0x22);     // reserv1[2]
  fill(16, 32, 0x33);   // version
  fill(48, 32, 0x44);   // project_name
  fill(80, 16, 0x55);   // time
  fill(96, 16, 0x66);   // date
  fill(112, 32, 0x77);  // idf_ver -- the field the bug read into
  const auto sha = hashOf(0x90);
  memcpy(head.data() + firmware_identity::DESC_OFFSET + firmware_identity::SHA_OFFSET_IN_DESC, sha.data(), sha.size());

  EXPECT_EQ(compare(head.data(), head.size(), sha.data()), Verdict::Same);

  // And every neighbouring window must not match, which is what "read the field"
  // means. 0x77 is idf_ver; a window starting 16 bytes early is half of it.
  std::vector<uint8_t> shifted(firmware_identity::SHA_BYTES);
  memcpy(shifted.data(), head.data() + firmware_identity::DESC_OFFSET + firmware_identity::SHA_OFFSET_IN_DESC - 16, 16);
  memcpy(shifted.data() + 16, sha.data(), 16);
  EXPECT_EQ(compare(head.data(), head.size(), shifted.data()), Verdict::Different)
      << "the old 128 offset read exactly this window";
}

TEST(FirmwareIdentity, TheOffsetsMatchTheEspAppDescLayout) {
  // Spelled out as the sum that produces them, not as the numbers, so a wrong
  // one has to be wrong about a field size to get here.
  constexpr size_t kMagic = 4, kSecure = 4, kReserv1 = 8, kVersion = 32, kProject = 32, kTime = 16, kDate = 16,
                   kIdfVer = 32;
  EXPECT_EQ(firmware_identity::SHA_OFFSET_IN_DESC,
            kMagic + kSecure + kReserv1 + kVersion + kProject + kTime + kDate + kIdfVer);
  EXPECT_EQ(firmware_identity::DESC_OFFSET, 24u + 8u) << "esp_image_header_t + esp_image_segment_header_t";
  EXPECT_EQ(firmware_identity::DESC_MAGIC, 0xABCD5432u);
  EXPECT_EQ(firmware_identity::SHA_BYTES, 32u);
  EXPECT_EQ(HEAD_BYTES, 208u) << "one short read identifies a 5.6 MB image";
}

}  // namespace
