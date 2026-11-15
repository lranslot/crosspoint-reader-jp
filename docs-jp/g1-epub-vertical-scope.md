# G-1 EPUB の縦書き ── 規模の調査

> ★ **調査のみ。実装していない。焼いていない。**
> 読んだ範囲: `lib/Epub/Epub/`（Page, ParsedText, Section, blocks/TextBlock,
> css/CssParser, parsers/ContentOpfParser, parsers/ChapterHtmlSlimParser）、
> `src/activities/reader/EpubReaderActivity.cpp`、
> `DictionaryWordSelectActivity.cpp`、`GfxRenderer.h`。
> 現物: `G:\book\saishu-sensoron.epub`（SD にある唯一の EPUB）。
> 『季節の台所』『テスト文章.epub』は **SD 上に無い** ── 羅闇さんが
> 追記で示した内容を一次資料として使った。

---

## 0. 先に結論

| | |
|---|---|
| **TXT で使った「`renderPage()` の先頭で分岐して戻る」手は使えない** | EPUB の活動はレイアウトを**していない**。座標は cache に焼かれており、活動は再生するだけ |
| **しかしそれは悪い報せではない** | 分岐すべき層が 1 段下に特定できる。**描画は `TextBlock::render` の 1 関数**、**組版は parser の cursor 1 本** |
| **縦書きの難しい部分は済んでいる** | UTR #50 の正立/横転、縦のグリフ描画、禁則表、桁の予算 ── すべて軸に依存しない形で lib 側にある |
| **新しく要るのは「判定」** | TXT には前例が無い。本の宣言を読む部分がまるごと新規 |
| **見積もり** | 段階A（本文だけ縦）で **TXT 段階1 と同程度（0.8〜1.2 倍）**、全部で **1.8〜2.5 倍**。★ 幅の根拠と、**bound できない 1 点**は §4 |

---

## 1. 使い回せる部品

### そのまま呼べる（手当て不要）

| 部品 | 所在 | なぜそのまま呼べるか |
|---|---|---|
| `drawTextVertical(fontId, x, y, cellSize, text, black, style)` | [GfxRenderer.h:378](../lib/GfxRenderer/GfxRenderer.h#L378) | `GfxRenderer` のメソッドで、TXT への依存が無い。1 列まとめて渡す API なので、呼ぶ側は日本語の約物を何も知らなくてよい |
| UTR #50 の正立/横転テーブル | `lib/GfxRenderer/VerticalOrientation.h` | `drawTextVertical` の内側で引かれる。呼ぶ側に露出していない |
| `CjkBreaks::jp` + `LineBreakTables.h` | **`lib/Epub/Epub/`** | ★ **すでに EPUB のライブラリの中にある。**include 1 行で届く |
| `Kinsoku.h` | `src/activities/reader/` | 依存は `<Epub/CjkBreaks.h>` と `<cstddef>` `<cstdint>` だけ。EPUB 側から include できる |
| `PageBudget.h` | 同 | 依存は `<algorithm>` のみ。軸の概念を持たない（「予算」と「消費」しか知らない） |
| `VerticalWrap.h` | 同 | 依存は `<cstddef>` のみ。`advanceCodepoints()` は「何文字入るか」を数えるだけで、縦横を知らない |

★ `Kinsoku.h` / `PageBudget.h` / `VerticalWrap.h` が
`src/activities/reader/` に置かれているのは TXT のためだが、**依存が無いので
置き場所は移動の問題にすぎない**（`lib/Epub/Epub/` か共通の lib へ移すのが筋）。

### 呼ぶのに手当てが要る

| 部品 | 要る手当て | 理由 |
|---|---|---|
| **ルビ・傍点の帯の計算** | ★ **抽出が必要** | `TxtReaderActivity.cpp:708,722` と `TxtReaderActivity.h` の `horizontalLinePitch()` に**活動のメンバとして埋まっている**。式自体（③の置き方＋①の高さ）は軸を問わないが、いまは `cellSize` / `rubyEm` / `boutenDot` を活動の状態から読んでいる |
| ルビの**データ模型** | ★ **作り直しではないが別物** | TXT のルビは青空記法を `AozoraStrip` が剥がして作る。EPUB のルビは **`TextBlock::rubyTexts[wordIndex]` + `RUBY_CONTINUE` スタイル**という別の持ち方で**すでに動いている**。帯の式は移せるが、**呼び出し位置は共有できない** |
| 傍点 | ★ **EPUB には無い** | `TextBlock` に傍点の概念が無い。EPUB の `text-emphasis` は CSS で来るので、やるなら CSS 側の新規項目 |
| `renderPageVertical()` | **移せない** | TXT の実装（[TxtReaderActivity.cpp:2009-2059](../src/activities/reader/TxtReaderActivity.cpp#L2009)、約 50 行）は「文字列の列を順に置く」もので、EPUB の「語ごとに位置と様式を持つ行」を扱えない |

---

## 2. 軸に依存している箇所

★ **行数ではなく箇所の数**で数えた。**連鎖**は「1 本の変数／1 つの概念を通る」という意味で、
直すときに 1 つの設計判断で済むもの。**独立**は別々に判断が要るもの。

### 2.1 まとめ

| 群 | 所在 | 箇所 | 連鎖か独立か |
|---|---|---|---|
| **A. ページの cursor** | `ChapterHtmlSlimParser.cpp` | **4 関数・26 参照** | ★ **連鎖**（`currentPageNextY` 1 本） |
| **B. 行の組み立て** | `ParsedText.{h,cpp}` | **7 関数** | ★ **連鎖**（`pageWidth` と `wordWidths` を通る） |
| **C. 描画** | `TextBlock::render()` | **7 箇所・1 関数** | ★ **連鎖**（`wordX` / `wordY` を通る） |
| **D. 余白と揃え** | `BlockStyle` | **2 概念** | **独立** |
| **E. 画像** | `ChapterHtmlSlimParser.cpp` | **1 箇所** | ★ **独立**（かつ**設計が新規**） |
| **F. 脚注・辞書の座標** | 活動側 | **脚注 0 / 辞書 4** | **独立** |
| **G. 索引・進捗** | `Page`, `ProgressFile` | **0** | ★ **軸に依存していない** |

**合計 25 関数・箇所。うち A/B/C（19）は 3 本の連鎖**。

### 2.2 EpubReaderActivity.cpp（92 KB）は、ほぼ軸を知らない

これは読んで驚いた点である。描画は 6 箇所すべて同じ形:

    page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);

([EpubReaderActivity.cpp:1592,1617,1630,1652](../src/activities/reader/EpubReaderActivity.cpp#L1592) 他)

**座標はページの原点だけで、行の位置も語の位置も活動は見ていない。**
`grep` で `getTextWidth` / `xPos` / `hitTest` は**この 92 KB に 1 件も無い**。

★ **だから「`renderPage()` の先頭で `renderPageVertical()` に分岐して戻る」手は、
使えないのではなく、使う場所が無い。**TXT ではその関数が組版と描画を
両方やっていたから分岐が効いた。EPUB では:

| 層 | TXT | EPUB |
|---|---|---|
| 組版 | 活動の中（読むたび） | **parser の中（cache を書くとき 1 回）** |
| 座標 | その場で計算 | ★ **cache に焼かれている** |
| 描画 | 活動の中 | `TextBlock::render` |

**帰結**: 軸は **cache を書く前に決まっていなければならない**。
実行時に切り替えると、cache の座標と描画の軸が食い違う。
→ `ReaderRenderSpec` に旗を 1 つ足すのが正しい置き場所（§2.8）。

### 2.3 A. ページの cursor ── 連鎖、4 関数

`currentPageNextY` を進める／判定する箇所（[ChapterHtmlSlimParser.cpp](../lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp)）:

| 関数 | 行 | 何をしているか |
|---|---|---|
| 水平罫線 | 358–387 | `availableWidth/4` の幅で中央に置き、`ruleThickness + spacing` 進める |
| 画像 | 737–818 | `containerWidth` で縮小、`displayHeight` 進める、溢れたら改ページ |
| 行 | 1669–1695 | `currentPageNextY + lineHeight > viewportHeight` で改ページ、`xOffset = leftInset()`、`lineHeight` 進める |
| 塊の余白 | 1706–1750 | `marginTop/Bottom`, `paddingTop/Bottom`, `lineHeight/2` を進める |

★ **4 つとも「進む向き = y、幅 = x」という同じ思い込みを 1 本の変数で共有している。**
縦書きでは「進む向き = x（右から左）、幅 = y」になる。
**1 つの概念（along / cross）を導入すれば 4 箇所が同時に直る**
── 逆に言えば、1 箇所だけ直しても意味が無い。

### 2.4 B. 行の組み立て ── 連鎖、7 関数

`ParsedText` の非公開関数はすべて `int pageWidth` と
`std::vector<uint16_t>& wordWidths` を通る
([ParsedText.h:63-79](../lib/Epub/Epub/ParsedText.h#L63)):

`computeLineBreaks` / `computeHyphenatedLineBreaks` / `extractLine` /
`calculateWordWidths` / `calculateRubyExtraStartOffset` /
`calculateRubyExtraEndOffset` / `resolveFirstLineIndent`

★ **折り返しの算法そのものは軸を知らない。**「語の進み量を足して上限を超えたら折る」
だけである。縦書きで変わるのは**何を渡すか**:

| | 横書き | 縦書き |
|---|---|---|
| 上限 | `viewportWidth` | `viewportHeight` |
| 語の進み量 | `getTextAdvanceX()` | **`cellSize × 文字数`**（和文は全角一律） |

後者は `VerticalWrap::advanceCodepoints()` が**すでにやっていること**である。
`calculateWordWidths` に縦の分岐を 1 つ入れれば、残り 6 関数は**引数の意味が
変わるだけで本体は無改造**。ハイフネーションは和文で off なので対象外。

★ ただし `wordWidths` が `uint16_t` 固定で、名前が `width` である。
**意味を変えて名前を残すと、§8.8-29（1 が何かを書く）の事故を自分で作る。**
改名が要る（機械的だが広い）。

### 2.5 C. 描画 ── 連鎖、1 関数の中の 7 箇所

[TextBlock.cpp:129-300](../lib/Epub/Epub/blocks/TextBlock.cpp#L129) の中:

| # | 箇所 | 縦書きでどうなるか |
|---|---|---|
| 1 | `wordX = xposArr[i] + x` | 列方向の `wordY` になる |
| 2 | `wordY = y + rubyShift`（SUP/SUB の上下ずらし） | 列の**左右**にずれる（軸が入れ替わる） |
| 3 | ルビの中央寄せ `leaderWordX - (rubyWidth - groupActualWidth)/2` | y 方向の中央寄せ |
| 4 | ルビの描画位置 `rubyY = wordY - ascender` | **列の右隣**（vertical-rl）。`ascender` ではなく `cellSize` 基準 |
| 5 | focus split の `suffixX` | along 方向の offset |
| 6 | 下線・取消線の `startX/endX/yPos` tracker | 縦の走り（傍線）になる |
| 7 | 装飾幅 `getTextWidth` と em space の読み飛ばし | along 方向の長さ |

★ **7 つとも `wordX` / `wordY` から派生している**ので、
**この 2 つの意味を抽象化すれば 1 つの判断で済む。**
ただし **1 関数に縦横 2 系統が同居する**形になり、
TXT で「壊しようがない形にするほうが強い」（§8.8-14）として
**別関数に分けた**判断とは逆になる。ここは設計の選択が要る。

### 2.6 D. 余白と揃え ── 独立、2 概念

- `BlockStyle::leftInset()` / `rightInset()` ── 縦書きでは**上下**の余白。名前と意味が合わなくなる
- `CssTextAlign { Justify, Left, Center, Right }` ── 縦書きでは Left=上、Right=下。
  ★ **CSS の `text-align: left` を「上」と読み替えるのが正しい**（CSS の論理方向の規定どおり）が、
  **我々の enum は物理方向の名前を持っている**

`CssTextDirection { Ltr, Rtl }` は**すでにある**ので、enum を足す前例はある。

### 2.7 E. 画像 ── 独立、設計が新規

[ChapterHtmlSlimParser.cpp:737-739](../lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp#L737):

    int maxWidth = containerWidth;
    float scaleX = (dims.width > maxWidth) ? (float)maxWidth / dims.width : 1.0f;

横書きでは「幅に合わせて縮め、高さで改ページ」。
縦書きでは「**高さに合わせて縮め、幅で改ページ**」になる。
★ これは名前の付け替えではない ── 和書の挿絵は**見開き前提**のものがあり、
「縦書きで図版をどう置くか」は組版の判断である。**段階A では後回しにできる**（§5）。

### 2.8 G. 索引・進捗 ── ★ 軸に依存していない

これは最も良い報せである。

- `Page::visibleTextOffset` は **「本文の何文字目か」**（zero-based visible codepoint offset）
  ([Page.h](../lib/Epub/Epub/Page.h))。**ページ番号でもピクセル位置でもない**
- `ReaderRenderSpec` は cache の鍵で、**全フィールドが一致しなければ section を捨てて作り直す**
  ([ReaderRenderSpec.h](../lib/Epub/Epub/ReaderRenderSpec.h))

★ **帰結 2 つ:**
1. `ReaderRenderSpec` に `bool vertical` を 1 つ足せば、**cache の無効化は既存の仕組みが勝手にやる。**
   `CACHE_VERSION` の手当ても要らない（spec 不一致で作り直される）
2. **進捗は軸を変えても生き残る。**ページ境界は動くが、「何文字目」は動かない

### 2.9 F. 脚注と辞書の座標 ── ★ 脚注は 0 箇所

★ **脚注にタップの座標計算は無い。**
`Page::footnotes` は `FootnoteEntry {number, href}` の配列（最大 16）で、
利用者は **`EpubReaderFootnotesActivity` の一覧から選ぶ**。
`EpubReaderActivity.cpp` に当たり判定は 1 件も無い。**縦書きで直す所が無い。**

辞書の語選択は座標を持つ（[DictionaryWordSelectActivity.cpp:113-140](../src/activities/reader/DictionaryWordSelectActivity.cpp#L113)）:

| 箇所 | 中身 | 縦書きで |
|---|---|---|
| `WordBox {x, y, width, row}` | 語の矩形 | `width` が along 方向、`row` が**列番号**になる |
| `wordAt(x,y)` | `x..x+width` × `y..y+lineHeight` | 軸が入れ替わる |
| `closestInRow(row, centerX)` | 行内で x 中心が近い語 | `centerY` になる |
| 上下左右の移動 | 行内＝左右、行間＝上下 | **入れ替わる** |

★ ただし **和文で辞書が使えるのかは別問題**である。
`WordBox` は**語に分かれていること**を前提にしており、
日本語には分かち書きが無い。**縦書き化とは独立の課題**なので、
段階C に置くか、そもそも対象外にするかの判断が先に要る。

---

## 3. EPUB 自身が持っている縦書きの情報

### 3.1 (a) 手がかりの一覧と、いまの実装がどこまで読んでいるか

| 手がかり | いまの実装 | 根拠 |
|---|---|---|
| `page-progression-direction` | ★ **見てすらいない** | `<spine>` の開始ハンドラは属性配列を**一切参照しない**（[ContentOpfParser.cpp:134](../lib/Epub/Epub/parsers/ContentOpfParser.cpp#L134)）。`grep` で `progression` が**リポジトリ全体に 0 件** |
| `writing-mode`（標準） | ★ **見てすらいない** | `grep -i "writing-mode"` が `lib/Epub/` `src/` で **0 件** |
| `-epub-writing-mode` | ★ **見てすらいない** | 同上 |
| `-webkit-writing-mode` | ★ **見てすらいない** | 同上 |
| `-epub-line-break: strict` | ★ **見てすらいない** | 同上 |
| `text-combine-upright` | ★ **見てすらいない** | 同上（縦中横。**そもそも描画の機能が無い**） |
| `<link rel="alternate stylesheet">` | ★ **`<link>` を 1 つも見ていない** | `ChapterHtmlSlimParser.cpp` に `"link"` / `rel=` / `stylesheet` が **0 件**。CSS は **OPF の manifest から**集める（[Epub.cpp:139](../lib/Epub/Epub.cpp#L139)） |
| `class` 属性（`.vrtl` など） | **読んでいる** | `CssParser::resolveStyle(tagName, classAttr)` が class セレクタを支持（[CssParser.h:22](../lib/Epub/Epub/css/CssParser.h#L22)） |
| `<html>` 要素の属性 | ★ **`<html>` を見ていない** | parser が扱う開始タグに `html` が無い。`body` から始まる（[ChapterHtmlSlimParser.cpp:397](../lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp#L397)） |

★ **「読んで捨てている」ものは 1 つも無い。全部「見ていない」。**
ただし**基盤は 3 つ揃っている**:

1. **class セレクタが既に動く** ── `.vrtl { ... }` は `resolveStyle("html", "vrtl")` で引ける
2. `CssStyle` は**性質ごとに bit 旗**を持つ構造で、`CssTextDirection{Ltr,Rtl}` という**方向の enum の前例がある**
3. `ReaderRenderSpec` が cache の鍵なので、**旗 1 つで無効化が通る**

### 3.2 ★ 一太郎製の本は、manifest 経由で CSS が「両方」入る

これは羅闇さんの現物確認と我々の実装を突き合わせて出た、
**対応しないと静かに間違う**点である。

『季節の台所』は

    <link rel="stylesheet"           title="横組" class="horizontal" href="stylehorizontal.css"/>
    <link rel="alternate stylesheet" title="縦組" class="vertical"   href="stylevertical.css"/>

だが、我々は `<link>` を読まず **OPF の manifest に挙がっている css を全部**
`CssParser` に流し込んで**累積**する（`loadFromStream` を複数回呼ぶ設計）。

★ **つまり `stylehorizontal.css` と `stylevertical.css` が
「どちらが有効か」の区別なしに混ざる。**
いま縦書きになっていないのは `writing-mode` を見ていないからで、
**見るようにした瞬間、代替スタイルシート側の宣言を本命として拾ってしまう。**
(d) の対応は「あると良い」ではなく**前提**である。

### 3.3 (e) 電書協（現・電書連）EPUB 3 制作ガイドの CSS ── 取得できた

商業出版の和書 EPUB が使う雛形の宣言を確認できた。**我々にとって決定的な点が 1 つある。**

    /* 縦組み用 */
    .vrtl {
      -webkit-writing-mode: vertical-rl;
      -epub-writing-mode:   vertical-rl;
    }

★ **標準の `writing-mode` が無い。**
ガイド本文（v1.1.3 付近）の規定は

> 「`-epub-` 接頭辞が必要なプロパティには `-webkit-` 接頭辞も併記する。」

であり、**標準プロパティの併記は求めていない**。
この点は外部からも指摘されている（「`writing-mode: vertical-rl;` を
付け足したほうがいいんじゃないでしょうか」）。

**帰結**: ★ **標準の `writing-mode` だけを見る実装は、商業出版の和書を 1 冊も縦にしない。**
接頭辞つきの 2 つが本命で、標準は「あれば使う」側である。

雛形はさらに:
- 縦組の本の中で**記事単位に横組**を指定できる設計で、**`html` 要素の class 属性**で切り替える
  （`.vrtl` / `.hltr`）
- → ★ **判定は「本ごと」ではなく「章ごと」になりうる。**我々の cache は section（章）単位なので、
  **これは都合が良い**

取得できなかったもの: ガイドの `style-standard.css` の**全文**。
`.vrtl` の宣言は引用から確定できたが、`.hltr` の中身、
`-epub-line-break` や `text-combine-upright` を雛形が使うかは
**今回の取得範囲では確認できなかった**。★ 必要なら公式配布の雛形一式を取るのが筋。

出典:
- https://momdo.hatenablog.jp/entry/20150310/1425989331 （`.vrtl` の宣言と、標準プロパティ不在の指摘）
- https://denshochan.com/EPUB-3-CSS-5441a8e4823f46df9f2d1d08eaca9a7c
- https://peaceroad.github.io/css-layout/vertical/

### 3.4 (b) 判定の設計案 ── 優先順位つき

★ **1 つの旗では決まらない。**『季節の台所』は
`page-progression-direction="ltr"` なのに縦組である ── 一太郎が
横組を既定にして縦組を代替にしているので、spine の向きは横組に合わせてある。

**`page-progression-direction` は縦書きの判定に使ってはならない。**
あれは**ページが進む向き**であって**組方向**ではない。
（和書で縦組なら普通は `rtl` になるが、この本が反例である。）

| 順位 | 手がかり | 判定 | 根拠の強さ |
|---|---|---|---|
| **1** | 章の `<html>` の class が、`-epub-`/`-webkit-`/標準いずれかの `writing-mode: vertical-*` を持つ規則に一致する | **縦** | ★ 最強。**本が「この章は縦」と言っている**。電書協の雛形がこの形 |
| **2** | `html` / `body` の要素規則が `writing-mode: vertical-*` を持つ | **縦** | 強い。『季節の台所』の `stylevertical.css` がこの形（`html { ... }`） |
| **3** | 2 だが、その宣言が **`rel="alternate stylesheet"` の css に由来**する | ★ **採らない** | 代替は**既定ではない**。一太郎の本はここで落ちる（§3.5 で救う） |
| **4** | どの css も縦を宣言していない | **設定に従う** | `saishu-sensoron.epub` は css を 1 枚も持たない |

★ **判定できないときは設定に従う** ── これで良い、と羅闇さんが示したとおり。
`SETTINGS.verticalWriting` は既に TXT のためにあるので、**設定項目は増えない。**

★ **「本が縦と言っているときだけ縦にする」は実現できる**が、
**それだけでは一太郎の本が縦にならない**（順位 3 で落ちるため）。
一太郎の本を縦にするなら、§3.5 の判断が要る。

### 3.5 (d) 代替スタイルシートに対応する費用

**要る作業**:

| # | 作業 | 規模 |
|---|---|---|
| 1 | `ContentOpfParser` が manifest の css を集める所で、**どの css がどのファイルか**を失わないようにする | いまは全部まとめて流す。**`CssParser` を css ごとに分けるか、規則に出自を持たせる**必要がある |
| 2 | `ChapterHtmlSlimParser` に `<link>` の開始タグを足し、`rel` / `title` / `class` / `href` を取る | `body` の handler と同じ形。小さい |
| 3 | 「`rel="stylesheet"` が既定、`rel="alternate stylesheet"` は候補」という選択を入れる | 設計判断。**候補が複数あるときどれを選ぶか**（`title="縦組"` を見る？ 利用者に選ばせる？） |
| 4 | 選ばれなかった css の規則を**適用しない** | ★ 1 が済んでいないとできない。**ここが本当の費用** |

★ **1 が重い。**`CssParser` は「複数の css を累積して 1 つの規則表にする」設計で、
**出自を保持していない**。分離するには規則表の構造を変えるか、
パーサを 2 個持つことになる。

★ **段階A では「代替を無視する」判断もありうる** ──
その場合、一太郎製の本は**設定の縦書きに従う**（順位 4 に落ちる）。
利用者から見れば「設定を縦にすれば縦で読める」であり、壊れてはいない。
**自動判定の対象から外す、という線引きで段階A を軽くできる。**

### 3.6 (c) `-epub-line-break: strict` と我々の厳格側 ── ★ 同じもの

**同じである。**根拠:

我々の `strict` は **Unicode の CJ クラス**（Conditional Japanese Starter）を
行頭禁止にするかどうかの旗である
([CjkBreaks.h:63-71](../lib/Epub/Epub/CjkBreaks.h#L63)):

> `strict` は JLReq が「本によって緩める」とする文字を決める ── 長音記号、
> 小書きのかな、繰り返し記号。Line_Break の CJ クラスはまさにその区別のために
> あり、だから独立の表になっている。

CSS Text の `line-break` も**同じ区別**で、UAX #14 は CJ を
`strict` では NS（行頭禁止）、`normal`/`loose` では ID（許容）に解決すると規定している。
**CJ クラスは CSS の `line-break` のために作られたクラス**である。

| CSS | 我々の呼び出し |
|---|---|
| `-epub-line-break: strict` / `line-break: strict` | `prohibitedAtLineStart(cp, true)` |
| `normal` / `loose` / 宣言なし | `prohibitedAtLineStart(cp, false)` |

★ **作業量は減る。**`Kinsoku.h` は `constexpr bool STRICT = true` で
既定を厳格にしているので、**宣言を読まなくても商業出版の既定と一致する。**
`-epub-line-break` の読み取りは**あとから足せる精度向上**であって、
段階A の前提ではない。

★ 1 つだけ食い違いが残る: 々（U+3005）を Unicode は NS に置き、
JLReq は緩められる側に置く。我々は Unicode に従っている（手で例外表を
持たないため）。**CSS が `normal` を宣言した本でも 々 は行頭に来ない。**
既知の逸脱として `CjkBreaks.cpp` の表に記録済み。

---

## 4. 見積もり ── TXT 段階1 の何倍か

**基準**: TXT の縦書き段階1 = 約 10 日・複数回の焼き。

### 4.1 安くなる理由（★ 半分は済んでいる）

TXT 段階1 の 10 日のうち、**軸に依存しない部分**は再利用できる:

| 済んでいるもの | TXT 段階1 での重さ |
|---|---|
| UTR #50 の正立/横転と `drawTextVertical` | 重い（参考画像との照合で**5 項目すべて形は合って位置が違う**という事故を含む。§8.8-15） |
| em / pitch の実測（17/21/25/29/33/38 px、advanceY 19〜54） | 重い（外挿してはならない、§8.8-13） |
| 禁則の表の生成と検証（44,400 視覚行を走査） | 重い |
| 桁の予算（`PageBudget`）と 0 での安全側 | 重い（焼き5 のクラッシュ 1 件ぶん。§8.8-27） |
| 進捗・索引の形式 | ★ **EPUB では作業が 0**（§2.8） |

### 4.2 高くなる理由

| | なぜ |
|---|---|
| 組版が 74 KB の parser の中にあり、**解析しながらページを組んでいる** | cursor の抽象化が 4 関数に及ぶ（§2.3） |
| `TextBlock::render` に**装飾・focus・bidi が同居**している | TXT の縦書きは素の文字列しか扱っていない（§2.5） |
| **装飾つき文字列が実在の中身**である | 見出し・太字・引用・箇条書きは EPUB の普通の内容。TXT では青空記法だけだった |
| **判定が新規**（§3） | TXT に前例が無い。CSS の出自分離（§3.5-1）は構造変更 |
| `wordWidths` の**改名が広い** | 意味を変えて名前を残すと §8.8-29 の事故を自作する（§2.4） |

### 4.3 答え

| | 倍率 | 根拠 |
|---|---|---|
| **段階A（本文だけ縦、画像・装飾・辞書は後回し）** | **0.8〜1.2 倍** | 連鎖 3 本（A/B/C の 19 箇所）＋ 判定の最小形。縦書きの難所が済んでいるぶんと、parser 内組版のぶんが打ち消し合う |
| **段階A+B（装飾つき文字列まで）** | **1.3〜1.7 倍** | §2.5 の 7 箇所のうち 2/6/7（SUP/SUB・傍線・装飾幅）と §2.6 が加わる |
| **全部（A+B+C）** | **1.8〜2.5 倍** | 画像の組版判断（§2.7）と辞書の座標（§2.9）。★ 辞書は和文の分かち書きという別課題を抱えている |

### 4.4 ★ bound できない 1 点 ── 正直に書く

**`TextBlock::render` に縦横 2 系統を同居させるか、分けるか**の設計判断の費用が
**見積もれていない。**

- 同居させれば、7 箇所すべてに分岐が入る。**横書きの回帰の危険**を負う
  （この関数は全 EPUB の全ページを描いている）
- 分ければ、ルビ・focus・装飾・bidi の処理が**二重になる**。
  TXT で採った「壊しようがない形」（§8.8-14）はこちらだが、
  TXT の縦書き描画は 50 行で、**こちらは 170 行で中身が濃い**

★ **どちらが安いかは、読んだだけでは分からない。**
片方を**試作して捨てる**（半日〜1 日）のが、見積もりを締める一番安い方法である。
これを**やらずに倍率の幅を狭めると、§8.8-22（解釈した数字と数えた数字が同じ顔で並ぶ）
をまた繰り返す。**

---

## 5. 段階に割る

### 段階A ── 本文だけ縦書き（**ここで一度止められる**）

**やること**: §2.3（cursor）＋ §2.4（行の組み立て）＋ §2.5 の 1/3/4/5
（語の位置・ルビ）＋ §3.4 の順位 1/2/4 の判定＋ `ReaderRenderSpec` の旗。

**やらないこと**: 画像（横書きの置き方のまま／または飛ばす）、
SUP/SUB の軸、傍線、装飾幅、代替スタイルシート、辞書、縦中横。

**価値**: ★ **「日本語の小説の EPUB が縦で読める」**。
根拠: 小説の本文は `<p>` の連なりで、ルビは付くが、画像・表・箇条書きはほぼ無い。
`saishu-sensoron.epub`（css 1 枚も無い、`text.xhtml` のみ）は**まさにこの形**で、
設定の縦書きだけで読めることになる。

**費用**: 0.8〜1.2 倍（§4.3）。★ ただし §4.4 の試作を先に。

**止められるか**: ★ **止められる。**装飾が横書きのまま出ても
（§2.5 の 2/6/7 を飛ばすと、太字や斜体は**位置は正しく、下線だけが横に走る**）、
本文は読める。**下線・取消線が和文の小説にほぼ出ない**ことが効いている。

### 段階B ── 装飾つき文字列

§2.5 の 2/6/7 ＋ §2.6（`leftInset`/`textAlign` の論理化）＋ 水平罫線。

**価値**: 見出しが正しく揃い、強調が崩れない。評論・実用書が読めるようになる。
**費用**: +0.5 倍程度。

### 段階C ── 画像・辞書・縦中横・代替スタイルシート

**価値**: 一太郎製の本の自動判定、挿絵、辞書。
**費用**: +0.5〜0.8 倍。★ 辞書は**和文の分かち書き**という別の設計課題を含む。

### ★ 段階0 ── 1 日で出せるもの（提案）

段階A の前に、**判定だけ**を入れる手がある:

`ReaderRenderSpec` に旗を足し、§3.4 の順位 1/2/4 を実装して、
**判定結果を `/epubmode.txt` に書くだけ**（縦書きの描画はしない）。

★ これで「手持ちの EPUB が何と宣言しているか」が**実機で分かる**。
いまは『季節の台所』と `saishu-sensoron` の 2 冊しか現物を見ていない。
**判定の設計が現物に合っているかを、組版を作る前に確かめられる。**

---

## 6. 報告の要点（5 項目への直接の答え）

1. **使い回せる部品**: 6 件そのまま（`drawTextVertical`、UTR #50、`CjkBreaks`、
   `Kinsoku.h`、`PageBudget.h`、`VerticalWrap.h`）。手当てが要るのは
   ルビ・傍点の帯（活動に埋まっている）。傍点は EPUB に概念が無い。
2. **軸に依存している箇所**: **25**。うち **19 が 3 本の連鎖**
   （cursor 4・行組み立て 7・描画 7+1）。残りは独立。
   ★ **`EpubReaderActivity.cpp` はほぼ軸を知らない**ので
   **「renderPage() で分岐」は使えないが、使う必要が無い**。
   分岐すべきは parser の cursor と `TextBlock::render`。
3. **EPUB の宣言**: ★ **1 つも読んでいない**（捨ててもいない）。
   `page-progression-direction` は組方向ではないので**使ってはならない**。
   電書協の雛形は**標準 `writing-mode` を書かない**ので、
   **接頭辞つきを見ないと商業出版の和書は 1 冊も縦にならない**。
   class セレクタは既に動くので `.vrtl` は引ける。
4. **見積もり**: 段階A **0.8〜1.2 倍**、全部 **1.8〜2.5 倍**。
   ★ `TextBlock::render` を分けるか同居させるかの費用は**見積もれていない**。
   試作 1 日で締まる。
5. **段階**: 段階A で**日本語の小説の EPUB が縦で読める**。止められる。
   ★ その前に**段階0（判定だけ、1 日）**を挟むと、現物で判定を検証できる。
