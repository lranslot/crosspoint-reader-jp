#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""端末のスクリーンショット (1bpp BMP) を PNG にする。外部ライブラリ不要。

端末は POWER + DOWN で /screenshots/ に 480x800 / 1bpp の BMP を書く
(ScreenshotUtil.cpp)。そのままでは画像として開けない場面が多いので、
PNG に変換して 1 px 単位で確かめられるようにする。

使い方:
  python scripts/bmp_to_png.py G:/screenshots           # 配下を全部変換
  python scripts/bmp_to_png.py G:/screenshots out_dir   # 出力先を指定
  python scripts/bmp_to_png.py shot.bmp                 # 1 枚だけ

出力先を省くと、BMP と同じ名前の .png を隣に作る。SD に書きたくない場合は
出力先を必ず指定すること。
"""
import pathlib
import struct
import sys
import zlib


def bmp_to_png_bytes(data: bytes) -> tuple[bytes, int, int]:
    """1bpp の BMP を 8bit グレースケール PNG に変換する。"""
    if data[:2] != b'BM':
        raise ValueError('BMP ではない (magic が BM でない)')
    pixel_offset = struct.unpack('<I', data[10:14])[0]
    width, height = struct.unpack('<ii', data[18:26])
    bpp = struct.unpack('<H', data[28:30])[0]
    if bpp != 1:
        raise ValueError(f'1bpp 以外には未対応 (bpp={bpp})')

    bottom_up = height > 0
    height = abs(height)
    row_bytes = (width + 31) // 32 * 4

    rows = []
    order = range(height - 1, -1, -1) if bottom_up else range(height)
    for y in order:
        line = data[pixel_offset + y * row_bytes: pixel_offset + y * row_bytes + row_bytes]
        out = bytearray([0])  # PNG のフィルタ種別 (None)
        for x in range(width):
            bit = (line[x >> 3] >> (7 - (x & 7))) & 1
            out.append(0xFF if bit else 0x00)
        rows.append(bytes(out))

    def chunk(tag: bytes, payload: bytes) -> bytes:
        body = tag + payload
        return struct.pack('>I', len(payload)) + body + struct.pack('>I', zlib.crc32(body) & 0xFFFFFFFF)

    png = (b'\x89PNG\r\n\x1a\n'
           + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 0, 0, 0, 0))
           + chunk(b'IDAT', zlib.compress(b''.join(rows), 9))
           + chunk(b'IEND', b''))
    return png, width, height


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    src = pathlib.Path(sys.argv[1])
    out_dir = pathlib.Path(sys.argv[2]) if len(sys.argv) > 2 else None
    if out_dir:
        out_dir.mkdir(parents=True, exist_ok=True)

    sources = [src] if src.is_file() else sorted(src.rglob('*.bmp'))
    if not sources:
        print(f'BMP が見つからない: {src}')
        return 1

    for path in sources:
        try:
            png, w, h = bmp_to_png_bytes(path.read_bytes())
        except ValueError as exc:
            print(f'  skip {path.name}: {exc}')
            continue
        dest = (out_dir / (path.stem + '.png')) if out_dir else path.with_suffix('.png')
        dest.write_bytes(png)
        print(f'{dest}  {w}x{h}  {len(png):,} B')
    return 0


if __name__ == '__main__':
    sys.exit(main())
