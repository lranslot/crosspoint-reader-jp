#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""チェックボックスの見本を実寸で生成する (A8 の意匠検討用)。外部ライブラリ不要。

実機のスクリーンショット (1bpp BMP) を読み、**本文はそのまま使って**左の欄の
箱だけを描き替える。合成した字形ではなく端末が実際に描いた字の隣に置くので、
「本文の隣にあるときに見分けられるか」をそのまま判定できる。

欄の寸法もスクリーンショットから実測する。選択中のセルは左余白にある
「べた塗りの矩形」なので、その幅と高さがそのまま markerWidth と lineHeight に
なる。設計メモの名目値 (16 x 20) ではなく、その本の設定での実寸が出る。

使い方:
  python scripts/checkbox_mockup.py <screenshot.bmp|.png> <出力先>
  python scripts/checkbox_mockup.py <screenshot.bmp|.png> <出力先> --side 20

出力:
  contact_1x.png       等倍。候補 x 4状態を本文と並べて縦に積む
  contact_8x.png       8倍。欄だけを格子に並べる (ドットを数える用)
  cell_<候補>_<状態>.png  等倍の欄 1 個ずつ

候補:
  A thick   線幅 2 px のチェック
  B cross   線幅 2 px の対角線 2 本 (x 印)
  C fill    箱の内側を塗りつぶす
  D gap     C の変種。反転時だけ内側に 1 px の隙間を入れる

状態の並びは常に:
  1 未選択・未チェック / 2 未選択・チェック済 /
  3 選択中・未チェック / 4 選択中・チェック済
"""
import pathlib
import struct
import sys
import zlib

INK = 0      # 黒
PAPER = 1    # 白

CANDIDATES = ('A_thick', 'B_cross', 'C_fill', 'D_gap')
STATES = ('1_plain', '2_checked', '3_sel', '4_sel_checked')


# --- BMP / PNG -------------------------------------------------------------

def load_bmp(path):
    """1bpp BMP を [y][x] の 0/1 配列にする。0 = 黒。"""
    data = pathlib.Path(path).read_bytes()
    if data[:2] != b'BM':
        raise ValueError('BMP ではない')
    off = struct.unpack('<I', data[10:14])[0]
    w, h = struct.unpack('<ii', data[18:26])
    if struct.unpack('<H', data[28:30])[0] != 1:
        raise ValueError('1bpp 以外には未対応')
    bottom_up = h > 0
    h = abs(h)
    rb = (w + 31) // 32 * 4
    rows = []
    for y in (range(h - 1, -1, -1) if bottom_up else range(h)):
        line = data[off + y * rb: off + y * rb + rb]
        rows.append([(line[x >> 3] >> (7 - (x & 7))) & 1 for x in range(w)])
    return rows


def load_png(path):
    """このスクリプトと bmp_to_png.py が書く 8bit グレースケール PNG を読む。

    SD カードを挿していなくても見本を作り直せるように用意した (カードは
    いつも挿さっているとは限らない)。フィルタ 0 の 8bit グレースケール
    だけに対応する。他所の PNG は読めない。
    """
    data = pathlib.Path(path).read_bytes()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('PNG ではない')
    pos = 8
    w = h = None
    idat = b''
    while pos < len(data):
        ln = struct.unpack('>I', data[pos:pos + 4])[0]
        tag = data[pos + 4:pos + 8]
        payload = data[pos + 8:pos + 8 + ln]
        if tag == b'IHDR':
            w, h, depth, color = struct.unpack('>IIBB', payload[:10])
            if depth != 8 or color != 0:
                raise ValueError('8bit グレースケール以外には未対応')
        elif tag == b'IDAT':
            idat += payload
        elif tag == b'IEND':
            break
        pos += 12 + ln
    raw = zlib.decompress(idat)
    rows = []
    stride = w + 1
    for y in range(h):
        if raw[y * stride] != 0:
            raise ValueError('フィルタ 0 以外には未対応')
        line = raw[y * stride + 1: y * stride + 1 + w]
        rows.append([PAPER if v >= 128 else INK for v in line])
    return rows


def load_image(path):
    return load_png(path) if str(path).lower().endswith('.png') else load_bmp(path)


def write_png(path, px):
    """[y][x] の 0/1 配列を 8bit グレースケール PNG にする。"""
    h = len(px)
    w = len(px[0])
    raw = b''.join(bytes([0]) + bytes(0xFF if v else 0x00 for v in row) for row in px)

    def chunk(tag, payload):
        body = tag + payload
        return struct.pack('>I', len(payload)) + body + struct.pack('>I', zlib.crc32(body) & 0xFFFFFFFF)

    pathlib.Path(path).write_bytes(
        b'\x89PNG\r\n\x1a\n'
        + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 0, 0, 0, 0))
        + chunk(b'IDAT', zlib.compress(raw, 9))
        + chunk(b'IEND', b''))
    return w, h


# --- スクリーンショットからの実測 ------------------------------------------

def measure(px, probe_width=24):
    """選択中のセル (べた塗りの矩形) を見つけて欄の寸法を返す。

    セルの上端と下端だけが全幅黒で、中ほどは白い箱で途切れる。よって
    「全幅が黒い行」を縦に数えると高さが取れない。**左右の端の列**を縦に
    たどる (セルの端は上から下まで黒い) ことで高さを出す。
    """
    h = len(px)
    w = len(px[0])
    # 欄は左余白にある。本文の字も「幅 12 px・縦に真っ直ぐな端」を満たすこと
    # があるので (口 / 日 など)、探す範囲を左余白に限る。
    lim = min(probe_width, w)

    # 幅 12 px 以上・両脇が白の黒い帯を上から順に試し、**セルの条件を全部
    # 満たすもの**を採る。本文の太い字も幅 12 px の帯を作るので、帯を 1 つ
    # 見つけただけで決めてはいけない (高さ 2 px の字を掴んだことがある)。
    for y0 in range(h):
        x = 0
        while x < lim:
            if px[y0][x] != INK or (x and px[y0][x - 1] == INK):
                x += 1
                continue
            x1 = x
            while x1 < w and px[y0][x1] == INK:
                x1 += 1
            if x1 - x < 12 or x1 >= w or px[y0][x1] != PAPER:
                x = x1 + 1
                continue

            # 端の列をたどって高さを出す (セルの左右端は上から下まで黒い)
            y = y0
            while y + 1 < h and px[y + 1][x] == INK and px[y + 1][x1 - 1] == INK:
                y += 1
            line_h = y - y0 + 1
            # セルの中ほどには白い枠が 2 本現れる
            mid = y0 + line_h // 2
            whites = [xx for xx in range(x, x1) if px[mid][xx] == PAPER]
            side = (whites[-1] - whites[0] + 1) if len(whites) >= 2 else 0
            if line_h >= 12 and side >= 4:
                return dict(gutter_x=x, marker_w=x1 - x, cell_y=y0, line_h=line_h, side=side)
            x = x1 + 1
    raise SystemExit('選択中のセルが見つからない。選択行が写ったスクリーンショットを渡すこと')


# --- 描画 (ファームウェアの drawCheckbox() と同じ算術) ----------------------

def blank(w, h, v=PAPER):
    return [[v] * w for _ in range(h)]


def fill_rect(px, x, y, w, h, v):
    for yy in range(y, y + h):
        if 0 <= yy < len(px):
            for xx in range(x, x + w):
                if 0 <= xx < len(px[0]):
                    px[yy][xx] = v


def draw_rect(px, x, y, w, h, v):
    for xx in range(x, x + w):
        px[y][xx] = v
        px[y + h - 1][xx] = v
    for yy in range(y, y + h):
        px[yy][x] = v
        px[yy][x + w - 1] = v


def _bresenham(px, x1, y1, x2, y2, v, clip):
    dx, dy = abs(x2 - x1), abs(y2 - y1)
    sx = 1 if x1 < x2 else -1
    sy = 1 if y1 < y2 else -1
    err = dx - dy
    x, y = x1, y1
    while True:
        if not (clip and not (clip[0] <= x <= clip[2] and clip[1] <= y <= clip[3])):
            if 0 <= y < len(px) and 0 <= x < len(px[0]):
                px[y][x] = v
        if x == x2 and y == y2:
            break
        e2 = 2 * err
        if e2 > -dy:
            err -= dy
            x += sx
        if e2 < dx:
            err += dx
            y += sy


def draw_line(px, x1, y1, x2, y2, v, width=1, clip=None):
    """GfxRenderer::drawLine(x1, y1, x2, y2, lineWidth, state) と同じ算法。

    ⚠️ ファームの線幅は**下方向にずらすだけ** (`drawLine(x1, y1 + i, ...)`)。
    斜線に対して垂直に太らせるのではないので、45 度の線では見かけの太さが
    width の約 0.71 倍になり、線全体が (width-1)/2 px 下にずれる。
    見本がファームと違う絵を描かないよう、ここでも同じことをする。
    ずれは呼び出し側で始点を上げて相殺する。
    """
    for i in range(width):
        _bresenham(px, x1, y1 + i, x2, y2 + i, v, clip)


def draw_cell(px, gx, gy, marker_w, line_h, side, candidate, checked, selected, stroke=2):
    """欄 1 個を描く。gx, gy は欄の左上。"""
    bx = gx + (marker_w - side) // 2
    by = gy + (line_h - side) // 2

    if selected:
        fill_rect(px, gx, gy, marker_w, line_h, INK)
    ink = PAPER if selected else INK

    draw_rect(px, bx, by, side, side, ink)
    if not checked:
        return

    # 箱の内側 (枠の 1 px を除く)
    ix, iy = bx + 1, by + 1
    iw = ih = side - 2
    clip = (ix, iy, ix + iw - 1, iy + ih - 1)

    if candidate == 'A_thick':
        # 現行と同じ折れ線を線幅 2 px で
        mid_x = bx + side * 5 // 12
        mid_y = by + side // 2
        draw_line(px, bx + side // 6, by + side // 4, mid_x, mid_y, ink, width=stroke, clip=clip)
        draw_line(px, mid_x, mid_y, bx + side * 3 // 4, by + side // 6, ink, width=stroke, clip=clip)
    elif candidate == 'B_cross':
        # 線幅は下にしか伸びないので、下端が内側の最終行にちょうど載るよう
        # 素の線の終点を stroke-1 だけ上げる。はみ出さないので切り取り不要。
        y_end = iy + ih - stroke
        draw_line(px, ix, iy, ix + iw - 1, y_end, ink, width=stroke, clip=clip)
        draw_line(px, ix + iw - 1, iy, ix, y_end, ink, width=stroke, clip=clip)
    elif candidate == 'C_fill':
        fill_rect(px, ix, iy, iw, ih, ink)
    elif candidate == 'D_gap':
        if selected:
            # 白い枠の内側に 1 px の黒い隙間を残してから白く塗る
            fill_rect(px, ix + 1, iy + 1, iw - 2, ih - 2, ink)
        else:
            fill_rect(px, ix, iy, iw, ih, ink)
    else:
        raise ValueError(candidate)


# --- 合成 ------------------------------------------------------------------

def scale(px, n):
    return [[v for v in row for _ in range(n)] for row in px for _ in range(n)]


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if len(args) < 2:
        print(__doc__)
        return 2
    shot, out_dir = pathlib.Path(args[0]), pathlib.Path(args[1])
    out_dir.mkdir(parents=True, exist_ok=True)

    src = load_image(shot)
    g = measure(src)
    side = g['side']
    if '--side' in sys.argv:
        side = int(sys.argv[sys.argv.index('--side') + 1])
    stroke = max(2, side // 6)
    if '--stroke' in sys.argv:
        stroke = int(sys.argv[sys.argv.index('--stroke') + 1])

    print('実測: 欄の左端 x=%d / 欄の幅 %d px / 行の高さ %d px / 現行の箱 %d px'
          % (g['gutter_x'], g['marker_w'], g['line_h'], g['side']))
    print('      箱の上限 min(12, %d, %d) = %d  → 今回描く一辺: %d px'
          % (g['marker_w'] - 4, g['line_h'] - 8, min(12, g['marker_w'] - 4, g['line_h'] - 8), side))
    print('      線幅: %d px' % stroke)

    gx, mw, lh = g['gutter_x'], g['marker_w'], g['line_h']
    text_left = gx + mw
    width = len(src[0])

    # 本文を 1 行まるごと借りる。選択されていない項目の 1 行目を使う。
    text_y = None
    for y in range(g['cell_y'] + lh, len(src) - lh):
        row = src[y]
        if all(row[x] == INK for x in range(gx + (mw - g['side']) // 2,
                                            gx + (mw - g['side']) // 2 + g['side'])) \
                and row[gx] == PAPER:
            text_y = y - (lh - g['side']) // 2
            break
    if text_y is None:
        raise SystemExit('未選択の項目が見つからない')
    text_strip = [src[text_y + i][text_left:width] for i in range(lh)]
    print('      本文は y=%d の行を借用 (x=%d 以降)' % (text_y, text_left))

    # --- 等倍のコンタクトシート -------------------------------------------
    gap = 6
    rows = []
    for ci, cand in enumerate(CANDIDATES):
        for si, state in enumerate(STATES):
            checked = si in (1, 3)
            selected = si in (2, 3)
            line = blank(width, lh)
            for i in range(lh):
                for j, v in enumerate(text_strip[i]):
                    line[i][text_left + j] = v
            draw_cell(line, gx, 0, mw, lh, side, cand, checked, selected, stroke)
            rows.extend(line)
            write_png(out_dir / ('cell_%s_%s.png' % (cand, state)),
                      [r[:text_left + 4] for r in line])
        if ci != len(CANDIDATES) - 1:
            rows.extend(blank(width, gap))
    w, h = write_png(out_dir / 'contact_1x.png', rows)
    print('contact_1x.png  %dx%d' % (w, h))

    # --- 8 倍の格子 --------------------------------------------------------
    n = 8
    pad = 2
    cell_w, cell_h = mw + pad * 2, lh + pad * 2
    grid = blank(cell_w * len(STATES) * n + 8 * (len(STATES) - 1),
                 cell_h * len(CANDIDATES) * n + 8 * (len(CANDIDATES) - 1))
    for ci, cand in enumerate(CANDIDATES):
        for si, state in enumerate(STATES):
            checked = si in (1, 3)
            selected = si in (2, 3)
            cell = blank(cell_w, cell_h)
            draw_cell(cell, pad, pad, mw, lh, side, cand, checked, selected, stroke)
            big = scale(cell, n)
            oy = ci * (cell_h * n + 8)
            ox = si * (cell_w * n + 8)
            for yy, brow in enumerate(big):
                for xx, v in enumerate(brow):
                    grid[oy + yy][ox + xx] = v
    w, h = write_png(out_dir / 'contact_8x.png', grid)
    print('contact_8x.png  %dx%d' % (w, h))
    print('\n縦が候補 %s、横が状態 %s' % (' / '.join(CANDIDATES), ' / '.join(STATES)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
