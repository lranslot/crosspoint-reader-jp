#!/usr/bin/env python3
"""
Generate lib/Utf8/Cp932Table.h from Python's built-in cp932 codec.

CP932 (Shift-JIS as shipped by Microsoft, and what Aozora Bunko text uses) encodes
kanji as a two-byte pair: a lead byte in 0x81-0x9F or 0xE0-0xFC followed by a trail
byte in 0x40-0xFC. The table is a flat lead x trail grid so a lookup is two
subtractions and an index — no search, no branching on ranges beyond the bounds
check. Undefined pairs are stored as 0 and rendered as U+FFFD at conversion time.

Single bytes (0x00-0x7F ASCII, 0xA1-0xDF halfwidth katakana) are handled
arithmetically by the converter and are deliberately not in the table.

No network access and no external data files: the mapping comes from the codec
bundled with CPython.

Usage:
    python scripts/gen_cp932_table.py [output_path]

Default output: lib/Utf8/Cp932Table.h
"""

import sys
from pathlib import Path

# Lead byte ranges, in order. Their concatenation defines the row index.
LEAD_RANGES = ((0x81, 0x9F), (0xE0, 0xFC))
# Trail bytes form one contiguous run, so the column index is a single subtraction.
TRAIL_FIRST = 0x40
TRAIL_LAST = 0xFC


def lead_bytes():
    for lo, hi in LEAD_RANGES:
        for b in range(lo, hi + 1):
            yield b


def main(out_path: str) -> None:
    leads = list(lead_bytes())
    trail_count = TRAIL_LAST - TRAIL_FIRST + 1

    rows = []
    mapped = 0
    for lead in leads:
        row = []
        for trail in range(TRAIL_FIRST, TRAIL_LAST + 1):
            try:
                text = bytes((lead, trail)).decode("cp932")
            except UnicodeDecodeError:
                row.append(0)
                continue
            # Every cp932 two-byte pair decodes to a single BMP codepoint, so it
            # fits a uint16_t. Guard the assumption rather than truncate silently.
            if len(text) != 1 or ord(text[0]) > 0xFFFF:
                raise ValueError(
                    "cp932 %02X%02X decoded to %r, which does not fit uint16_t" % (lead, trail, text)
                )
            row.append(ord(text[0]))
            mapped += 1
        rows.append(row)

    lines = [
        "// Auto-generated CP932 (Shift-JIS) to Unicode table. Generated from Python's",
        "// built-in cp932 codec by scripts/gen_cp932_table.py — do not edit by hand.",
        "// Used by the TXT reader to convert Shift-JIS files (Aozora Bunko and most",
        "// Japanese plain text) to UTF-8 once, on first open, into the file's cache.",
        "//",
        "// Layout: a flat [lead][trail] grid. Lead bytes 0x81-0x9F and 0xE0-0xFC are",
        "// concatenated into one row index; trail bytes 0x40-0xFC form the column.",
        "// A lookup is two subtractions and an index. 0 means the pair is undefined.",
        "//",
        "// Single bytes are not in the table: 0x00-0x7F pass through as ASCII and",
        "// 0xA1-0xDF (halfwidth katakana) map arithmetically to U+FF61 + (b - 0xA1).",
        "#pragma once",
        "#include <cstdint>",
        "",
        "constexpr uint8_t kCp932TrailFirst = 0x%02X;" % TRAIL_FIRST,
        "constexpr uint8_t kCp932TrailLast = 0x%02X;" % TRAIL_LAST,
        "constexpr int kCp932TrailCount = %d;" % trail_count,
        "constexpr int kCp932LeadCount = %d;" % len(leads),
        "",
        "// Row index for a lead byte, or -1 when the byte cannot start a pair.",
        "inline int cp932LeadIndex(const uint8_t b) {",
        "  if (b >= 0x81 && b <= 0x9F) return b - 0x81;",
        "  if (b >= 0xE0 && b <= 0xFC) return (b - 0xE0) + %d;" % (LEAD_RANGES[0][1] - LEAD_RANGES[0][0] + 1),
        "  return -1;",
        "}",
        "",
        "constexpr uint16_t kCp932Table[kCp932LeadCount][kCp932TrailCount] = {",
    ]

    for lead, row in zip(leads, rows):
        lines.append("    // lead 0x%02X" % lead)
        for i in range(0, trail_count, 12):
            chunk = row[i : i + 12]
            body = ", ".join("0x%04X" % v for v in chunk)
            prefix = "    {" if i == 0 else "     "
            suffix = "," if i + 12 < trail_count else "},"
            lines.append(prefix + body + suffix)

    lines.append("};")
    lines.append("")
    lines.append("// Returns the Unicode codepoint for a cp932 pair, or 0 when undefined.")
    lines.append("inline uint16_t cp932Lookup(const uint8_t lead, const uint8_t trail) {")
    lines.append("  const int row = cp932LeadIndex(lead);")
    lines.append("  if (row < 0 || trail < kCp932TrailFirst || trail > kCp932TrailLast) return 0;")
    lines.append("  return kCp932Table[row][trail - kCp932TrailFirst];")
    lines.append("}")

    out = Path(out_path)
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))
        f.write("\n")

    table_bytes = len(leads) * trail_count * 2
    print("Wrote %s" % out)
    print("  lead rows   : %d" % len(leads))
    print("  trail cols  : %d" % trail_count)
    print("  entries     : %d (%d mapped, %d undefined)" % (len(leads) * trail_count, mapped, len(leads) * trail_count - mapped))
    print("  table bytes : %d (%.1f KB)" % (table_bytes, table_bytes / 1024.0))


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "lib/Utf8/Cp932Table.h")
