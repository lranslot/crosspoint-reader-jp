#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""UI フォントに焼き込むべき非 ASCII 文字を翻訳ファイルから抽出する (Phase B)。

出所: docs-jp/a1-ui-font-subset.md §7 の本文にあったもの。文書の中のコードは
走らせられないので実ファイルにした (aozora2epub.py と同じ失われ方を避ける)。

japanese.yaml を編集するたびに文字集合は変わる。翻訳を 1 つ足すだけで
焼き込む字が増えるため (「推定」の「推」がそうだった — 仕様書 §8.6-1)、
Phase B ではこれをビルド時に走らせるのが確実。

使い方:
  python scripts/gen_ui_charset.py              # 内訳を表示
  python scripts/gen_ui_charset.py --intervals  # fontconvert.py 用の引数を出力
  python scripts/gen_ui_charset.py --chars      # 文字だけを 1 行で出力
"""
import pathlib
import re
import sys

TRANSLATIONS = pathlib.Path(__file__).resolve().parent.parent / 'lib' / 'I18n' / 'translations'

# 翻訳ファイルに現れないが UI フォントで描かれる文字。
# GfxRenderer::truncatedText() が付ける省略記号 (U+2026)。
EXTRA = '…'


def values(path: pathlib.Path) -> str:
    """KEY: "value" の値だけを連結して返す。"""
    out = []
    for line in path.read_text(encoding='utf-8').splitlines():
        m = re.match(r'^([A-Za-z_][A-Za-z0-9_]*):\s*"(.*)"\s*$', line.rstrip())
        if m:
            out.append(m.group(2).replace('\\n', '\n').replace('\\"', '"'))
    return ''.join(out)


def classify(ch: str) -> str:
    cp = ord(ch)
    if 0x4E00 <= cp <= 0x9FFF or ch in '々〆ヶ':
        return '漢字'
    if 0x3040 <= cp <= 0x309F:
        return 'ひらがな'
    if 0x30A0 <= cp <= 0x30FF:
        return 'カタカナ'
    if 0x3000 <= cp <= 0x303F:
        return '和文約物'
    if 0xFF00 <= cp <= 0xFFEF:
        return '全角記号'
    return 'その他'


def main() -> int:
    blob = (values(TRANSLATIONS / 'japanese.yaml')
            + values(TRANSLATIONS / 'english.yaml')
            + EXTRA)
    chars = sorted({c for c in blob if ord(c) > 0x7F})

    if '--intervals' in sys.argv:
        # fontconvert.py 用。1 文字ずつの区間として出す。
        for c in chars:
            print(f'--additional-intervals 0x{ord(c):04X},0x{ord(c):04X}')
        print(f'# {len(chars)} characters', file=sys.stderr)
        return 0

    if '--chars' in sys.argv:
        print(''.join(chars))
        return 0

    from collections import Counter
    counts = Counter(classify(c) for c in chars)
    print(f'非 ASCII 合計: {len(chars)} 字')
    for kind in ('漢字', 'ひらがな', 'カタカナ', '和文約物', '全角記号', 'その他'):
        if counts[kind]:
            print(f'  {kind:8s} {counts[kind]:4d}')
    print()
    print('ASCII 95 字は別途すべて含める (Ubuntu / NotoSans に既にある)。')
    print()
    kanji = ''.join(c for c in chars if classify(c) == '漢字')
    print(f'漢字 ({len(kanji)} 字):')
    for i in range(0, len(kanji), 40):
        print(f'  {kanji[i:i + 40]}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
