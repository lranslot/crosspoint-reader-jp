#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/aozora2epub.py の見出し処理を確認する。
実行: python test/aozora2epub/test_headings.py   (引数なし。終了コードが 0 なら合格)

各ケースを実際に変換し、出来上がった EPUB を開いて <body> を検査する。
ctest には載っていない: test/ の他は gtest の C++ で、これは Python のため。
"""
import re
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[2] / 'tools' / 'aozora2epub.py'

# 凡例ヘッダと底本フッタ。本文の前後にあるものなので、どのケースにも付ける。
HEADER = """見出し試験
試験 太郎

-------------------------------------------------------
【テキスト中に現れる記号について】
《》：ルビ
-------------------------------------------------------

"""
FOOTER = "\n底本：「試験」試験書房\n"

# (ケース名, 本文, 出てほしいもの, 出てはいけないもの)
CASES = [
    (
        'インライン形式',
        "［＃５字下げ］第一章［＃「第一章」は大見出し］\n",
        ['<h1>第一章</h1>'],
        [],
    ),
    (
        '別行形式 (大/中/小)',
        "第一章\n［＃「第一章」は大見出し］\n\n"
        "第一節\n［＃「第一節」は中見出し］\n\n"
        "細目\n［＃「細目」は小見出し］\n",
        ['<h1>第一章</h1>', '<h2>第一節</h2>', '<h3>細目</h3>'],
        ['<p>第一章</p>', '<p>第一節</p>', '<p>細目</p>'],
    ),
    (
        '指している文字列が直前の行と不一致',
        "第一章\n［＃「序章」は大見出し］\n",
        ['<p>第一章</p>'],
        ['<h1>第一章</h1>', '<h1>序章</h1>', '序章'],
    ),
    (
        '直前が空行',
        "本文の段落。\n\n［＃「第一章」は大見出し］\n\n次の段落。\n",
        ['<p>本文の段落。</p>', '<p>次の段落。</p>'],
        ['<h1>', '第一章'],
    ),
    (
        'ルビ付きの見出し (別行形式)',
        "真面目《しんめんぼく》\n［＃「真面目」は大見出し］\n",
        ['<h1><ruby>真面目<rt>しんめんぼく</rt></ruby></h1>'],
        ['<p><ruby>真面目'],
    ),
]

# 1行目の作品名と2行目の著者名は全ケース共通で出るので、判定から外す。
BOILERPLATE = ('<h1>見出し試験</h1>', '<p class="author">試験 太郎</p>')


def convert(body, workdir, tag):
    """本文を青空文庫形式の Shift-JIS ファイルにして変換し、<body> の中身を返す。"""
    src = workdir / f'case_{tag}.txt'
    out = workdir / f'case_{tag}.epub'
    src.write_text(HEADER + body + FOOTER, encoding='cp932', errors='replace')
    result = subprocess.run([sys.executable, str(SCRIPT), str(src), str(out)],
                            capture_output=True, text=True, encoding='utf-8')
    if result.returncode != 0:
        raise SystemExit(f'変換に失敗しました:\n{result.stderr}')
    with zipfile.ZipFile(out) as z:
        xhtml = z.read('OEBPS/text.xhtml').decode('utf-8')
    inner = re.search(r'<body>(.*?)</body>', xhtml, re.S).group(1).strip()
    return '\n'.join(l for l in inner.split('\n') if l not in BOILERPLATE)


def main():
    if not SCRIPT.exists():
        raise SystemExit(f'変換スクリプトが見つかりません: {SCRIPT}')

    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        workdir = Path(tmp)
        for n, (name, body, expected, forbidden) in enumerate(CASES, 1):
            got = convert(body, workdir, n)
            problems = [f'出ていない: {e}' for e in expected if e not in got]
            problems += [f'出てはいけない: {f}' for f in forbidden if f in got]

            print(f'--- {n}. {name} ---')
            print(got if got else '(本文なし)')
            if problems:
                failures += 1
                for p in problems:
                    print(f'  NG {p}')
            else:
                print('  OK')
            print()

    if failures:
        print(f'{failures} / {len(CASES)} ケース失敗')
        return 1
    print(f'{len(CASES)} ケース全て合格')
    return 0


if __name__ == '__main__':
    sys.exit(main())
