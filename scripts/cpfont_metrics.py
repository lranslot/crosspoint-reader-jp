#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""`.cpfont` のヘッダと字幅を読む。**読み取り専用。**外部ライブラリ不要。

SD フォントの advanceY / ascender / descender と、任意の文字の advance を
ファイルから直接読む。仕様書に書く数値を推測で埋めないための道具
(§8.7: 一次情報を二次推定で上書きしない)。

出所は `lib/EpdFont/SdCardFont.cpp` の load():
  ヘッダ 32 B    : magic[8] / version u16@8 / flags u16@10 / styleCount u8@12
  スタイル TOC 32 B: styleId u8@0 / intervalCount u32@4 / glyphCount u32@8 /
                     advanceY u8@12 / ascender i16@13 / descender i16@15 /
                     kernLeft u16@17 / kernRight u16@19 /
                     kernLeftClass u8@21 / kernRightClass u8@22 /
                     ligaturePair u8@23 / dataOffset u32@24
  EpdUnicodeInterval 12 B : first u32 / last u32 / offset u32
  EpdGlyph 16 B           : width u8 / height u8 / advanceX u16@2 (12.4) /
                            left i16@4 / top i16@6 / dataLength u16@8 /
                            dataOffset u32@12

使い方:
  python scripts/cpfont_metrics.py G:/.fonts/UDEVGothic
  python scripts/cpfont_metrics.py G:/.fonts/UDEVGothic --chars "> x"
  python scripts/cpfont_metrics.py G:/.fonts/UDEVGothic/UDEVGothic_12.cpfont
"""
import pathlib
import re
import struct
import sys

HEADER_SIZE = 32
TOC_ENTRY_SIZE = 32
INTERVAL_SIZE = 12
GLYPH_SIZE = 16
STYLE_NAMES = {0: 'REGULAR', 1: 'BOLD', 2: 'ITALIC', 3: 'BOLDITALIC'}


def to_pixel(fp):
    """fp4::toPixel — 12.4 固定小数点を四捨五入して px にする。"""
    return (fp + 8) >> 4


class CpFont:
    def __init__(self, path):
        self.path = pathlib.Path(path)
        self.f = open(self.path, 'rb')
        head = self.f.read(HEADER_SIZE)
        if head[:6] != b'CPFONT':
            raise ValueError('magic が CPFONT ではない')
        self.version = struct.unpack('<H', head[8:10])[0]
        self.is2bit = bool(struct.unpack('<H', head[10:12])[0] & 1)
        style_count = head[12]
        self.styles = {}
        for _ in range(style_count):
            t = self.f.read(TOC_ENTRY_SIZE)
            self.styles[t[0]] = dict(
                intervalCount=struct.unpack('<I', t[4:8])[0],
                glyphCount=struct.unpack('<I', t[8:12])[0],
                advanceY=t[12],
                ascender=struct.unpack('<h', t[13:15])[0],
                descender=struct.unpack('<h', t[15:17])[0],
                kernLeftEntryCount=struct.unpack('<H', t[17:19])[0],
                kernRightEntryCount=struct.unpack('<H', t[19:21])[0],
                kernLeftClassCount=t[21],
                kernRightClassCount=t[22],
                ligaturePairCount=t[23],
                dataOffset=struct.unpack('<I', t[24:28])[0],
            )
        self._intervals = {}

    def intervals(self, style=0):
        if style not in self._intervals:
            s = self.styles[style]
            self.f.seek(s['dataOffset'])
            raw = self.f.read(s['intervalCount'] * INTERVAL_SIZE)
            self._intervals[style] = [
                struct.unpack('<III', raw[i * INTERVAL_SIZE:(i + 1) * INTERVAL_SIZE])
                for i in range(s['intervalCount'])]
        return self._intervals[style]

    def glyph_index(self, cp, style=0):
        for first, last, offset in self.intervals(style):
            if first <= cp <= last:
                return offset + (cp - first)
        return None

    def advance_fp(self, cp, style=0):
        """advance を 12.4 固定小数点のまま返す。収録されていなければ None。"""
        idx = self.glyph_index(cp, style)
        if idx is None:
            return None
        s = self.styles[style]
        glyphs_off = s['dataOffset'] + s['intervalCount'] * INTERVAL_SIZE
        self.f.seek(glyphs_off + idx * GLYPH_SIZE)
        return struct.unpack('<H', self.f.read(GLYPH_SIZE)[2:4])[0]

    def kern_classes(self, style=0):
        """(左クラス表, 右クラス表) を {codepoint: classId} で返す。"""
        s = self.styles[style]
        glyphs_off = s['dataOffset'] + s['intervalCount'] * INTERVAL_SIZE
        left_off = glyphs_off + s['glyphCount'] * GLYPH_SIZE
        right_off = left_off + s['kernLeftEntryCount'] * 3
        out = []
        for off, count in ((left_off, s['kernLeftEntryCount']), (right_off, s['kernRightEntryCount'])):
            self.f.seek(off)
            raw = self.f.read(count * 3)
            out.append({struct.unpack('<H', raw[i * 3:i * 3 + 2])[0]: raw[i * 3 + 2]
                        for i in range(count)})
        return out[0], out[1]

    def close(self):
        self.f.close()


def pt_of(path):
    m = re.search(r'_(\d+)\.cpfont$', str(path))
    return int(m.group(1)) if m else None


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    src = pathlib.Path(sys.argv[1])
    files = [src] if src.is_file() else sorted(src.glob('*.cpfont'), key=lambda p: pt_of(p) or 0)
    if not files:
        print('.cpfont が見つからない: %s' % src)
        return 1

    extra = ''
    if '--chars' in sys.argv:
        extra = sys.argv[sys.argv.index('--chars') + 1]

    print('%-5s %-9s %-9s %-10s %-8s %-9s %-8s' %
          ('pt', 'advanceY', 'ascender', 'descender', 'glyphs', 'intervals', '2bit'))
    rows = []
    for path in files:
        fnt = CpFont(path)
        s = fnt.styles[0]
        pt = pt_of(path)
        print('%-5s %-9d %-9d %-10d %-8d %-9d %-8s' %
              (pt, s['advanceY'], s['ascender'], s['descender'],
               s['glyphCount'], s['intervalCount'], fnt.is2bit))
        rows.append((pt, fnt, s))

    # markerWidth = getTextAdvanceX(fontId, "> ")
    print('\n%-5s %-14s %-14s %-14s %-10s' %
          ('pt', "advance('>')", "advance(' ')", 'markerWidth', 'kerning'))
    for pt, fnt, s in rows:
        gt = fnt.advance_fp(0x3E)
        sp = fnt.advance_fp(0x20)
        if gt is None or sp is None:
            print('%-5s 未収録' % pt)
            continue
        kl, kr = fnt.kern_classes()
        # '>' の右側クラスと ' ' の左側クラスが両方 0 ならカーニングは 0
        kerned = (0x3E in kr) and (0x20 in kl)
        print('%-5s %-14s %-14s %-14d %-10s' %
              (pt, '%d (%.4f px)' % (gt, gt / 16),
               '%d (%.4f px)' % (sp, sp / 16),
               to_pixel(gt + sp),
               'あり (要確認)' if kerned else 'なし'))

    # 箱の寸法
    print('\n%-5s %-13s %-13s %-9s %-9s %-26s' %
          ('pt', 'markerWidth', 'lineHeight', '一辺', '線幅', '一辺 vs ascender'))
    for pt, fnt, s in rows:
        gt = fnt.advance_fp(0x3E)
        sp = fnt.advance_fp(0x20)
        if gt is None or sp is None:
            continue
        mw = to_pixel(gt + sp)
        lh = s['advanceY']
        side = min(mw - 4, lh - 8)
        stroke = max(2, side // 6)
        asc = s['ascender']
        verdict = '超過' if side > asc else 'OK (%+d)' % (side - asc)
        print('%-5s %-13d %-13d %-9d %-9d %-26s' % (pt, mw, lh, side, stroke, verdict))

    for ch in extra:
        if ch == ' ':
            continue
        print('\n%-5s advance(%r)' % ('pt', ch))
        for pt, fnt, s in rows:
            a = fnt.advance_fp(ord(ch))
            print('%-5s %s' % (pt, '未収録' if a is None else '%d (%.4f px) -> %d px'
                               % (a, a / 16, to_pixel(a))))

    for _, fnt, _ in rows:
        fnt.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
