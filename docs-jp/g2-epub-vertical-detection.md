# G-2 段階0 ── 縦書きの判定規則（設計のみ。実装前）

> ★ **実装していない。焼いていない。**
> SD の EPUB **5 冊**（4 系統）を zip ごと開いて確認した結果にもとづく。
> 羅闇さんの記録を出発点にしたが、**全件を自分で開き直した**（§8.8-19）。
> その結果 2 点が変わっている ── §0 を先に読んでいただきたい。

---

## 0. ★ 現物を開いて変わったこと（2 点）

### (1) 系統は 4 つではなく、一太郎製が 2 冊ある

`G:\book\テスト文章.epub` は **『季節の台所』と同一構造**だった。

| ファイル | spine | 構造 |
|---|---|---|
| `テスト文章.epub` | `page-progression-direction="ltr"` | ★ **一太郎製**。`stylehorizontal.css` / `stylevertical.css`、`rel="alternate stylesheet" class="vertical"` |

**一太郎系が 2 冊**になったので、この系統は「1 冊の例外」ではなく**現物の 4 割**である。

### (2) AozoraEpub3 の `.vrtl` は `html` にも付いている ── そして**同じ本の中で食い違っている**

羅闇さんの記録は `<div class="main vrtl block-align-center">` だった。これは正しい。
ただし**それは `title.xhtml`（扉）**で、本文の章は違う:

| 文書 | 根の要素 | 子孫 |
|---|---|---|
| `0001.xhtml`（本文） | **`<html class="vrtl">`** | ─ |
| `0002.xhtml`（本文） | **`<html class="vrtl">`** | ─ |
| `0003.xhtml`（本文） | **`<html class="vrtl">`** | ─ |
| `title.xhtml`（扉） | ★ **`<html class="hltr">`** | ★ **`<div class="main vrtl ...">`** |
| `nav.xhtml` | class なし | ─ |

★ **扉は「根が横組、中の塊が縦組」である。**
記録と私の観測は**どちらも正しく、見ていた文書が違った**だけだった。
そして**この 1 文書が、この 5 冊で最も難しい形**である（§3.4）。

同じことが でんでん でも起きている:

| 文書 | class |
|---|---|
| `bodymatter_0_0.xhtml` | **`<body class="bodymatter vrtl">`** |
| `nav.xhtml` | `<body class="frontmatter">`（縦の指定なし） |

★ **判定は本ごとではなく文書（章）ごと**である。目次は横、本文は縦。
我々の cache は section（章）単位なので、**粒度は合っている。**

---

## 1. ★ 設計が変わる決定的な事実 ── 「CSS に書いてある」は「本が選んだ」ではない

羅闇さんが「最重要」とされた点を、**我々の実装側から裏づける事実**が出た。

### 1.1 雛形は常に両方を定義している（確認した原文）

`style-standard.css`（電書協 ver.1.3.1、AozoraEpub3 と でんでん の両方が同梱）:

    /* 横組み用 */
    html, .hltr { writing-mode: horizontal-tb }
    /* 縦組み用 */
    .vrtl       { writing-mode: vertical-rl }

★ **`html` の要素規則そのものが「横組」と言っている。**
縦組は `.vrtl` という**クラス**でしか言われていない。

でんでん はさらに踏み込んでいて、**`html` の要素規則が 2 つの css で矛盾する**:

| css | 内容 | linked か |
|---|---|---|
| `template.css` | `html { -ms-writing-mode: lr-tb; -epub-: horizontal-tb; -webkit-: horizontal-tb; writing-mode: horizontal-tb }` | ★ **されていない**（manifest にはある） |
| `book-style.css` | `html { -ms-writing-mode: tb-rl; -epub-: vertical-rl; -webkit-: vertical-rl; writing-mode: vertical-rl }` | されている |
| `style-standard.css` | `html, .hltr { horizontal-tb }` | されている |

### 1.2 ★ 我々の CSS 置き場には、順序も出自も無い

`Epub.cpp:139` は **OPF の manifest に挙がった css を全部**
`CssParser::loadFromStream()` に流して**累積**する。`<link>` は読まない。

でんでん の manifest: `template.css` / `book-style.css` / `style-standard.css` / `style-advance.css`
── ★ **linked されていない `template.css` も混ざる。**

**帰結**: 素の `html { writing-mode }` を引くと、
**横（template.css・style-standard.css）と縦（book-style.css）が同じ鍵に入る。**
どちらが勝つかは**カスケードの順序で決まるが、我々はその順序を保持していない。**

AozoraEpub3 も同じで、manifest に 8 枚 ──
`style-standard.css`（`html, .hltr` → 横）と `text.css`（`html.vrtl` → 縦）が同居する。

### 1.3 ★ したがって、要素規則は判定に使えない

| | |
|---|---|
| **CSS が縦を「定義」している** | 雛形なら**必ず**している。情報量 0 |
| **本が縦を「選んだ」** | ★ **文書の markup（class）にしか書かれていない** |
| **我々の置き場で要素規則を引く** | ★ **矛盾した値が同じ鍵に入る。証拠にならない** |

**一太郎系でも同じ結論になる**（ただし理由は別）:
`stylevertical.css` は `html { writing-mode: vertical-rl }` を持ち、
`stylehorizontal.css` は素の `html` 規則を**持たない**（`div.page-*` だけ）。
→ 累積すると素の `html` は**縦で一意**になる。
★ **だから要素規則を使うと「縦」と答えてしまう** ── しかしこの本は
**代替スタイルシートなので、既定は横組**である。**要素規則は嘘をつく。**

---

## 2. 判定規則（段階0）

### 2.1 一文で

> ★ **本が、文書の markup のなかで、要素に向かって縦横を名指ししているときだけ判定する。
> スタイルシートに定義があることは根拠にしない。**

### 2.2 効く手がかりは 1 つだけ

**S1 ── class（および inline `style`）経由で要素に届いた writing-mode。
文書のなかで最も根に近いものを採る。**

| 値 | 判定 |
|---|---|
| `vertical-rl` / `vertical-lr` / `tb-rl` / `tb-lr` | **縦** |
| `horizontal-tb` / `lr-tb` | **横** |
| 届かない | **不明 → 設定に従う** |

★ 接頭辞 4 種（`-epub-` / `-webkit-` / `-ms-` / 無印）すべてを同じ性質として読む。
**`-ms-` の旧値 `tb-rl` / `lr-tb` を受けること** ── でんでん は
`-ms-writing-mode: tb-rl` を**最初に**書くので、無印だけ見る実装は
宣言の並びによって取り落とす。

★ **`html` 要素に特別な扱いは要らない。**
`ChapterHtmlSlimParser::startElement` は**すべての開始タグ**について
`class` を取り（:426）、`resolveStyle(name, classAttr)` を呼んでいる（:466）。
`html` も `body` も `div` も、**すでに class ごとスタイル解決されている。**
前回の報告で「`<html>` のハンドラ追加が要る」と書いたが、**要らない。**

### 2.3 根拠にしないもの（記録はする）

| 手がかり | なぜ判定に使わないか |
|---|---|
| 素の要素規則 `html { writing-mode }` | ★ §1.3。矛盾した値が同じ鍵に入る。一太郎系では嘘をつく |
| `page-progression-direction` | ★ **組方向ではなくページの進む向き。**一太郎系は `ltr` で縦組。逆に `rtl` の本の `nav.xhtml` は横組 |
| css のどこかに `vertical-rl` がある | ★ 雛形は必ず持つ。`stylehorizontal.css` ですら `div.page-lefter` で `vertical-rl` を使う |
| `-epub-line-break: strict` | 禁則の強さの指定で、組方向ではない。我々の既定（strict）と一致するので§8.8 的にも触る必要がない |

★ **ただし全部 `/epubmode.txt` に書く。**判定に使わないことと、
記録しないことは別である ── 次に何を直すかは記録から決まる。

### 2.4 (b) `@import` を辿る必要があるか ── ★ 不要。理由は規格

AozoraEpub3 の章は `book-style.css` **1 枚だけ**を link し、
その中身は `@import` 7 枚である:

    style-reset.css, style-standard.css, style-advance.css,
    aozora.css, font.css, text.css, style-check.css

★ **しかし我々は `<link>` を読まず manifest を読む。**
manifest には 8 枚すべてが個別に挙がっており、
**`@import` 先はすべて既に読み込まれている。**

これは偶然ではない: **EPUB 3 は container 内の全資源を manifest に
宣言することを要求する**ので、**実在する `@import` 先は必ず manifest にある。**
（`style-check.css` だけは `@import` されているが manifest にも zip にも
**無い** ── 存在しないので辿る対象でもない。）

**結論**: 段階0 で `@import` の追跡は**不要**。
（将来カスケードの順序を扱うなら話が変わるが、順序は今も持っていない。）

### 2.5 (c) 代替スタイルシート ── 段階0 では対象外、出力で明示する

一太郎系 2 冊は **class を 1 つも持たない**。
したがって S1 が発火せず、**自動的に「不明 → 設定に従う」**になる。
★ **規則を追加しなくてもご指示どおりの答えになる** ── S1 だけにした副産物である。

出力でそれと分かるようにするため、`alt=1` を立てる:
**zip 内に `rel="alternate stylesheet"` を持つ文書がある**ことを記録する
（`<link>` は判定に使わないが、**この旗を立てるためだけに読む**）。

---

## 3. 4 系統（5 冊）での答え合わせ

### 3.1 どの手がかりがどの順で効くか

| 本 | S1: class 経由（★ 唯一効く） | 要素規則（使わない） | spine（使わない） | alt | **判定** |
|---|---|---|---|---|---|
| `saishu-sensoron.epub` | ─（class なし・css 0 枚） | ─ | 属性なし | ─ | **不明 → 設定** |
| `テスト文章.epub`（一太郎） | ─（class なし） | `html`→縦のみ（**嘘**） | `ltr` | **1** | **不明 → 設定** |
| `季節の台所.epub`（一太郎） | ─（class なし） | `html`→縦のみ（**嘘**） | `ltr` | **1** | **不明 → 設定** |
| `[石原莞爾]…AozoraEpub3.epub` 本文 | ★ **`html.vrtl` → vertical-rl** | `html`→横 と `html.vrtl`→縦（**矛盾**） | `rtl` | ─ | **縦** |
| 同 `title.xhtml`（扉） | ★ **`html.hltr` → horizontal-tb**（根） | 同上 | `rtl` | ─ | **横**＋`mixed=div.vrtl` |
| `最終戦争論でんでん.epub` 本文 | ★ **`body.vrtl` → vertical-rl** | `html`→横2枚・縦1枚（**矛盾**） | `rtl` | ─ | **縦** |
| 同 `nav.xhtml` | ─（`body.frontmatter` に縦の指定なし） | 同上 | `rtl` | ─ | **不明 → 設定** |

★ **ご提示の期待と一致する**（4 系統すべて）。
一太郎系は「代替スタイルシートだから対象外」ではなく
**「class で名指ししていないから判定しない」**という同じ 1 本の規則で落ちる。

### 3.2 ★ 扉の食い違いをどう答えるか

`title.xhtml` は **根が `hltr`、中の塊が `vrtl`**。
1 文書に 1 つの答えを出すなら、**根に近いほうを採る**（＝横）。

理由: CSS の継承は根から下へ流れ、子孫の指定は**その塊だけ**を覆す。
扉は「横組の紙面に、縦組の題字の塊を置いた」ものである ──
**文書全体を縦にするのは誤り。**

★ 段階0 では **`mixed=` に食い違いを書き出す**。
段階A で本文を縦にするとき、**この塊だけ横のまま置けるか**が
設計の判断になる（§2.7 の画像と同じ種類の問題）。

---

## 4. `/epubmode.txt` の形

1 文書 1 行。追記。

    <millis> <epub名> doc=<文書名> mode=<v|h|?> by=<手がかり> spine=<値|-> decl=<見つけた宣言> [mixed=<…>] [alt=1]

実例（期待値）:

    12345 saishu-sensoron.epub doc=text.xhtml mode=? by=none spine=- decl=none
    13200 テスト文章.epub doc=document1.xhtml mode=? by=none spine=ltr decl=elem:html=vertical-rl alt=1
    14100 [石原莞爾]…AozoraEpub3.epub doc=0001.xhtml mode=v by=class:html.vrtl spine=rtl decl=vertical-rl
    14900 [石原莞爾]…AozoraEpub3.epub doc=title.xhtml mode=h by=class:html.hltr spine=rtl decl=horizontal-tb mixed=div.vrtl=vertical-rl
    15600 最終戦争論でんでん.epub doc=bodymatter_0_0.xhtml mode=v by=class:body.vrtl spine=rtl decl=vertical-rl

★ **`by=` が本体である。**「縦と判定した」だけでは次に何を直すか分からない
（§8.8-29 と同じ筋 ── 1 が何かを同じ場所に書く）。
`by=` は 4 値だけ: `class:<tag>.<cls>` / `none` / `inline` / `conflict`。

★ `decl=elem:…` は**判定に使わなかった要素規則**を記録するための形。
`mode=?` と `decl=elem:html=vertical-rl` が並ぶ行は
**「CSS は縦と言っているが、本は名指ししていない」**という状態を表す ──
一太郎系を見分ける手掛かりになる。

---

## 5. 実装に要るもの（規模の確認のみ。まだやらない）

| # | 変更 | 規模 | 備考 |
|---|---|---|---|
| 1 | `CssStyle` に `writingMode` と `defined` の 1 bit | 小 | `CssTextDirection{Ltr,Rtl}` が**前例** |
| 2 | `CssParser` に性質 1 つ ── 接頭辞 4 種＋旧値 `tb-rl`/`lr-tb` | 小 | 既存の性質と同じ形 |
| 3 | ★ `CssParser::CSS_CACHE_VERSION` を 8 → 9 | 必須 | CSS cache の形式が変わる |
| 4 | parser で「class 経由で届いた writing-mode」を根に近い順に 1 つ記録 | 小 | `resolveStyle` は**既に全要素で呼ばれている**（:466） |
| 5 | ★ 「class 経由か要素規則か」を区別できるようにする | ★ **ここが唯一の設計** | `resolveStyle` は**合成後の値**を返す。素の要素規則と class 由来を**分けて問える必要がある** |
| 6 | `<link rel="alternate stylesheet">` の有無だけ拾う（`alt=1` 用） | 小 | 判定には使わない |
| 7 | `/epubmode.txt` への 1 行追記 | 小 | `heapdiag::sample` と同じ形 |

★ **5 が唯一の実装上の論点。**`resolveStyle(tag, class)` は
「要素規則 < クラス規則 < 要素.クラス規則」を**合成して**返すので、
返り値からは**どの層が値を入れたか分からない**。
段階0 の規則は**層を区別することが本質**なので、
`resolveStyle` に層別の問い合わせを足すか、
`writingMode` だけ「どの層由来か」を持たせる必要がある。

★ **これを「合成後の値を見ればよい」と誤解したまま実装すると、
一太郎系が「縦」と答える**（§1.3）── つまり**検証が通らない**。
4 系統の答え合わせが、まさにこの誤りを捕まえる試験になっている。

---

## 6. 要点

1. ★ **判定は class だけで行う。**スタイルシートの定義は根拠にしない ──
   雛形は**必ず両方を定義**し、我々の置き場には**順序も出自も無い**ので、
   要素規則は矛盾するか、嘘をつく（一太郎系）
2. ★ **`page-progression-direction` は使わない。**記録するだけ
3. ★ **`@import` は辿らなくてよい。**規格が全資源の manifest 宣言を要求するため、
   実在する `@import` 先は必ず読み込まれている
4. ★ **代替スタイルシートは規則を追加せずに「不明」になる** ── class が無いため。
   `alt=1` で見分けられるようにする
5. ★ **判定は文書（章）ごと。**目次は横、本文は縦。cache の粒度と一致している
6. ★ **扉（`html.hltr` ＋ `div.vrtl`）は根を採って「横」とし、`mixed=` に記録する**
7. ★ **実装の論点は 1 つだけ** ── `resolveStyle` の合成後の値では層が分からない。
   層を区別しないと一太郎系で検証が落ちる
