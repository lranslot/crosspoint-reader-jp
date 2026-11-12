// Is the firmware on the card the one already running?
//
// The case that matters most here is not "same" or "different" -- it is
// **broken**. A check that stands between the device and an update is worse
// than no check, so every failure has to fall towards flashing. Those are the
// tests this file exists for; the two happy answers are the easy half.
//
// The unit under test is the shipped header, included from src/, not a copy.

#include "FirmwareIdentity.h"

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

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

TEST(FirmwareIdentity, TheOffsetsAreTheOnesMeasuredOnARealImage) {
  // Pinned, because they were established by parsing a real firmware.bin and a
  // later change to them would silently turn every verdict into Unknown.
  EXPECT_EQ(firmware_identity::DESC_OFFSET, 32u) << "image header 24 + segment header 8";
  EXPECT_EQ(firmware_identity::DESC_MAGIC, 0xABCD5432u);
  EXPECT_EQ(firmware_identity::SHA_OFFSET_IN_DESC, 128u);
  EXPECT_EQ(firmware_identity::SHA_BYTES, 32u);
  EXPECT_EQ(HEAD_BYTES, 192u) << "one short read identifies a 5.6 MB image";
}

}  // namespace
