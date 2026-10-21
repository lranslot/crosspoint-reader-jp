#!/usr/bin/env python3
"""青空文庫形式のテキストを、ruby 付き EPUB に変換する。

対応する記法:
  漢字《かんじ》        → <ruby>漢字<rt>かんじ</rt></ruby>
  ｜文字列《よみ》      → 明示的な範囲指定
  ［＃...］            → 見出し指定は <h*> に、字下げは無視、その他は除去
  ※［＃...］          → 外字。U+xxxx が読めれば実文字に、駄目なら〓

使い方:
  python tools/aozora2epub.py <入力.txt> <出力.epub>

入力の文字コードは Shift-JIS / CP932 / UTF-8 を自動判別する。
1行目を作品名、2行目を著者名として扱う (青空文庫の慣例)。
Python 3.6 以上。標準ライブラリのみで動く。
"""
import html
import re
import sys
import zipfile
from pathlib import Path

# ルビの親文字になりうる文字
KANJI = r'\u4e00-\u9fff\u3005\u3006\u30f6々〆ヶ'
KATAKANA = r'\u30a1-\u30fa\u30fc'
HIRAGANA = r'\u3041-\u3096\u309d\u309e'
ALNUM = r'0-9a-zA-Z０-９Ａ-Ｚａ-ｚ'


def strip_header_footer(lines):
    """青空文庫の凡例ヘッダと底本フッタを落とす。"""
    # ヘッダ: 2本目の "----" 行の直後から本文
    seps = [i for i, l in enumerate(lines) if l.startswith('---') and len(l) > 20]
    start = seps[1] + 1 if len(seps) >= 2 else 0
    # フッタ: "底本：" で始まる行から
    end = len(lines)
    for i, l in enumerate(lines):
        if l.startswith('底本：'):
            end = i
            break
    body = lines[start:end]
    # 前後の空行を落とす
    while body and not body[0].strip():
        body.pop(0)
    while body and not body[-1].strip():
        body.pop()
    return body


def convert_gaiji(text):
    """※［＃...、U+XXXX、...］ を実文字に。読めなければ 〓 に。"""
    def repl(m):
        inner = m.group(1)
        cp = re.search(r'U\+([0-9A-Fa-f]{4,6})', inner)
        if cp:
            try:
                return chr(int(cp.group(1), 16))
            except ValueError:
                pass
        return '〓'
    return re.sub(r'※［＃([^］]*)］', repl, text)


def extract_headings(text):
    """［＃「...」は大見出し］等を拾い、(レベル, 見出し文字列) を返す。"""
    m = re.search(r'［＃「(.+?)」は(大|中|小)見出し］', text)
    if not m:
        return None
    level = {'大': 1, '中': 2, '小': 3}[m.group(2)]
    return level, m.group(1)


def strip_annotations(text):
    """残った ［＃...］ を除去する。"""
    return re.sub(r'［＃[^］]*］', '', text)


def apply_ruby(text):
    """《》記法を <ruby> に変換する。HTML エスケープ後に適用する前提。"""
    # 1) ｜による明示指定が最優先
    text = re.sub(
        r'｜([^《]+)《([^》]*)》',
        lambda m: f'<ruby>{m.group(1)}<rt>{m.group(2)}</rt></ruby>',
        text)

    # 2) 直前の同種文字の連続を親文字とみなす
    def auto(m):
        before, yomi = m.group(1), m.group(2)
        if not yomi:
            return before  # 空ルビは捨てる
        for cls in (KANJI, KATAKANA, ALNUM, HIRAGANA):
            mm = re.search(f'[{cls}]+$', before)
            if mm:
                base = mm.group(0)
                head = before[:mm.start()]
                return f'{head}<ruby>{base}<rt>{yomi}</rt></ruby>'
        return before

    return re.sub(r'(.*?)《([^》]*)》', auto, text)


def build_xhtml(body_lines, title, author):
    parts = []
    for raw in body_lines:
        line = convert_gaiji(raw)
        heading = extract_headings(line)
        line = strip_annotations(line)
        line = line.strip('\u3000 \t')
        if not line:
            continue
        if heading:
            level, _ = heading
            esc = apply_ruby(html.escape(line))
            parts.append(f'<h{level}>{esc}</h{level}>')
        else:
            esc = apply_ruby(html.escape(line))
            parts.append(f'<p>{esc}</p>')

    return f'''<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="ja" lang="ja">
<head>
<meta charset="UTF-8"/>
<title>{html.escape(title)}</title>
</head>
<body>
<h1>{html.escape(title)}</h1>
<p class="author">{html.escape(author)}</p>
{chr(10).join(parts)}
</body>
</html>
'''


def build_epub(out_path, title, author, xhtml):
    container = '''<?xml version="1.0" encoding="UTF-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
<rootfiles>
<rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>
</rootfiles>
</container>
'''
    opf = f'''<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="bookid" xml:lang="ja">
<metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
<dc:identifier id="bookid">urn:uuid:aozora-saishu-sensoron</dc:identifier>
<dc:title>{html.escape(title)}</dc:title>
<dc:creator>{html.escape(author)}</dc:creator>
<dc:language>ja</dc:language>
<meta property="dcterms:modified">2026-08-10T00:00:00Z</meta>
</metadata>
<manifest>
<item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>
<item id="text" href="text.xhtml" media-type="application/xhtml+xml"/>
</manifest>
<spine>
<itemref idref="text"/>
</spine>
</package>
'''
    nav = f'''<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops" xml:lang="ja" lang="ja">
<head><meta charset="UTF-8"/><title>目次</title></head>
<body>
<nav epub:type="toc" id="toc"><h1>目次</h1>
<ol><li><a href="text.xhtml">{html.escape(title)}</a></li></ol>
</nav>
</body>
</html>
'''
    with zipfile.ZipFile(out_path, 'w') as z:
        z.writestr('mimetype', 'application/epub+zip', compress_type=zipfile.ZIP_STORED)
        z.writestr('META-INF/container.xml', container, zipfile.ZIP_DEFLATED)
        z.writestr('OEBPS/content.opf', opf, zipfile.ZIP_DEFLATED)
        z.writestr('OEBPS/nav.xhtml', nav, zipfile.ZIP_DEFLATED)
        z.writestr('OEBPS/text.xhtml', xhtml, zipfile.ZIP_DEFLATED)


def main():
    src = Path(sys.argv[1])
    out = Path(sys.argv[2])
    raw = src.read_bytes()
    for enc in ('shift_jis', 'cp932', 'utf-8'):
        try:
            text = raw.decode(enc)
            break
        except UnicodeDecodeError:
            continue
    else:
        raise SystemExit('文字コードを判別できませんでした')

    text = text.replace('\r\n', '\n').replace('\x85', '\n')
    lines = text.split('\n')
    title = lines[0].strip()
    author = lines[1].strip()
    body = strip_header_footer(lines)
    xhtml = build_xhtml(body, title, author)
    build_epub(out, title, author, xhtml)
    print(f'出力: {out}')
    print(f'ruby 数: {xhtml.count("<ruby>")}')
    print(f'段落数: {xhtml.count("<p>")}')
    print(f'見出し数: h1={xhtml.count("<h1>")} h2={xhtml.count("<h2>")} h3={xhtml.count("<h3>")}')


if __name__ == '__main__':
    main()
