# F-2: Markdown チェックボックスのトグル — 設計と実装記録

**版数**: v0.2
**作成日**: 2026-08-10 (v0.1) → **改訂**: 2026-08-13 (v0.2)
**位置づけ**: `docs-jp/xteink-x4-jp-firmware-spec-v0_6.md` §6.2 の詳細設計
**状態**: **段階1 実装完了。実機で動作確認済み。**

> **v0.2 での性格の変化**: v0.1 は着手前の設計案だった。
> v0.2 は**実装された内容の記録**である。v0.1 の提案のうち複数が実装時に覆っており、
> 本書は現物に合わせて書き直してある。覆った項目は §5 に一覧で残した。

---

## 0. これは何をする機能か

`.md` ファイル内の Markdown チェックボックスを、**端末上でトグルする。**

`- [ ] 牛乳を買う` にカーソルを合わせて電源ボタンを短く押すと `- [x] 牛乳を買う` になり、
**SD 上のファイルにも1バイトだけ書き戻される。**

**プロジェクトの原点。**スマホで作った TODO を端末で消化する。

### 上流との関係

`SCOPE.md` の Out-of-Scope に「No typed notes, journals, or editors」、
`ROADMAP.md` の Out of Roadmap に「Writing / authoring tools」。
さらに ROADMAP に **「But it was on the old roadmap is not a valid argument」**と明記。

**上流では通らない。Discussion で確認する必要もない。フォークで実装した。**

---

## 1. 実装の結果

| 項目 | 結果 |
|---|---|
| 実装日 | 2026-08-13 |
| 対象 | `.md` のみ (`FsHelpers::hasMarkdownExtension`) |
| Flash | **+2,142 B** (5,590,927 → 5,593,069 B / 85.3%) |
| RAM (静的) | **±0** — 追加した状態はすべて `TxtReaderActivity` のインスタンスメンバ (ヒープ) |
| 残 Flash | 約 938 KB |
| 体感 | **快適。**ページ送りと同程度で、引っかかりは無い |

### 変更ファイル

```
src/activities/reader/TxtReaderActivity.cpp   (中核)
src/activities/reader/TxtReaderActivity.h
src/CrossPointSettings.h                      (SHORT_PWRBTN::CHECKBOX = 5)
src/SettingsList.h                            (enumValues 末尾に追加)
lib/I18n/translations/english.yaml            (STR_CHECKBOX_TOGGLE)
lib/I18n/translations/japanese.yaml           (「チェック切替」)
```

### 実機で確認できたこと

- 記法のゆれ (`-` / `*` / `+`)、インデント (スペース・タブ)、`[x]` / `[X]` の検出
- **偽物の除外** — `-[ ]` / `- []` / `- [y]` / `[ ]` (箇条書き記号なし) はいずれも素通り
- カーソルの移動と着地点 (前方から入れば先頭、後方から入れば末尾)
- トグルと**ファイルへの書き戻し** (PC で開き直して確認)
- チェック状態を変えても総ページ数が変わらない

---

## 2. 確定した設計

### 2.1 入力

| ボタン | チェックボックスがあるとき | 無いとき |
|---|---|---|
| **電源・短押し** | **トグル** (設定が「チェック切替」のとき) | 何も起きない |
| **側面** | 次/前のチェックボックスへ | 通常のページ送り |
| 前面4つ | ページ送り・戻る | 同左 |

**片手で完結し、持ち替えが要らない。**

#### 原則: カーソルが実際に動くときだけ消費する

側面ボタンを F-2 が横取りするかを、状態ではなく**結果**で決める。
押した結果カーソルが動くなら消費し、動かないならページ送りに渡す。

- チェックボックスが 0 個または 1 個 → 消費しない
- 末尾で「次へ」/ 先頭で「前へ」 → 消費しない

**死んだボタンを作らないための規則。**「押しても何も起きない」はユーザーには故障と区別が付かない。

#### `Button::Up` / `Down` ではなく `PageBack` / `PageForward` を使う

`MappedInputManager.cpp:70-77` の `Up`/`Down` は `sideButtonLayout` 設定を素通りする。
`PageBack`/`PageForward` (`:81-98`) を使えば、
**`SIDE_BUTTONS_DISABLED` の尊重と `NEXT_PREV` の向き反転が自動で効く。**

#### 電源ボタン短押し

`CrossPointSettings.h:133` の `SHORT_PWRBTN` に `CHECKBOX = 5` を**末尾に**追加。

> **末尾でなければならない。**同ファイル L138-141 のコメントのとおり、この種の enum は
> `settings.json` に index で永続化されるため、途中に挿入すると既存の設定が
> 黙って別物として解釈される。

`POWER + DOWN` はスクリーンショットの組み合わせ (`main.cpp:503`) だが、
`loop()` の先頭で捕まって `return` するのでアクティビティには届かない。
念のため `EpubReaderActivity.cpp:580-582` (脚注) に倣って `!wasReleased(Button::Down)` でも守っている。

### 2.2 マーカー — ASCII の `>`

**`▶` (U+25B6) は使えない。**

| フォント | U+25B6 |
|---|---|
| 組込フォント (`fontconvert.py:47-100`) | ❌ Geometric Shapes (0x25A0-0x25FF) が intervals に無い |
| SD フォント `cjk` プリセット (`fontconvert_sdcard.py:53-55`) | ❌ 無い |
| SD フォント `symbols` / `reading` プリセット | ✅ あるが、ユーザーが焼き直す必要がある |

**`>` は ASCII。**チェックボックス記法 `- [ ]` 自体が ASCII なので、
**その行が描画できているなら `>` も必ず描画できる。**論理的に破綻しない。

#### マーカー欄の確保

`.md` を開いている間、**ファイル全体で**左に `"> "` 分の幅を確保する。

```cpp
// initializeReader() 内、viewportWidth を求めた直後
markerWidth = isMarkdown ? renderer.getTextAdvanceX(cachedFontId, "> ", REGULAR) : 0;
viewportWidth -= markerWidth;
```

`viewportWidth` はページインデックスキャッシュの検証項目 (`TxtReaderActivity.cpp:491`) なので、
**既存の `.md` キャッシュは自動的に1回だけ再構築され、以後は整合する。**

描画は行の文字列を書き換えず、`"> "` を別の `drawText` として確保済みの欄に描く。
**折り返しは一切動かない。**

### 2.3 検出

正規表現は使わない。

```
先頭の ' ' と '\t' を読み飛ばす
→ 次が '-' / '*' / '+' のいずれか
→ 次が ' '
→ 次が '[' 
→ 次が ' ' / 'x' / 'X'   ← この1バイトの位置と文字を記録
→ 次が ']'
```

`]` の後に何が来るかは問わない。したがって
`- [ ]の直前に…` のように直後が日本語でも**チェックボックスとして正しく検出される。**

```cpp
struct CheckboxRef {
  int lineIndex;      // currentPageLines 内の視覚行番号
  size_t markOffset;  // '[' の中身1バイトの絶対ファイルオフセット
  bool checked;
};
```

検出はソース行を切り出した直後、**折り返しループより前**に行う。
ソース行頭の絶対オフセットは `offset + pos`。

#### 収集は `render()` 経路だけ、判定は両経路

```cpp
bool loadPageAtOffset(size_t offset, std::vector<std::string>& outLines, size_t& nextOffset,
                      std::vector<CheckboxRef>* outCheckboxes = nullptr);
```

- `render()` からは `&pageCheckboxes` を渡す
- `buildPageIndex()` からは `nullptr`

**ただしマーク文字の判定そのものは、`outCheckboxes` が null でも `isMarkdown` なら必ず行う。**
理由は §3.4 を参照。`vector` への push だけが `render()` 経路に限定される。

### 2.4 幅の正規化 — 基準は小文字の `x`

`[ ]` / `[x]` / `[X]` は**別のグリフなので描画幅が違う** (実機で確認)。
本文フォントが Noto Serif JP の場合、`advance(' ') < advance('x') < advance('X')`。

放置すると、**トグルした行がちょうど1行に収まるかどうかの境目にあったとき視覚行が1本増え、
`pageOffsets[currentPage+1]` は固定なのでその差分のテキストがどのページにも表示されなくなる。**

**折り返しの計算のときだけ、3状態すべてを `'x'` の幅として測る。**

| マーク | 補正 | 備考 |
|---|---|---|
| `[x]` | 0 | 基準 |
| `[ ]` | `advance('x') - advance(' ')` | 正の値 |
| `[X]` | `advance('x') - advance('X')` | **負の値。クランプしてはならない** |

補正はマークを含む区間 (`lineBytePos == 0`) にだけ加算する。
折り返した後の区間にはマークが無いので加算しない。

#### なぜ `X` ではなく `x` を基準にするか

**トグルが書き込むのは `' '` か `'x'` だけ。**`[X]` は他のエディタが作った一時的な状態で、
一度触れば消える。定常状態である2つを正確に扱うほうが得で、`X` 基準にすると
すべての行が実際より広く測られ続けることになる。

**代償**: 未トグルの `[X]` 行だけ、描画が測定より `advance('X') - advance('x')` px 広い。
右余白に数px はみ出しうる。**既知・許容。**トグルすれば解消する。

### 2.5 書き戻し

`- [ ]` ↔ `- [x]` は**ファイル長が変わらない。**差分は角括弧内の1バイトのみ。

```cpp
HalFile f = Storage.open(txt->getPath().c_str(), O_RDWR);
if (!f) { LOG_ERR(...); return false; }
if (!f.seek(cb.markOffset)) { f.close(); return false; }
const char c = cb.checked ? ' ' : 'x';
const bool ok = (f.write(&c, 1) == 1);
f.flush();
f.close();
```

- **`Storage.openFileForWrite()` は使えない。**SDK の `SDCardManager.cpp:342` が
  `vol().open(path, O_RDWR | O_CREAT | O_TRUNC)` で開くため、呼んだ瞬間にファイルが空になる
- `O_RDWR` が有効な `oflag_t` であることは同じ行が根拠。`HalStorage::open()` は
  `oflag` を素通しする (`HalStorage.cpp:84-87`)
- **成功したときだけ `requestUpdate()` を呼ぶ。**失敗時は画面を変えない
  (「トグルしたつもり」を起こさせない)
- 書き戻しの後は `render()` が通常どおりファイルを読み直すので、
  **表示は自動的に新しい状態になる。**`currentPageLines` を手で書き換える必要はない

---

## 3. 実装上の罠 (4件)

> **本節が v0.2 で最も価値のある部分。**いずれも v0.1 の設計メモには書かれておらず、
> 実装して実機で動かして初めて表に出た。

### 3.1 `detectPageTurn` は既定では「押し下げ」で発火する

```cpp
// ReaderUtils.h:52
const bool usePress = SETTINGS.longPressButtonBehavior == SETTINGS.OFF;   // 既定は OFF
...
(usePress ? input.wasPressed(PageForward) : input.wasReleased(PageForward))
```

`longPressButtonBehavior` の既定値は `OFF` (`CrossPointSettings.h:247`)。
F-2 側で `wasReleased()` を使うと、

1. ボタンを押す → `wasPressed` が真、`wasReleased` は偽
2. F-2 のカーソル処理は何もしない
3. 直後の `detectPageTurn()` が `wasPressed` を見て**ページを送る**

**指を離す前にページが送られるので、カーソル処理に永遠に到達しない。**

**対策**: 側面ボタンの取得を `detectPageTurn` と同じ規則に揃える。

```cpp
const bool usePress = SETTINGS.longPressButtonBehavior == SETTINGS.OFF;
const bool sideNext = usePress ? mappedInput.wasPressed(Button::PageForward)
                               : mappedInput.wasReleased(Button::PageForward);
const bool sidePrev = usePress ? mappedInput.wasPressed(Button::PageBack)
                               : mappedInput.wasReleased(Button::PageBack);
```

タッチと傾きは拾わない (カーソル移動は側面ボタン限定)。

> **消費しないで先に進んでよい。**`wasReleased()` / `wasPressed()` は
> `InputManager.cpp:456` の `return releasedEvents & (1 << buttonIndex);` で、
> **読み取りで状態を消さない const 関数。**同じフレームで `detectPageTurn()` が同じ押下を再度読める。

### 3.2 `render()` でのカーソル初期化は、ページが変わったときだけ

`render()` は毎回走る。カーソル移動は `requestUpdate()` → `render()` を呼ぶ。
初期化を無条件にすると、**インクリメントした直後に自分で打ち消す。**

```cpp
// ❌ これだと画面上でカーソルが1つも動かない
if (pageCheckboxes.empty()) selectedCheckbox = -1;
else selectedCheckbox = enteredFromForward ? n - 1 : 0;
```

**対策**: カーソルがどのページのものかを覚える。

```cpp
int checkboxPageStamp = -1;
...
const int n = static_cast<int>(pageCheckboxes.size());
if (n == 0) {
  selectedCheckbox = -1;
} else if (checkboxPageStamp != currentPage) {
  selectedCheckbox = enteredFromForward ? n - 1 : 0;   // ページが変わったときだけ
} else if (selectedCheckbox >= n) {
  selectedCheckbox = n - 1;                            // 保険のクランプ
}
checkboxPageStamp = currentPage;
enteredFromForward = false;
```

### 3.3 折り返しに影響する変更は `CACHE_VERSION` を上げる

`loadPageIndexCache()` の検証項目は
magic / version / fileSize / viewportWidth / linesPerPage / fontId / screenMargin / paragraphAlignment の8つで、
**折り返しアルゴリズムを表すものが1つもない。**

実際に踏みかけた経路:

1. マーカー欄の導入で `.md` の `viewportWidth` が縮む → キャッシュはその値で再構築される
2. その後に幅の正規化 (§2.4) を入れる → `viewportWidth` は**同じ値のまま**
3. → キャッシュが「有効」と判定され、**古い折り返しで作った `pageOffsets` が再利用される**

**`CACHE_VERSION` を 3 → 4 に上げた。**コメントの "Increment when cache format changes" は
**格納内容の意味が変わった場合も含む。**

> 代償として `.txt` を含む全インデックスが1回再構築される。
> キャッシュは拡張子で分かれておらず `/.crosspoint/txt_<hash>/` を共有しているため。
> 112KB 程度なら数十秒。

### 3.4 `buildPageIndex()` と `render()` は同じ折り返しをしなければならない

`buildPageIndex()` は `pageOffsets` を作るために `loadPageAtOffset()` を全ページ分回す。
ここで `render()` と**違う折り返し**をすると、
「インデックスが想定したページの中身」と「実際に表示される中身」がずれ、
ずれた分のテキストはどこにも表示されなくなる。

幅の正規化 (§2.4) を `outCheckboxes != nullptr` の内側に置くと、
**インデックス構築時だけ補正が効かない**という形でこれを踏む。

**対策**: マーク文字の判定は `isMarkdown` なら両経路で行い、
`vector` への push だけを `render()` 経路に限定する。
判定は行頭数バイトを見るだけなので、`buildPageIndex()` のコストへの影響は誤差。

---

## 4. 既知の残り (対応済みの判断)

| # | 内容 | 判断 |
|---|---|---|
| 1 | **`- [ ]` が単独で1行を占める** | 既存の折り返しが `]` の後ろの半角スペースで折るため。折り返す長さの項目では必ず起きる。1行分の無駄で、壊れてはいない。**優先度低** |
| 2 | 未トグルの `[X]` 行が右に数px はみ出す | §2.4 の代償。トグルすれば解消。**許容** |
| 3 | **最終ページで「次へ」を押すとホームに戻る** | 最終ページに**チェックボックスが無い**場合、例外処理が発動しない。**現状維持と決定** (§4.1) |
| 4 | トグルとカーソル移動がリフレッシュ周期を消費する | `displayWithRefreshCycle` が N 回に1回 `HALF_REFRESH` (1720ms) を使うため、連続操作の途中で全画面フラッシュが入る。**妥当な挙動** |

### 4.1 最終ページの扱い — 現状維持と決定 (2026-08-13)

`TxtReaderActivity` には**終了画面が無い。**最終ページで「次へ」を押すと
`onGoHome()` が直接呼ばれ、確認なしにファイルが閉じる (`TxtReaderActivity.cpp:79-86`)。
`EndOfBookOptions` を使っているのは EPUB (`EpubReaderActivity.cpp:498-513`) と
XTC (`XtcReaderActivity.cpp:83-93`) だけ。

F-2 は「最終ページかつ末尾のチェックボックス」のときだけ握りつぶすが、
**最終ページにチェックボックスが無ければ発動しない。**

**このままでよい**と判断した。

- 進捗は毎回の描画で保存されるので、開き直せば最終ページに戻る
- 「本文の終わりまで来たら閉じる」は、ページ送りだけで使っていたときと同じ挙動
- ここを塞ぐと「最後まで来たのに閉じられない」という別の不便が生まれる

---

## 5. v0.1 から覆った設計 (記録)

| 項目 | v0.1 の案 | 実装 | 理由 |
|---|---|---|---|
| マーカー | `▶` (§3.2) | **`>`** | 組込・SD どちらのフォントにも U+25B6 が無い |
| カーソル入力 | `Up`/`Down` を直接 (§2.3) | **`PageBack`/`PageForward`** | `SIDE_BUTTONS_DISABLED` の尊重と向き反転が自動で効く |
| 初期位置 | 常に先頭 (§3.2) | **方向依存** | 後方から入ったら末尾。前に向かって歩く途中で先頭に飛ばされない |
| トグル時の描画 | `loadPageAtOffset` 省略 + AA 省略 (§4.2) | **どちらも省略しない** | 読み直すことが「書き込みがファイルに届いた」ことの証明になる。速度は保留 (§6) |
| 設定項目 | 不要 (§6) | **`SHORT_PWRBTN::CHECKBOX` を追加** | 電源短押しの既存 enum に1項目足すのが最も素直 |
| 描画コストの見込み | 1591ms → 100ms 程度 (§4.2) | **誤り。**下限は約 870ms | E-Ink 転送 548ms は物理時間。`txt_prewarm` 269ms もフォントキャッシュ破棄の都合で省略できない |
| `O_RDWR` の前例 | `Section.cpp:705-710` (§3.3) | **前例は存在しなかった** | 当該行はコメント。根拠は SDK の `SDCardManager.cpp:342` |

### `pageOffsets` キャッシュ (v0.1 §7 論点1) — 問題なし

検証項目にファイル長は含まれるが**更新日時もハッシュも無い**ため、
1バイト書き戻しでは無効化されない。**ファイル長が変わらない設計にした判断がそのまま効いた。**

ただし §3.3 のとおり、**折り返しを変える改修を入れるときは別問題**である。

---

## 6. 保留中の作業

**描画の最適化 (窓更新 + 固定描画) は `docs-jp/f2-deferred-partial-update.md` に分離した。**
SDK の実査結果 (配線3段のうち2段は既にある)、罠2件、煮詰めるべき論点5件がそこにある。

**着手の判断材料は「体感」で足りている。**実機で「快適」だったため、優先度は当初より下がった。

### その他の保留

| # | 内容 |
|---|---|
| 1 | 電源ボタン短押しを `.md` 以外でも使えるようにするか。**「決定/選択」への流用は非推奨** (`Button::Confirm` は全アクティビティ共用で誤爆の被害が大きい)。現実的なのは「`.md` 以外ではページめくりにフォールバック」で、`ReaderUtils.h:62` の `powerTurn` の条件に1つ足すだけ |
| 2 | `- [ ]` が単独で1行を占める見た目の改善 (§4 の1) |

---

## 7. 段階2 以降

| 段階 | 内容 | 状態 |
|---|---|---|
| **段階1** | **チェックボックスのトグル** | **✅ 完了 (2026-08-13)** |
| 段階2 | 表示の記号化 (`☐` / `☑`)、箇条書き (`・`)、水平線 | 未着手。**字形の有無を先に確認すること** (§2.2 と同じ問題。`☐` U+2610 も Geometric Shapes 系ではないが、組込フォントの intervals には無い) |
| 段階3 | 連番の整形 | 任意 |

**見出しのフォントサイズ変更は対象外。**480×800 では行数を圧迫し、かえって読みにくくなる。

---

## 8. 動作確認用ファイル

`docs-jp/f2-test.md` に、実機確認用の `.md` を置いてある。各節に期待値が書かれており、
端末の画面を見ながら確認できる。

| 節 | 内容 |
|---|---|
| 1 | 基本形 |
| 2 | 記法のゆれ (`-` / `*` / `+` / `[X]`) |
| 3 | インデント (スペース2/4・タブ) |
| 4 | **検出されてはいけないもの** 4種 |
| 5 | 折り返す長い行 |
| 6-a | 目視での桁揃えテスト (シリアル不要で `advance` の差を判定) |
| 6-b / 6-c | 長い項目の折り返し (**注: これは検証にならない。**§2.4 の問題は短い項目でしか出ない) |
| **6-d** | **1行に収まるかどうかの境目** — 幅の正規化の本当の検証 |
| 7 | チェックボックスが1個だけのページ |
| 8 | チェックボックスが無いページ |
| 9 | ページをまたぐカーソルの着地点 |
| 10 | ファイルの末尾 |

---

## 改訂履歴

| 版数 | 日付 | 内容 |
|---|---|---|
| v0.1 | 2026-08-10 | 初版。入力設計を確定、実装の3部品を整理、未解決の論点を5件洗い出し |
| **v0.2** | **2026-08-13** | **段階1 の実装完了を受けて全面改訂。**実装された内容の記録に性格を変更。実装上の罠4件 (§3) を新規に記録。v0.1 から覆った設計7件を §5 に整理。幅の正規化 (§2.4) と最終ページの扱い (§4.1) を新規に決定。描画の最適化を別メモに分離 |
