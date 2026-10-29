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
from vertical_orientation_table import vo  # noqa: E402

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
    4. TextRotation::Rotated90CW は縦組みには使えない (2026-10-28 に実測)。
       「左 -> 下」に写すため括弧 12 字すべてで向きが逆になる。
       ここでは逆回転 (blit_vertical_rot) で正しい向きを描いている。
       ★ ファーム側には新しい回転を足す必要がある。
================================================================"""


# --- 縦書きの写像 (UTR #50 から機械的に) --------------------------------------
#
#   U   そのままマス中央に置く
#   R   時計回り 90 度回転
#   Tu  縦書き字形の代用。墨をマスの右上へ寄せる (回転しない)
#   Tr  縦書き字形の代用。時計回り 90 度回転
#
# このフォントには縦書き字形 (U+FE10-FE19 / U+FE30-FE4F) が入っていないことを
# 実測済み (仕様書 3.6)。Tu / Tr はここで代用する。
#
# 表は scripts/vertical_orientation_table.py (規格から自動生成)。
# 手で列挙していたときは 30 字抜けていた。
CORNER_MARGIN = 2          # Tu をマスの角から何 px 離すか

# 半角数字を全角数字に写す。
# UTR #50 では半角 0-9 は R (横倒し)、全角 ０-９ は U (正立)。
# 写してから描けば規格どおりになり、実物の組版とも一致する。
# **英字は写さない。**ｐａｒｅｎｔｈｅｓｉｓ は日本語の組版に無い形である。
FULLWIDTH_DIGITS = {chr(0x30 + i): chr(0xFF10 + i) for i in range(10)}


# 段階1 の逸脱: ASCII の英字と記号は正立のまま 1 マスに置く。
# UTR #50 では R (横倒し) だが、**実物の組版が回すのは「語」であって
# 「字」ではない。**1 文字ずつ回しても縦に倒れた字が積まれるだけで、
# 実物の html や "float start" のような横倒しの語にはならない。
# 語をまとめて回すのは段階2。それまでは正立のほうが読める。
# 数字はこの限りでない (全角に写すので規格どおり正立になる)。
STAGE1_UPRIGHT_ASCII = True


def vertical_form(ch, map_digits=True):
    """(実際に描く字, UTR #50 の値) を返す。"""
    if map_digits and ch in FULLWIDTH_DIGITS:
        return FULLWIDTH_DIGITS[ch], 'U'
    v = vo(ch)
    if STAGE1_UPRIGHT_ASCII and v == 'R' and ' ' <= ch <= '~':
        return ch, 'U'
    return ch, v


def corner_offset(g, asc, cell, margin):
    """墨をマスの右上の角に寄せる dx, dy を返す。

    em 比ではなく、グリフの墨の箱から直接求める。フォントが変わっても
    余白が変わらない。利用者がフォントを作れる以上 (仕様書 3.6)、
    比で持つと端末ごとに寄り方が変わってしまう。
    """
    ink_x0 = g['left']
    ink_y0 = asc - g['top']
    want_x0 = cell - margin - g['width']
    want_y0 = margin
    return want_x0 - ink_x0, want_y0 - ink_y0



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


def blit_vertical_rot(page, grid, g, asc, cell_left, cell_top, cell):
    """縦組み用の回転。**ファームの Rotated90CW とは逆向き。**

    実測で分かったこと (2026-10-28): Rotated90CW は「左 → 下」に写す。
    縦組みの約物は「左 → 上」でなければならず、14 字すべてが 180 度ずれる。
    ここでは正しい向きを描く。ファーム側には新しい回転が要る。

      screenX = cell_left + (cell - inkTop - h) + (h - 1 - gy)
      screenY = cell_top  + (cell - left  - w) + gx

    上端の余白が左端の余白に、左端の余白が上端の余白になる — つまり
    「縦の位置」と「横の位置」が入れ替わる。これにより 「 は上、」 は下へ行く。
    """
    w, h = g['width'], g['height']
    ink_top = asc - g['top']
    bx = cell_left + max(0, cell - ink_top - h)
    by = cell_top + max(0, cell - g['left'] - w)
    for gy in range(h):
        for gx in range(w):
            if grid[gy][gx] >= 3:
                continue
            sx, sy = bx + (h - 1 - gy), by + gx
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


def font_dot(fnt):
    """フォント自身の点を 1 つ取り出す。U+2026 の左端の点。

    自前で丸を描くと書体と合わない。字の中の点をそのまま使えば、
    利用者がどのフォントを入れても点の大きさと形が本文と揃う。
    """
    g, grid = fnt.pixels(0x2026)
    cols = [x for x in range(g['width']) if any(grid[y][x] < 3 for y in range(g['height']))]
    runs = []
    for x in cols:
        if runs and x == runs[-1][-1] + 1:
            runs[-1].append(x)
        else:
            runs.append([x])
    first = runs[0]
    dot = [[grid[y][x] for x in first] for y in range(g['height'])]
    return dot, len(first), g['height']


def stamp(page, dot, x, y):
    for gy, row in enumerate(dot):
        for gx, v in enumerate(row):
            if v < 3 and 0 <= y + gy < SCREEN_H and 0 <= x + gx < SCREEN_W:
                page[y + gy][x + gx] = INK


# 三点リーダ・二点リーダは自前で点を描く。
# このフォントの U+2026 は半角 12.5 px の欧文用グリフで、回転しても
# マスの高さに足りない。本来の縦書き字形は全角で点がマスいっぱいに広がる。
# 自前で描くのは、フォントが持っていない縦書き字形を代わりに作ることであり、
# UTR #50 の Tr / R のフォールバックより規格の意図に近い。
LEADER_DOTS = {'…': 3, '‥': 2}


def draw_leader(page, fnt, cell_left, cell_top, cell, count):
    """マスの高さに点を count 個、均等に置く。

    2 マス並べば 2*count 個が等間隔で連続する。
    点はフォント自身の U+2026 から切り出すので書体と揃う。
    """
    dot, dw, dh = font_dot(fnt)
    step = cell / float(count)
    for k in range(count):
        cy = cell_top + int(step * (k + 0.5)) - dh // 2
        stamp(page, dot, cell_left + (cell - dw) // 2, cy)


def draw_cell(page, fnt, asc, cell_left, cell_top, cell, ch,
              with_nudge=True, map_digits=True):
    """1 マスに 1 文字。UTR #50 の値で経路を決める。"""
    ch, v = vertical_form(ch, map_digits)
    if ch in LEADER_DOTS:
        draw_leader(page, fnt, cell_left, cell_top, cell, LEADER_DOTS[ch])
        return
    g, grid = fnt.pixels(ord(ch))
    if g is None:
        return
    if v in ('R', 'Tr'):
        blit_vertical_rot(page, grid, g, asc, cell_left, cell_top, cell)
        return
    dx = dy = 0
    if v == 'Tu' and with_nudge:
        dx, dy = corner_offset(g, asc, cell, CORNER_MARGIN)
    else:
        # U: 半角は送りが半分なのでマスの中央に寄せる
        adv = (g['advanceX'] + 8) >> 4
        if adv < cell:
            dx = (cell - adv) // 2
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

def ink_box(g, asc, dx, dy):
    """マス座標での墨の (x0, y0, x1, y1)。"""
    x0 = g['left'] + dx
    y0 = asc - g['top'] + dy
    return x0, y0, x0 + g['width'] - 1, y0 + g['height'] - 1


def cell_frame(page, x, y, cell, step=3):
    """マスの上辺・下辺を点線で示す。"""
    for k in range(0, cell, step):
        if y < SCREEN_H:
            page[y][x + k] = INK
        if y + cell - 1 < SCREEN_H:
            page[y + cell - 1][x + k] = INK


# --- 確認1: 括弧の回転だけ ---------------------------------------------------

def diag_brackets(fnt, asc, cell, colw, out_dir):
    """括弧だけ。1 字 1 マス、マスの左に横書きでその字を添える。"""
    chars = '（）「」『』〔〕【】〈〉'
    page = blank()
    draw_h(page, fnt, asc, MARGIN_L, 8, '確認1 括弧の回転')
    draw_h(page, fnt, asc, MARGIN_L, 8 + asc + 4, '左の小字=元の形 fixed=正しい向き fw=ファーム')
    top = 8 + (asc + 4) * 2 + 8
    col_fw = SCREEN_W - MARGIN_R - colw
    col_ok = col_fw - colw
    label_x = col_ok - 40
    draw_h(page, fnt, asc, col_fw, top - asc - 4, 'fw')
    draw_h(page, fnt, asc, col_ok, top - asc - 4, 'fixed')
    for j, ch in enumerate(chars):
        y = top + j * cell
        if y + cell > SCREEN_H - MARGIN_B:
            break
        draw_h(page, fnt, asc, label_x, y, ch)            # 元の形 (横書きのまま)
        g, grid = fnt.pixels(ord(ch))
        # fw 列はファームの Rotated90CW をそのまま呼ぶ。
        # draw_cell() は規格駆動で補正版を使うので、ここでは使えない。
        blit(page, grid, g, asc, col_fw, y + cell, True)
        blit_vertical_rot(page, grid, g, asc, col_ok, y, cell)  # 正しい向き
        cell_frame(page, col_fw, y, cell)
        cell_frame(page, col_ok, y, cell)
    write_png(out_dir / 'c1_brackets.png', page)
    write_zoom(out_dir / 'c1_brackets_zoom.png', page,
               label_x - 4, top - 4, colw * 2 + 48, cell * 12 + 8, n=6)


# --- 確認2: 三点リーダだけ ---------------------------------------------------

def diag_ellipsis(fnt, asc, cell, colw, out_dir):
    """c (2 つで 1 マス) と 自前描画 (点 3 つ) の 2 列だけ。"""
    page = blank()
    draw_h(page, fnt, asc, MARGIN_L, 8, '確認2 三点リーダ')
    draw_h(page, fnt, asc, MARGIN_L, 8 + asc + 4, '右 = c 字形2つで1マス / 左 = 自前で点3つ')
    top = 8 + (asc + 4) * 2 + 8
    g26, _ = fnt.pixels(0x2026)
    adv26 = (g26['advanceX'] + 8) >> 4
    dot, dw, dh = font_dot(fnt)

    for i in range(2):
        x = SCREEN_W - MARGIN_R - (i + 1) * colw
        draw_h(page, fnt, asc, x, top - asc - 4, 'c' if i == 0 else 'self')
        for j in range(5):
            y = top + j * cell
            if i == 0:
                # 字形を 2 つ、マスの中に縦に積む (回転して縦の点列にする)
                g, grid = fnt.pixels(0x2026)
                blit(page, grid, g, asc, x, y + cell, True)
                blit(page, grid, g, asc, x, y + cell - adv26, True)
            else:
                # 1 マスに点 3 つ。マスの高さに均等配置。
                # 2 マス続けば 6 点が等間隔になるよう、間隔は cell/3。
                step = cell / 3.0
                for k in range(3):
                    cy = y + round(step * (k + 0.5)) - dh // 2
                    stamp(page, dot, x + (cell - dw) // 2, cy)
            cell_frame(page, x, y, cell)
    write_png(out_dir / 'c2_ellipsis.png', page)
    write_zoom(out_dir / 'c2_ellipsis_zoom.png', page,
               SCREEN_W - MARGIN_R - colw * 2 - 4, top - 4, colw * 2 + 8, cell * 5 + 8, n=8)


# --- 確認3: 、。の寄せだけ ---------------------------------------------------

def diag_kuten(fnt, asc, cell, colw, out_dir, margin=2):
    """いまの em 比と、角に寄せきった版の 2 列だけ。"""
    page = blank()
    draw_h(page, fnt, asc, MARGIN_L, 8, '確認3 句読点の寄せ')
    draw_h(page, fnt, asc, MARGIN_L, 8 + asc + 4, '右 = いまの em 比 / 左 = 角に余白 %d px' % margin)
    top = 8 + (asc + 4) * 2 + 8
    sample = 'あ、い。う、え。'
    report = []
    for i in range(2):
        x = SCREEN_W - MARGIN_R - (i + 1) * colw
        draw_h(page, fnt, asc, x, top - asc - 4, 'corner' if i else 'ratio')
        for j, ch in enumerate(sample):
            y = top + j * cell
            g, grid = fnt.pixels(ord(ch))
            if vo(ch) == 'Tu':
                if i:
                    dx, dy = corner_offset(g, asc, cell, margin)
                else:
                    dx, dy = round(cell * 0.5), round(cell * -0.5)   # 旧: em 比
                if j < 4:
                    report.append((ch, 'corner' if i else 'ratio', dx, dy,
                                   ink_box(g, asc, dx, dy), g))
            else:
                dx = dy = 0
            blit(page, grid, g, asc, x + dx, y + asc + dy, False)
            cell_frame(page, x, y, cell)
    write_png(out_dir / 'c3_kuten.png', page)
    write_zoom(out_dir / 'c3_kuten_zoom.png', page,
               SCREEN_W - MARGIN_R - colw * 2 - 4, top - 4, colw * 2 + 8, cell * 8 + 8, n=8)

    print('\n  マスの大きさ: %d x %d px' % (cell, cell))
    print('  %-4s %-8s %-10s %-26s %s' % ('字', '方式', 'dx, dy', '墨の箱 (x0,y0)-(x1,y1)', '右/上の余白'))
    for ch, mode, dx, dy, (x0, y0, x1, y1), g in report:
        print('  %-4s %-8s %-10s (%2d,%2d)-(%2d,%2d)%10s 右 %d px / 上 %d px'
              % (ch, mode, '%+d, %+d' % (dx, dy), x0, y0, x1, y1, '', cell - 1 - x1, y0))


def diag_digits(fnt, asc, cell, colw, out_dir):
    """半角数字を全角に写した版と、写さない版。"""
    page = blank()
    draw_h(page, fnt, asc, MARGIN_L, 8, '確認4 数字と英字')
    draw_h(page, fnt, asc, MARGIN_L, 8 + asc + 4, '右 = 写さない / 左 = 0-9 を全角に写す')
    top = 8 + (asc + 4) * 2 + 8
    sample = '第3章1980年ABC'
    for i, mapd in enumerate([False, True]):
        x = SCREEN_W - MARGIN_R - (i + 1) * colw
        draw_h(page, fnt, asc, x, top - asc - 4, 'mapped' if mapd else 'raw')
        for j, ch in enumerate(sample):
            y = top + j * cell
            if y + cell > SCREEN_H - MARGIN_B:
                break
            draw_cell(page, fnt, asc, x, y, cell, ch, map_digits=mapd)
            cell_frame(page, x, y, cell)
    write_png(out_dir / 'c4_digits.png', page)
    write_zoom(out_dir / 'c4_digits_zoom.png', page,
               SCREEN_W - MARGIN_R - colw * 2 - 4, top - 4,
               colw * 2 + 8, cell * len(sample) + 8, n=6)


def diag(fnt, asc, em, cell, colw, out_dir):
    diag_brackets(fnt, asc, cell, colw, out_dir)
    diag_ellipsis(fnt, asc, cell, colw, out_dir)
    diag_kuten(fnt, asc, cell, colw, out_dir)
    diag_digits(fnt, asc, cell, colw, out_dir)



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
