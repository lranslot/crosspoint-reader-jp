#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""UTR #50 の Vertical_Orientation から縦書きの回転表を作る。外部ライブラリ不要。

**約物の表を手で列挙するのをやめるための道具。**規格に書いてあるものを
手で写すと必ず抜ける (実際 U+2015 が抜けていた)。

UTR #50 の値:
  U   正立。コード表と同じ向き
  R   横倒し。コード表に対して**時計回りに 90 度**回転
  Tu  縦書き用の別グリフが要る。フォールバックは正立
  Tr  縦書き用の別グリフが要る。フォールバックは時計回り 90 度

データの取得 (リポジトリには含めない):
  curl -o VerticalOrientation.txt \\
    https://www.unicode.org/Public/UCD/latest/ucd/VerticalOrientation.txt

  ⚠️ https://www.unicode.org/Public/vertical/latest/VerticalOrientation.txt は
     404 になる。UCD 側の道を使うこと。

使い方:
  python scripts/gen_vertical_orientation.py <VerticalOrientation.txt>
      我々が扱う範囲の内訳を出す

  python scripts/gen_vertical_orientation.py <file> --chars "、。「」"
      指定した字の値を引く

  python scripts/gen_vertical_orientation.py <file> --diff
      スクリプトの手書きの表と突き合わせる
"""
import pathlib
import sys

# 我々が実際に描く範囲。.cpfont の収録区間 (仕様書 §3.6) と
# 日本語テキストに出るものに絞る。全 Unicode を持ち歩く必要は無い。
RANGES = [
    (0x0020, 0x007E, 'ASCII'),
    (0x00A0, 0x00FF, 'Latin-1'),
    (0x2000, 0x206F, '一般句読点'),
    (0x2190, 0x21FF, '矢印'),
    (0x2500, 0x257F, '罫線'),
    (0x25A0, 0x25FF, '幾何学模様'),
    (0x3000, 0x303F, 'CJK 約物'),
    (0x3040, 0x309F, 'ひらがな'),
    (0x30A0, 0x30FF, 'カタカナ'),
    (0x4E00, 0x9FFF, 'CJK 統合漢字'),
    (0xFF00, 0xFFEF, '半角・全角形'),
]


def load(path):
    """codepoint -> 値 の辞書。ファイルに無いものは既定の 'R'。"""
    vo = {}
    for line in pathlib.Path(path).read_text(encoding='utf-8').splitlines():
        line = line.split('#')[0].strip()
        if not line:
            continue
        rng, _, val = line.partition(';')
        val = val.strip()
        rng = rng.strip()
        if '..' in rng:
            a, b = rng.split('..')
        else:
            a = b = rng
        for cp in range(int(a, 16), int(b, 16) + 1):
            vo[cp] = val
    return vo


def value(vo, cp):
    """UTR #50 の既定値は R (ファイルに載らない符号位置は R)。"""
    return vo.get(cp, 'R')


def main():
    # 端末が cp932 でも落とさない。U+3016 などは印字できない。
    try:
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    except Exception:
        pass
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if not args:
        print(__doc__)
        return 2
    vo = load(args[0])

    if '--chars' in sys.argv:
        chars = sys.argv[sys.argv.index('--chars') + 1]
        print('%-4s %-9s %-4s' % ('字', '符号位置', '値'))
        for ch in chars:
            print('%-4s U+%04X   %-4s' % (ch, ord(ch), value(vo, ord(ch))))
        return 0

    if '--diff' in sys.argv:
        sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
        from vertical_proof import ROTATE
        # 我々が「回転する」と手で決めた集合と、規格で R/Tr のものを比べる
        spec_rot = set()
        for lo, hi, _ in RANGES:
            for cp in range(lo, hi + 1):
                if value(vo, cp) in ('R', 'Tr'):
                    spec_rot.add(chr(cp))
        # 日本語テキストに実際に出る約物だけに絞って比較する。
        # 漢字・かなは U なので対象外、ラテン文字は段階1 で意図的に正立。
        interesting = set('　、。') | {chr(c) for c in range(0x3008, 0x3020)} | \
            {chr(c) for c in range(0x30FB, 0x3100)} | \
            set('—―‥…‐‒–‘’“”') | \
            {chr(c) for c in range(0xFF01, 0xFF65)}
        spec_rot &= interesting
        print('■ 規格では回転 (R / Tr) なのに、手書きの表に無い字')
        missing = sorted(spec_rot - ROTATE)
        for ch in missing:
            print('   %s U+%04X  %s' % (ch, ord(ch), value(vo, ord(ch))))
        print('   計 %d 字' % len(missing))
        print()
        print('■ 手書きの表にあるのに、規格では回転しない字')
        extra = sorted(ROTATE - spec_rot)
        for ch in extra:
            print('   %s U+%04X  %s' % (ch, ord(ch), value(vo, ord(ch))))
        print('   計 %d 字' % len(extra))
        return 0

    if '--emit' in sys.argv:
        # 我々が実際に描く約物だけを Python の表にして吐く。
        # 元データはリポジトリに入れないので、これが唯一の記録になる。
        # ASCII も入れる。段階1 は意図的に規格から外れるので、
        # 「規格では R だが我々はこうする」と書けるよう元の値を残す。
        interesting = ({chr(c) for c in range(0x0020, 0x007F)}
                       | set('　、。')
                       | {chr(c) for c in range(0x3008, 0x3020)}
                       | {chr(c) for c in range(0x30FB, 0x3100)}
                       | set('‐‒–—―‘’“”‥…')
                       | {chr(c) for c in range(0x3041, 0x30FB)}
                       | {chr(c) for c in range(0xFF01, 0xFF65)})
        rows = {}
        for ch in sorted(interesting):
            v = value(vo, ord(ch))
            if v != 'U':          # U は既定なので省く。表が小さくなる。
                rows[ch] = v
        out = [
            '# -*- coding: utf-8 -*-',
            '"""UTR #50 Vertical_Orientation の抜粋。**自動生成。手で編集しない。**',
            '',
            '生成: python scripts/gen_vertical_orientation.py <VerticalOrientation.txt> --emit',
            '取得: curl -o VerticalOrientation.txt',
            '      https://www.unicode.org/Public/UCD/latest/ucd/VerticalOrientation.txt',
            '',
            '値の意味:',
            '  R   横倒し。コード表に対して時計回りに 90 度回転',
            '  Tu  縦書き用の別グリフが要る。フォールバックは正立',
            '  Tr  縦書き用の別グリフが要る。フォールバックは時計回り 90 度',
            '  (U = 正立。既定なのでこの表には載せない)',
            '',
            'この表は我々が描く約物の範囲だけを含む。漢字・かなの本体は U。',
            '"""',
            '',
            'VERTICAL_ORIENTATION = {',
        ]
        for ch, v in sorted(rows.items()):
            out.append("    %-8r: %-6s  # U+%04X" % (ch, repr(v) + ",", ord(ch)))
        out += ['}', '', '',
                'def vo(ch):',
                '    """UTR #50 の値。既定は U (正立)。"""',
                "    return VERTICAL_ORIENTATION.get(ch, 'U')", '']
        dest = pathlib.Path(__file__).resolve().parent / 'vertical_orientation_table.py'
        dest.write_text('\n'.join(out), encoding='utf-8', newline='\n')
        print('%s  %d 字' % (dest, len(rows)))
        return 0

    # 既定: 範囲ごとの内訳
    print('%-18s %-7s %-7s %-7s %-7s %-7s' % ('範囲', 'U', 'R', 'Tu', 'Tr', '計'))
    for lo, hi, name in RANGES:
        c = {'U': 0, 'R': 0, 'Tu': 0, 'Tr': 0}
        for cp in range(lo, hi + 1):
            c[value(vo, cp)] = c.get(value(vo, cp), 0) + 1
        print('%-18s %-7d %-7d %-7d %-7d %-7d'
              % (name, c['U'], c['R'], c['Tu'], c['Tr'], hi - lo + 1))
    print()
    print('■ CJK 約物 (U+3000-303F) の内訳')
    for cp in range(0x3000, 0x3040):
        v = value(vo, cp)
        if v != 'U':
            print('   %s U+%04X  %-3s' % (chr(cp), cp, v))
    return 0


if __name__ == '__main__':
    sys.exit(main())
