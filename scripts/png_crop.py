#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""PNG を切り出して拡大する。外部ライブラリ不要。

参考画像 (`docs-jp/references/`) はカラーの PNG で、
`bmp_to_png.py` が書くグレースケールとは形式が違う。約物の位置を
1 px 単位で見るには切り出しと拡大が要るので、最小限の復号を持つ。

対応: 8bit の グレースケール / グレースケール+α / RGB / RGBA。
      フィルタは 0-4 すべて。インタレースとパレットは非対応。

使い方:
  python scripts/png_crop.py <入力.png> --info
  python scripts/png_crop.py <入力.png> <出力.png> --rect x,y,w,h [--zoom 4]
"""
import pathlib
import struct
import sys
import zlib

CHANNELS = {0: 1, 2: 3, 4: 2, 6: 4}


def read_png(path):
    """(画素 [y][x] = 0-255 の明度, 幅, 高さ) を返す。"""
    data = pathlib.Path(path).read_bytes()
    if data[:8] != b'\x89PNG\r\n\x1a\x0a':
        raise ValueError('PNG ではない')
    pos, idat = 8, b''
    w = h = depth = color = None
    while pos < len(data):
        ln = struct.unpack('>I', data[pos:pos + 4])[0]
        tag = data[pos + 4:pos + 8]
        payload = data[pos + 8:pos + 8 + ln]
        if tag == b'IHDR':
            w, h, depth, color, _, _, interlace = struct.unpack('>IIBBBBB', payload[:13])
            if depth != 8:
                raise ValueError('8bit 以外には未対応 (depth=%d)' % depth)
            if color not in CHANNELS:
                raise ValueError('パレットには未対応 (color=%d)' % color)
            if interlace:
                raise ValueError('インタレースには未対応')
        elif tag == b'IDAT':
            idat += payload
        elif tag == b'IEND':
            break
        pos += 12 + ln

    nch = CHANNELS[color]
    raw = zlib.decompress(idat)
    stride = w * nch
    out = []
    prev = bytearray(stride)
    i = 0
    for _ in range(h):
        ft = raw[i]
        i += 1
        line = bytearray(raw[i:i + stride])
        i += stride
        # PNG のフィルタ解除。a=左, b=上, c=左上。
        for x in range(stride):
            a = line[x - nch] if x >= nch else 0
            b = prev[x]
            c = prev[x - nch] if x >= nch else 0
            if ft == 1:
                line[x] = (line[x] + a) & 0xFF
            elif ft == 2:
                line[x] = (line[x] + b) & 0xFF
            elif ft == 3:
                line[x] = (line[x] + ((a + b) >> 1)) & 0xFF
            elif ft == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x] + pr) & 0xFF
        prev = line
        if nch >= 3:
            row = [(line[x * nch] * 299 + line[x * nch + 1] * 587 + line[x * nch + 2] * 114) // 1000
                   for x in range(w)]
        else:
            row = [line[x * nch] for x in range(w)]
        out.append(row)
    return out, w, h


def write_gray(path, px):
    h, w = len(px), len(px[0])
    raw = b''.join(bytes([0]) + bytes(row) for row in px)

    def chunk(tag, payload):
        body = tag + payload
        return struct.pack('>I', len(payload)) + body + struct.pack('>I', zlib.crc32(body) & 0xFFFFFFFF)

    pathlib.Path(path).parent.mkdir(parents=True, exist_ok=True)
    pathlib.Path(path).write_bytes(
        b'\x89PNG\r\n\x1a\n'
        + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 0, 0, 0, 0))
        + chunk(b'IDAT', zlib.compress(raw, 9))
        + chunk(b'IEND', b''))
    print('  %s  %dx%d' % (path, w, h))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if not args:
        print(__doc__)
        return 2
    px, w, h = read_png(args[0])
    if '--info' in sys.argv or len(args) < 2:
        print('%s  %dx%d' % (args[0], w, h))
        return 0

    def opt(name, default):
        return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default

    x0, y0, cw, ch = (int(v) for v in opt('--rect', '0,0,%d,%d' % (w, h)).split(','))
    n = int(opt('--zoom', '1'))
    crop = [[px[y][x] for x in range(x0, min(x0 + cw, w))]
            for y in range(y0, min(y0 + ch, h))]
    big = [[v for v in row for _ in range(n)] for row in crop for _ in range(n)]
    write_gray(args[1], big)
    return 0


if __name__ == '__main__':
    sys.exit(main())
