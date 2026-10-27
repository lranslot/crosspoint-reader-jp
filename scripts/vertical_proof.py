#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""縦書きの試し刷りを PC で出す (F-5 段階1)。外部ライブラリ不要。

`.cpfont` からグリフのビットマップを直接読み、**ファームと同じ算法で**
480x800 の PNG に組む。約物の回転とオフセットを実機に焼かずに詰めるための
道具である。

⚠️ **これは実機ではない。**合わせられない 4 点は走らせるたびに表示する
(`docs-jp/f5-vertical-writing.md` §11.4)。

算法の出所:
  blit            GfxRenderer.cpp:379 renderCharImpl<None> / <Rotated90CW>
  ビットマップ位置 SdCardFont.cpp computeStyleFileOffsets() + 1454 行
  列の座標        f5-vertical-writing.md §2.3

使い方:
  python scripts/vertical_proof.py --diag <出力先>
      確認用 3 項目のページを出す (①回転の規約 ②U+2026 ③小書きかな)

  python scripts/vertical_proof.py <本文.txt> <出力先> [--encoding cp932]
      本文を縦書きに組む

  共通: [--font <.cpfont>] [--ratio 1.3] [--skip N] [--strip-aozora]
"""
import pathlib
import struct
import sys
import zlib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from cpfont_metrics import CpFont  # noqa: E402

SCREEN_W, SCREEN_H = 480, 800
# 12 pt のスクリーンショットから実測した余白 (f5 §11.4 の「合わせられない点 3」)
MARGIN_L, MARGIN_T, MARGIN_R, MARGIN_B = 8, 14, 8, 46

INK, PAPER = 0, 1

NOT_THE_DEVICE = """\
================================================================
  これは PC の試し刷りであって実機ではない。
  合わせられない点 (f5-vertical-writing.md §11.4):
    1. 電子ペーパーの見え方そのもの - 濃度・残像・細い線が読めるか。
       読めるかどうかの判断は実機でしかできない。寸法だけが PC で決まる。
    2. グレースケール (renderAntiAliased) - ここでは BW だけを描く。
    3. viewport の実寸 - 余白とステータスバーは実行時に UITheme が決める。
       ここではスクリーンショットから実測した値を固定で使っている。
    4. TextRotation::Rotated90CW の座標規約 - 横向きラベル用に作られており、
       約物 1 文字に使うときの基準が違う可能性がある (確認項目 ①)。
================================================================"""


# --- 約物の表 (初期値。PC で詰める) -----------------------------------------
# 値は em 比。dx は右が正、dy は下が正。
# 回転する字。U+2015 (――) は青空文庫のダッシュで実際のテキストに出る。
# 初版は入れ忘れており、実際の本文を食わせて気づいた。
ROTATE = set('「」『』（）〔〕〈〉【】［］｛｝〝〟'
             'ー〜‥…'
             '—―'      # — EM DASH / ― HORIZONTAL BAR
             '～')            # ～ FULLWIDTH TILDE
SMALL_KANA = set('ぁぃぅぇぉっゃゅょゎヵヶァィゥェォッャュョヮ')
NUDGE_TOP_RIGHT = {'、': (0.5, -0.5), '。': (0.5, -0.5)}
SMALL_KANA_NUDGE = (0.10, -0.10)


def rule(ch, with_nudge=True):
    """(回転するか, dx比, dy比) を返す。

    with_nudge=False は「寄せを一切しない」状態で、見本の比較用。
    """
    if ch in ROTATE:
        return True, 0.0, 0.0
    if with_nudge and ch in NUDGE_TOP_RIGHT:
        return False, *NUDGE_TOP_RIGHT[ch]
    if with_nudge and ch in SMALL_KANA:
        return False, *SMALL_KANA_NUDGE
    return False, 0.0, 0.0


# --- 描画 (GfxRenderer.cpp:379 の写し) ---------------------------------------

def blit(page, grid, g, ascender, cursor_x, cursor_y, rotated):
    """renderCharImpl<rotation> と同じ座標計算で 1 グリフを置く。

    cursor_y はベースライン (drawText が y + ascender として渡す値)。
    BW モード: 2-bit 値が 3 未満なら黒 (灰も黒で塗る)。
    """
    if grid is None:
        return
    if rotated:
        outer_base = cursor_x + ascender - g['top']   # screenX = outer_base + glyphY
        inner_base = cursor_y - g['left']             # screenY = inner_base - glyphX
    else:
        outer_base = cursor_y - g['top']              # screenY = outer_base + glyphY
        inner_base = cursor_x + g['left']             # screenX = inner_base + glyphX
    for gy in range(g['height']):
        outer = outer_base + gy
        for gx in range(g['width']):
            if grid[gy][gx] >= 3:
                continue
            if rotated:
                sx, sy = outer, inner_base - gx
            else:
                sx, sy = inner_base + gx, outer
            if 0 <= sx < SCREEN_W and 0 <= sy < SCREEN_H:
                page[sy][sx] = INK


def draw_h(page, fnt, asc, x, y, text):
    """横書きで 1 行。見本の見出し用。y は字の上端。"""
    pen = x
    for ch in text:
        g, grid = fnt.pixels(ord(ch))
        if g is None:
            continue
        blit(page, grid, g, asc, pen, y + asc, False)
        pen += (g['advanceX'] + 8) >> 4
    return pen


def draw_cell(page, fnt, asc, cell_left, cell_top, cell, ch, with_nudge=True):
    """1 マスに 1 文字。cell_left/cell_top はマスの左上。"""
    g, grid = fnt.pixels(ord(ch))
    if g is None:
        return
    rot, dxr, dyr = rule(ch, with_nudge)
    dx, dy = round(cell * dxr), round(cell * dyr)
    if rot:
        # Rotated90CW: screenX = cx + ascender - top + glyphY
        #              screenY = cy - left - glyphX
        # マスの左上を基準に、字がマスに収まるよう cx/cy を選ぶ。
        blit(page, grid, g, asc, cell_left + dx, cell_top + cell + dy, True)
    else:
        blit(page, grid, g, asc, cell_left + dx, cell_top + asc + dy, False)


# --- PNG ---------------------------------------------------------------------

def blank():
    return [[PAPER] * SCREEN_W for _ in range(SCREEN_H)]


def write_png(path, px):
    h, w = len(px), len(px[0])
    raw = b''.join(bytes([0]) + bytes(0xFF if v else 0x00 for v in row) for row in px)

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


def write_zoom(path, page, x0, y0, w, h, n=6):
    """墨のある範囲を n 倍で切り出す。1 px 単位で確かめるため。"""
    x0, y0 = max(0, x0), max(0, y0)
    crop = [[page[y][x] for x in range(x0, min(x0 + w, SCREEN_W))]
            for y in range(y0, min(y0 + h, SCREEN_H))]
    write_png(path, [[v for v in row for _ in range(n)] for row in crop for _ in range(n)])


# --- 確認用の 3 項目 ---------------------------------------------------------

def diag(fnt, asc, em, cell, colw, out_dir):
    """① 回転の規約 ② U+2026 ③ 小書きかな。"""
    cols_x = lambda i: MARGIN_L + (SCREEN_W - MARGIN_L - MARGIN_R) - (i + 1) * colw

    # --- ① Rotated90CW の規約 ---
    page = blank()
    draw_h(page, fnt, asc, MARGIN_L, 8, '確認1 回転の規約')
    draw_h(page, fnt, asc, MARGIN_L, 8 + asc + 6, '左列=回転あり 右列=回転なし')
    top = 8 + (asc + 6) * 2 + 10
    for i, text in enumerate(['「あ」（い）', '「あ」（い）']):
        x = cols_x(i)
        draw_h(page, fnt, asc, x, top - asc - 4, 'rot' if i else 'none')
        for j, ch in enumerate(text):
            # 右の列 (i=0) は回転させずに描いて、横書きのままの形を見せる
            if i == 0:
                g, grid = fnt.pixels(ord(ch))
                if g:
                    blit(page, grid, g, asc, x, top + j * cell + asc, False)
            else:
                draw_cell(page, fnt, asc, x, top + j * cell, cell, ch)
    # マスの枠を薄く示す (1 px の点線)
    for i in range(2):
        x = cols_x(i)
        for j in range(6):
            y = top + j * cell
            for k in range(0, cell, 3):
                page[y][x + k] = INK
                if y + cell - 1 < SCREEN_H:
                    page[y + cell - 1][x + k] = INK
    write_png(out_dir / 'diag1_rotation.png', page)
    write_zoom(out_dir / 'diag1_rotation_zoom.png', page, cols_x(3) - 4, top - 6, colw * 4 + 12, cell * 6 + 12)

    # --- ② U+2026 の扱い 3 通り + U+2025 ---
    page = blank()
    draw_h(page, fnt, asc, MARGIN_L, 8, '確認2 三点リーダ')
    draw_h(page, fnt, asc, MARGIN_L, 8 + asc + 6, 'a=中央寄せ b=回転 c=2つで1マス d=二点')
    top = 8 + (asc + 6) * 2 + 10
    g26, _ = fnt.pixels(0x2026)
    adv26 = (g26['advanceX'] + 8) >> 4 if g26 else 0
    for i, mode in enumerate(['a', 'b', 'c', 'd']):
        x = cols_x(i)
        draw_h(page, fnt, asc, x, top - asc - 4, mode)
        for j in range(4):
            y = top + j * cell
            if mode == 'a':      # 1 マスに中央寄せ、回転なし
                g, grid = fnt.pixels(0x2026)
                blit(page, grid, g, asc, x + (cell - adv26) // 2, y + asc, False)
            elif mode == 'b':    # 1 マスに中央寄せ、90 度回転
                g, grid = fnt.pixels(0x2026)
                blit(page, grid, g, asc, x + (cell - adv26) // 2, y + cell, True)
            elif mode == 'c':    # 2 つで 1 マス (回転)
                g, grid = fnt.pixels(0x2026)
                blit(page, grid, g, asc, x, y + cell, True)
                blit(page, grid, g, asc, x, y + cell - adv26, True)
            else:                # ‥ U+2025 (全角)
                draw_cell(page, fnt, asc, x, y, cell, '‥')
        for j in range(4):
            y = top + j * cell
            for k in range(0, cell, 3):
                page[y][x + k] = INK
    write_png(out_dir / 'diag2_ellipsis.png', page)
    write_zoom(out_dir / 'diag2_ellipsis_zoom.png', page, cols_x(3) - 4, top - 6, colw * 4 + 12, cell * 6 + 12)

    # --- ③ 小書きかな ---
    page = blank()
    draw_h(page, fnt, asc, MARGIN_L, 8, '確認3 寄せの比較')
    draw_h(page, fnt, asc, MARGIN_L, 8 + asc + 6, '左=オフセットあり 右=なし')
    top = 8 + (asc + 6) * 2 + 10
    sample = 'あ、い。うっえょおぁかぃ'
    for i, nudge in enumerate([False, True]):
        x = cols_x(i)
        draw_h(page, fnt, asc, x, top - asc - 4, 'nudge' if nudge else 'none')
        for j, ch in enumerate(sample):
            y = top + j * cell
            if y + cell > SCREEN_H - MARGIN_B:
                break
            draw_cell(page, fnt, asc, x, y, cell, ch, with_nudge=nudge)
            for k in range(0, cell, 3):
                page[y][x + k] = INK
    write_png(out_dir / 'diag3_small_kana.png', page)
    write_zoom(out_dir / 'diag3_small_kana_zoom.png', page, cols_x(3) - 4, top - 6, colw * 4 + 12, cell * 6 + 12)


def strip_aozora(text):
    """青空文庫記法のごく簡単な除去。

    ⚠️ **ファームの lib/Aozora/AozoraStrip.cpp とは別物である。**
    ここでやるのは注記 ［＃…］ とルビ ｜親《ルビ》 を落とすことだけで、
    本家の挙動を再現したものではない。約物の見え方を確かめるために
    記法が本文に混ざるのを避けるのが目的。
    """
    out = []
    i = 0
    while i < len(text):
        c = text[i]
        if c == '［' and text.startswith('［＃', i):
            j = text.find('］', i)
            i = len(text) if j < 0 else j + 1
            continue
        if c == '《':
            j = text.find('》', i)
            i = len(text) if j < 0 else j + 1
            continue
        if c == '｜':
            i += 1
            continue
        out.append(c)
        i += 1
    return ''.join(out)


# --- 本文の組版 --------------------------------------------------------------

def typeset(fnt, asc, em, cell, colw, text, out_dir):
    vw = SCREEN_W - MARGIN_L - MARGIN_R
    vh = SCREEN_H - MARGIN_T - MARGIN_B
    cols = vw // colw
    chars = vh // cell
    print('  列数=%d 1列の字数=%d (1ページ %d 字)' % (cols, chars, cols * chars))
    page = blank()
    col = 0
    row = 0
    for ch in text:
        if ch == '\n':
            col += 1
            row = 0
            if col >= cols:
                break
            continue
        if row >= chars:
            col += 1
            row = 0
            if col >= cols:
                break
        x = MARGIN_L + vw - (col + 1) * colw + (colw - cell) // 2
        draw_cell(page, fnt, asc, x, MARGIN_T + row * cell, cell, ch)
        row += 1
    write_png(out_dir / 'page1.png', page)
    # 右端 4 列を 6 倍。約物の位置はここで見る。
    write_zoom(out_dir / 'page1_zoom.png', page,
               MARGIN_L + vw - colw * 4, MARGIN_T, colw * 4, cell * 8)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    def opt(name, default=None):
        return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default

    font = opt('--font', 'G:/.fonts/UDEVGothic/UDEVGothic_12.cpfont')
    ratio = float(opt('--ratio', '1.3'))
    diag_mode = '--diag' in sys.argv

    if not args or (not diag_mode and len(args) < 2):
        print(__doc__)
        return 2

    print(NOT_THE_DEVICE)
    fnt = CpFont(font)
    st = fnt.styles[0]
    asc = st['ascender']
    g, _ = fnt.pixels(ord('亜'))
    em = (g['advanceX'] + 8) >> 4
    cell = em
    colw = round(em * ratio)
    print('\nフォント: %s' % font)
    print('em=%d ascender=%d advanceY=%d  列幅=%d (em x %.2f)' % (em, asc, st['advanceY'], colw, ratio))
    print()

    if diag_mode:
        out = pathlib.Path(args[0])
        out.mkdir(parents=True, exist_ok=True)
        diag(fnt, asc, em, cell, colw, out)
    else:
        enc = opt('--encoding', 'cp932')
        raw = pathlib.Path(args[0]).read_bytes()
        try:
            text = raw.decode(enc)
        except UnicodeDecodeError:
            text = raw.decode('utf-8', 'replace')
            print('  (%s で読めなかったので utf-8 として読んだ)' % enc)
        text = text.replace('\r\n', '\n')
        if '--strip-aozora' in sys.argv:
            text = strip_aozora(text)
        skip = int(opt('--skip', '0'))
        if skip:
            text = text[skip:]
        out = pathlib.Path(args[1])
        out.mkdir(parents=True, exist_ok=True)
        typeset(fnt, asc, em, cell, colw, text, out)
    return 0


if __name__ == '__main__':
    sys.exit(main())
