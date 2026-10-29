#pragma once

// UTR #50 Vertical_Orientation の抜粋。**自動生成。手で編集しない。**
//
// 生成: python scripts/gen_vertical_orientation.py <VerticalOrientation.txt> --emit-cpp
// 取得: curl -o VerticalOrientation.txt
//       https://www.unicode.org/Public/UCD/latest/ucd/VerticalOrientation.txt
//
// 既定は U (正立) なので、U の符号位置はこの表に載せない。
// 手で列挙していたときは 30 字抜けていた (《》を含む)。

#include <cstdint>

namespace vo {

/// 縦書きでの字の向き (UTR #50)。
enum class Orientation : uint8_t {
  Upright,     ///< U  コード表と同じ向き
  Rotated,     ///< R  時計回りに 90 度
  TransformU,  ///< Tu 縦書き字形の代用。回転しない (墨を升目の角へ)
  TransformR,  ///< Tr 縦書き字形の代用。時計回りに 90 度
};

struct Entry {
  uint16_t cp;
  Orientation value;
};

// 符号位置で昇順。二分探索する。
inline constexpr Entry TABLE[] = {
    {0x0020, Orientation::Rotated},    {0x0021, Orientation::Rotated},    {0x0022, Orientation::Rotated},
    {0x0023, Orientation::Rotated},    {0x0024, Orientation::Rotated},    {0x0025, Orientation::Rotated},
    {0x0026, Orientation::Rotated},    {0x0027, Orientation::Rotated},    {0x0028, Orientation::Rotated},
    {0x0029, Orientation::Rotated},    {0x002A, Orientation::Rotated},    {0x002B, Orientation::Rotated},
    {0x002C, Orientation::Rotated},    {0x002D, Orientation::Rotated},    {0x002E, Orientation::Rotated},
    {0x002F, Orientation::Rotated},    {0x0030, Orientation::Rotated},    {0x0031, Orientation::Rotated},
    {0x0032, Orientation::Rotated},    {0x0033, Orientation::Rotated},    {0x0034, Orientation::Rotated},
    {0x0035, Orientation::Rotated},    {0x0036, Orientation::Rotated},    {0x0037, Orientation::Rotated},
    {0x0038, Orientation::Rotated},    {0x0039, Orientation::Rotated},    {0x003A, Orientation::Rotated},
    {0x003B, Orientation::Rotated},    {0x003C, Orientation::Rotated},    {0x003D, Orientation::Rotated},
    {0x003E, Orientation::Rotated},    {0x003F, Orientation::Rotated},    {0x0040, Orientation::Rotated},
    {0x0041, Orientation::Rotated},    {0x0042, Orientation::Rotated},    {0x0043, Orientation::Rotated},
    {0x0044, Orientation::Rotated},    {0x0045, Orientation::Rotated},    {0x0046, Orientation::Rotated},
    {0x0047, Orientation::Rotated},    {0x0048, Orientation::Rotated},    {0x0049, Orientation::Rotated},
    {0x004A, Orientation::Rotated},    {0x004B, Orientation::Rotated},    {0x004C, Orientation::Rotated},
    {0x004D, Orientation::Rotated},    {0x004E, Orientation::Rotated},    {0x004F, Orientation::Rotated},
    {0x0050, Orientation::Rotated},    {0x0051, Orientation::Rotated},    {0x0052, Orientation::Rotated},
    {0x0053, Orientation::Rotated},    {0x0054, Orientation::Rotated},    {0x0055, Orientation::Rotated},
    {0x0056, Orientation::Rotated},    {0x0057, Orientation::Rotated},    {0x0058, Orientation::Rotated},
    {0x0059, Orientation::Rotated},    {0x005A, Orientation::Rotated},    {0x005B, Orientation::Rotated},
    {0x005C, Orientation::Rotated},    {0x005D, Orientation::Rotated},    {0x005E, Orientation::Rotated},
    {0x005F, Orientation::Rotated},    {0x0060, Orientation::Rotated},    {0x0061, Orientation::Rotated},
    {0x0062, Orientation::Rotated},    {0x0063, Orientation::Rotated},    {0x0064, Orientation::Rotated},
    {0x0065, Orientation::Rotated},    {0x0066, Orientation::Rotated},    {0x0067, Orientation::Rotated},
    {0x0068, Orientation::Rotated},    {0x0069, Orientation::Rotated},    {0x006A, Orientation::Rotated},
    {0x006B, Orientation::Rotated},    {0x006C, Orientation::Rotated},    {0x006D, Orientation::Rotated},
    {0x006E, Orientation::Rotated},    {0x006F, Orientation::Rotated},    {0x0070, Orientation::Rotated},
    {0x0071, Orientation::Rotated},    {0x0072, Orientation::Rotated},    {0x0073, Orientation::Rotated},
    {0x0074, Orientation::Rotated},    {0x0075, Orientation::Rotated},    {0x0076, Orientation::Rotated},
    {0x0077, Orientation::Rotated},    {0x0078, Orientation::Rotated},    {0x0079, Orientation::Rotated},
    {0x007A, Orientation::Rotated},    {0x007B, Orientation::Rotated},    {0x007C, Orientation::Rotated},
    {0x007D, Orientation::Rotated},    {0x007E, Orientation::Rotated},    {0x2010, Orientation::Rotated},
    {0x2012, Orientation::Rotated},    {0x2013, Orientation::Rotated},    {0x2014, Orientation::Rotated},
    {0x2015, Orientation::Rotated},    {0x2018, Orientation::TransformR}, {0x2019, Orientation::TransformR},
    {0x201C, Orientation::TransformR}, {0x201D, Orientation::TransformR}, {0x2025, Orientation::Rotated},
    {0x2026, Orientation::Rotated},    {0x3001, Orientation::TransformU}, {0x3002, Orientation::TransformU},
    {0x3008, Orientation::TransformR}, {0x3009, Orientation::TransformR}, {0x300A, Orientation::TransformR},
    {0x300B, Orientation::TransformR}, {0x300C, Orientation::TransformR}, {0x300D, Orientation::TransformR},
    {0x300E, Orientation::TransformR}, {0x300F, Orientation::TransformR}, {0x3010, Orientation::TransformR},
    {0x3011, Orientation::TransformR}, {0x3014, Orientation::TransformR}, {0x3015, Orientation::TransformR},
    {0x3016, Orientation::TransformR}, {0x3017, Orientation::TransformR}, {0x3018, Orientation::TransformR},
    {0x3019, Orientation::TransformR}, {0x301A, Orientation::TransformR}, {0x301B, Orientation::TransformR},
    {0x301C, Orientation::TransformR}, {0x301D, Orientation::TransformR}, {0x301E, Orientation::TransformR},
    {0x301F, Orientation::TransformR}, {0x3041, Orientation::TransformU}, {0x3043, Orientation::TransformU},
    {0x3045, Orientation::TransformU}, {0x3047, Orientation::TransformU}, {0x3049, Orientation::TransformU},
    {0x3063, Orientation::TransformU}, {0x3083, Orientation::TransformU}, {0x3085, Orientation::TransformU},
    {0x3087, Orientation::TransformU}, {0x308E, Orientation::TransformU}, {0x3095, Orientation::TransformU},
    {0x3096, Orientation::TransformU}, {0x309B, Orientation::TransformU}, {0x309C, Orientation::TransformU},
    {0x30A0, Orientation::TransformR}, {0x30A1, Orientation::TransformU}, {0x30A3, Orientation::TransformU},
    {0x30A5, Orientation::TransformU}, {0x30A7, Orientation::TransformU}, {0x30A9, Orientation::TransformU},
    {0x30C3, Orientation::TransformU}, {0x30E3, Orientation::TransformU}, {0x30E5, Orientation::TransformU},
    {0x30E7, Orientation::TransformU}, {0x30EE, Orientation::TransformU}, {0x30F5, Orientation::TransformU},
    {0x30F6, Orientation::TransformU}, {0x30FC, Orientation::TransformR}, {0xFF01, Orientation::TransformU},
    {0xFF08, Orientation::TransformR}, {0xFF09, Orientation::TransformR}, {0xFF0C, Orientation::TransformU},
    {0xFF0D, Orientation::Rotated},    {0xFF0E, Orientation::TransformU}, {0xFF1A, Orientation::TransformR},
    {0xFF1B, Orientation::TransformR}, {0xFF1C, Orientation::Rotated},    {0xFF1D, Orientation::Rotated},
    {0xFF1E, Orientation::Rotated},    {0xFF1F, Orientation::TransformU}, {0xFF3B, Orientation::TransformR},
    {0xFF3D, Orientation::TransformR}, {0xFF3F, Orientation::TransformR}, {0xFF5B, Orientation::TransformR},
    {0xFF5C, Orientation::TransformR}, {0xFF5D, Orientation::TransformR}, {0xFF5E, Orientation::TransformR},
    {0xFF5F, Orientation::TransformR}, {0xFF60, Orientation::TransformR}, {0xFF61, Orientation::Rotated},
    {0xFF62, Orientation::Rotated},    {0xFF63, Orientation::Rotated},    {0xFF64, Orientation::Rotated},
};
inline constexpr int TABLE_SIZE = 183;

/// 既定は Upright。表に無い符号位置はすべて正立。
inline Orientation lookup(uint32_t cp) {
  if (cp > 0xFFFF) return Orientation::Upright;
  int lo = 0, hi = TABLE_SIZE - 1;
  while (lo <= hi) {
    const int mid = (lo + hi) / 2;
    if (TABLE[mid].cp == cp) return TABLE[mid].value;
    if (TABLE[mid].cp < cp)
      lo = mid + 1;
    else
      hi = mid - 1;
  }
  return Orientation::Upright;
}

}  // namespace vo
