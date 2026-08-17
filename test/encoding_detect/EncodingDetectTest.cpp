#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "Utf8.h"

namespace {

// Byte sequences are spelled out so the test does not depend on the encoding of
// this source file.
const std::string kUtf8Hello = "hello world";
// "こんにちは" in UTF-8
const std::string kUtf8Japanese = "\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF";
// "こんにちは" in CP932 (Shift-JIS)
const std::string kSjisJapanese = "\x82\xB1\x82\xF1\x82\xC9\x82\xBF\x82\xCD";
// Halfwidth katakana "ｱｲｳ" in CP932: single bytes 0xB1 0xB2 0xB3
const std::string kSjisHalfKana = "\xB1\xB2\xB3";

TextEncoding detect(const std::string& s) {
  return detectTextEncoding(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

std::string convert(const std::string& s, const bool flush = true) {
  std::string out;
  cp932ToUtf8(reinterpret_cast<const uint8_t*>(s.data()), s.size(), out, flush);
  return out;
}

}  // namespace

// Pure ASCII is UTF-8 and must never be converted.
TEST(DetectTextEncoding, AsciiIsUtf8) {
  EXPECT_EQ(detect(kUtf8Hello), TextEncoding::Utf8);
  EXPECT_EQ(detect(""), TextEncoding::Utf8);
  EXPECT_EQ(detect("- [ ] task\n- [x] done\n"), TextEncoding::Utf8);
}

// Valid UTF-8 wins over Shift-JIS: the leading bytes of Japanese Shift-JIS are
// continuation bytes in UTF-8 and cannot appear on their own.
TEST(DetectTextEncoding, Utf8Japanese) {
  EXPECT_EQ(detect(kUtf8Japanese), TextEncoding::Utf8);
  EXPECT_EQ(detect(kUtf8Hello + kUtf8Japanese), TextEncoding::Utf8);
}

// A UTF-8 BOM settles it immediately.
TEST(DetectTextEncoding, Utf8Bom) {
  const std::string withBom = "\xEF\xBB\xBF" + kUtf8Japanese;
  EXPECT_EQ(detect(withBom), TextEncoding::Utf8);
  EXPECT_EQ(utf8BomLength(reinterpret_cast<const uint8_t*>(withBom.data()), withBom.size()), 3u);
  EXPECT_EQ(utf8BomLength(reinterpret_cast<const uint8_t*>(kUtf8Hello.data()), kUtf8Hello.size()), 0u);
}

TEST(DetectTextEncoding, ShiftJis) {
  EXPECT_EQ(detect(kSjisJapanese), TextEncoding::ShiftJis);
  EXPECT_EQ(detect("Title: " + kSjisJapanese), TextEncoding::ShiftJis);
  EXPECT_EQ(detect(kSjisHalfKana), TextEncoding::ShiftJis);
}

// UTF-16 is out of scope and must be reported as UTF-8 so the file is left alone.
TEST(DetectTextEncoding, Utf16IsLeftAlone) {
  EXPECT_EQ(detect(std::string("\xFF\xFE", 2) + std::string("h\0i\0", 4)), TextEncoding::Utf8);
  EXPECT_EQ(detect(std::string("\xFE\xFF", 2) + std::string("\0h\0i", 4)), TextEncoding::Utf8);
}

// Neither valid UTF-8 nor valid Shift-JIS: report UTF-8 rather than rewrite on a guess.
TEST(DetectTextEncoding, UndecidableFallsBackToUtf8) {
  EXPECT_EQ(detect(std::string("\xFF\xFF\xFF", 3)), TextEncoding::Utf8);
  // 0x81 is a Shift-JIS lead byte, but 0x20 is not a valid trail byte.
  EXPECT_EQ(detect(std::string("\x81\x20\x81\x20", 4)), TextEncoding::Utf8);
}

// A multi-byte sequence cut off by the end of the probe must not flip the verdict.
TEST(DetectTextEncoding, ToleratesTruncatedTail) {
  // UTF-8 "こ" is E3 81 93; keep only the first two bytes.
  EXPECT_EQ(detect(kUtf8Japanese.substr(0, kUtf8Japanese.size() - 1)), TextEncoding::Utf8);
  // Shift-JIS pair cut after its lead byte.
  EXPECT_EQ(detect(kSjisJapanese.substr(0, kSjisJapanese.size() - 1)), TextEncoding::ShiftJis);
}

TEST(Cp932ToUtf8, AsciiPassesThrough) { EXPECT_EQ(convert(kUtf8Hello), kUtf8Hello); }

TEST(Cp932ToUtf8, ConvertsKanaAndKanji) {
  EXPECT_EQ(convert(kSjisJapanese), kUtf8Japanese);
  // 0x93 0xFA is "日" U+65E5
  EXPECT_EQ(convert(std::string("\x93\xFA", 2)), "\xE6\x97\xA5");
}

// Halfwidth katakana are single bytes mapped arithmetically, with no table lookup.
TEST(Cp932ToUtf8, HalfwidthKatakana) {
  // 0xB1 -> U+FF71, 0xB2 -> U+FF72, 0xB3 -> U+FF73
  EXPECT_EQ(convert(kSjisHalfKana), "\xEF\xBD\xB1\xEF\xBD\xB2\xEF\xBD\xB3");
}

// Undefined pairs and bytes that cannot start or stand alone become U+FFFD rather
// than vanishing.
TEST(Cp932ToUtf8, InvalidBytesBecomeReplacement) {
  const std::string replacement = "\xEF\xBF\xBD";
  // 0x80 is neither a lead byte, a trail byte nor halfwidth katakana.
  EXPECT_EQ(convert(std::string("\x80", 1)), replacement);
  // 0x81 0x7F is a hole in the CP932 grid (0x81 0x40 is defined, 0x81 0x7F is not).
  EXPECT_EQ(convert(std::string("\x81\x7F", 2)), replacement);
  // ...while the pair either side of it decodes normally: U+3001 IDEOGRAPHIC COMMA.
  EXPECT_EQ(convert(std::string("\x81\x41", 2)), "\xE3\x80\x81");
}

// Without flush, a lead byte at the buffer edge is handed back to the caller so it
// can be re-fed with the next chunk. With flush it becomes U+FFFD.
TEST(Cp932ToUtf8, HoldsBackSplitPairUntilFlush) {
  const std::string lead("\x82", 1);
  std::string out;
  EXPECT_EQ(cp932ToUtf8(reinterpret_cast<const uint8_t*>(lead.data()), lead.size(), out, /*flush=*/false), 0u);
  EXPECT_TRUE(out.empty());

  out.clear();
  EXPECT_EQ(cp932ToUtf8(reinterpret_cast<const uint8_t*>(lead.data()), lead.size(), out, /*flush=*/true), 1u);
  EXPECT_EQ(out, "\xEF\xBF\xBD");
}

// Feeding the same bytes in two chunks must produce the same result as one pass.
TEST(Cp932ToUtf8, StreamingMatchesSinglePass) {
  const std::string& input = kSjisJapanese;
  for (size_t split = 1; split < input.size(); split++) {
    std::string out;
    const auto* data = reinterpret_cast<const uint8_t*>(input.data());
    const size_t consumed = cp932ToUtf8(data, split, out, /*flush=*/false);
    std::string tail = input.substr(consumed);
    cp932ToUtf8(reinterpret_cast<const uint8_t*>(tail.data()), tail.size(), out, /*flush=*/true);
    EXPECT_EQ(out, kUtf8Japanese) << "split at " << split;
  }
}
