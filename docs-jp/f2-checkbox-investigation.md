# F-2 未解決論点の調査結果 — ソース実査による回答

**作成日**: 2026-08-13
**位置づけ**: `docs-jp/f2-markdown-checkbox.md` §7 (未解決の論点5件) への回答
**調査対象**: `lranslot/crosspoint-reader-jp` @ `c1f42145` (ベース upstream/develop `81028d58`)
**方法**: リポジトリを clone し、該当コードを全文読解。**実機は未使用。**行番号はすべて `c1f42145` 時点のもの。

---

## 0. 結論一覧

| # | 論点 | 結論 |
|---|---|---|
| **1** | `pageOffsets` キャッシュの検証ロジック | **✅ 再構築は走らない。設計変更は不要。**検証項目に更新日時もハッシュも無い |
| 2 | AA 省略後の見た目 | 実機案件。**ただし「1591ms → 100ms」の見積もりは誤り。**実際の下限は約 870ms (§5) |
| 3 | `SIDE_BUTTONS_DISABLED` の尊重 | **尊重すべき。**`Button::Up`/`Down` は設定を素通りする実装なので、F-2 側で明示的に見る必要がある |
| 4 | `[ ]` と `[x]` の advance 幅 | **等幅なら無害。可変幅だと1ページ分の取りこぼしが起きうる** (§4 で新たに判明)。実機ログ1回で判定できる |
| 5 | `▶` の字形 | **❌ 使えない。**組込フォント・SDフォントの `cjk` プリセット双方に U+25B6 は無い。**ASCII の `>` を使う** |

### さらに、設計メモに訂正が要る点が4件見つかった

| # | 箇所 | 内容 |
|---|---|---|
| A | §3.1 / §4.2 | **`render()` は毎回 `loadPageAtOffset()` を呼ぶ。**「`currentPageLines` が残っている」前提は成り立たない。省略するにはフラグが要る |
| B | §2.2 | 電源短押しの選択肢追加は **enum 末尾への追加が必須** (`settings.json` に index で永続化されるため) |
| C | §3.3 | 「`Section.cpp:705-710` が `O_RDWR` の前例」は**誤り。**当該箇所はコメントで、**コード上に `O_RDWR` の使用例はゼロ** |
| D | §4.2 | `txt_prewarm` 269ms は**省略すると危険。**描画スコープの destructor がフォントキャッシュを破棄している |

---

## 1. 【論点 #1】`pageOffsets` キャッシュの検証ロジック — ✅ 問題なし

### 結論

**1バイト書き戻しでキャッシュは無効化されない。設計を変える必要はない。**

### 根拠

`TxtReaderActivity::loadPageIndexCache()` (`TxtReaderActivity.cpp:447-540`) の検証項目は**8件のみ**で、そのすべてが早期 return の条件になっている。

| 順 | 検証項目 | 行 | トグルで変化するか |
|---|---|---|---|
| 1 | magic `"TXTI"` | L470-473 | しない |
| 2 | `CACHE_VERSION` (= 3) | L476-480 | しない |
| 3 | **ファイルサイズ** | L484-487 | **しない** (`- [ ]` ↔ `- [x]` は同一バイト長) |
| 4 | `viewportWidth` | L491-494 | しない |
| 5 | `linesPerPage` | L498-501 | しない |
| 6 | `cachedFontId` | L505-508 | しない |
| 7 | `cachedScreenMargin` | L512-515 | しない |
| 8 | `cachedParagraphAlignment` | L519-522 | しない |

**更新日時 (mtime)、ハッシュ、チェックサムはいずれも検証項目に無い。**
`savePageIndexCache()` (L542-567) も同じ8項目しか書いていないため、そもそも記録されていない。

```cpp
// TxtReaderActivity.cpp:482-487
uint32_t fileSize;
serialization::readPod(f, fileSize);
if (fileSize != txt->getFileSize()) {
  LOG_DBG("TRS", "Cache file size mismatch, rebuilding");
  return false;
}
```

**ファイル長が変わらない設計にした判断が、そのまま効いている。**

### 副次的に確認できたこと

`viewportWidth` がキャッシュ検証項目に入っている (L491) ことは、**カーソル欄の確保に使える** (§4.3 で後述)。
`.md` のときだけ `viewportWidth` を狭めれば、既存キャッシュは自動的に1回だけ再構築され、以後は整合する。**`CACHE_VERSION` を上げる必要がない。**

---

## 2. 【論点 #3】`SIDE_BUTTONS_DISABLED` の尊重 — 尊重すべき

### 実装の確認

`MappedInputManager::mapButton()` (`MappedInputManager.cpp:52-110`) で、
**`Button::Up`/`Down` と `Button::PageBack`/`PageForward` は挙動が違う。**

```cpp
// MappedInputManager.cpp:70-77 — 設定を見ない
case Button::Up:
  // Side buttons remain fixed for Up/Down.
  return (gpio.*fn)(HalGPIO::BTN_UP);
case Button::Down:
  return (gpio.*fn)(HalGPIO::BTN_DOWN);

// MappedInputManager.cpp:81-98 — 設定を見る
case Button::PageBack:
  switch (sideLayout) {
    case CrossPointSettings::PREV_NEXT:          return (gpio.*fn)(HalGPIO::BTN_UP);
    case CrossPointSettings::NEXT_PREV:          return (gpio.*fn)(HalGPIO::BTN_DOWN);
    case CrossPointSettings::SIDE_BUTTONS_DISABLED:
    default:                                     return false;
  }
```

設計メモ §2.4 の注意書きは正しい。**`Button::Up`/`Down` を直接拾うと `SIDE_BUTTONS_DISABLED` を素通りする。**

### 判断

**F-2 側で `SETTINGS.sideButtonLayout != SIDE_BUTTONS_DISABLED` を明示的に確認する。**

理由:

- 「無効」を選ぶユーザーは、**側面ボタンに触れても何も起きないこと**を期待している (誤爆防止・ポケット内対策)
- 尊重しない場合、`.md` を開いたときだけ側面が生き返る。設定画面からは説明できない挙動になる
- 尊重した場合の代償は「無効にしている人はカーソル移動が使えない」だけ。トグル自体は電源ボタンなので機能は死なない

なお `PREV_NEXT` / `NEXT_PREV` の向き反転もカーソル移動に反映すべき (`NEXT_PREV` なら下=前)。**`Button::PageBack`/`PageForward` を使えば、設定の尊重と向き反転が同時に手に入る。**論理名は「前ページ/次ページ」だが、F-2 が消費するのはチェックボックスがあるページに限られる。

### 電源ボタンとの競合 — 既に解決済み

`POWER` + `BTN_DOWN` の同時押しはスクリーンショットの組み合わせだが、
`main.cpp:503-512` が `loop()` の先頭で捕まえて `return` するため、**アクティビティまで届かない。**

```cpp
// main.cpp:503
if (gpio.isPressed(HalGPIO::BTN_POWER) && gpio.isPressed(HalGPIO::BTN_DOWN)) { ... return; }
```

`EpubReaderActivity.cpp:580-582` は念のため `!wasReleased(Button::Down)` でも守っている。**同じガードを踏襲するのが安全。**

---

## 3. 【論点 #5】`▶` の字形 — ❌ 使えない

### 根拠

| フォント | 収録範囲 | U+25B6 (`▶`) |
|---|---|---|
| **組込フォント** (`fontconvert.py:47-100`) | ASCII / Latin / キリル / 矢印 (0x2190-0x21FF) / 数学記号 (0x2200-0x22FF) | **❌ 無い** (Geometric Shapes 0x25A0-0x25FF が intervals に無い) |
| **SDフォント `cjk` プリセット** (`fontconvert_sdcard.py:53-55`) | 0x3000-0x303F / かな / 0x4E00-0x9FFF / 0xF900-0xFAFF / 0xFF00-0xFFEF | **❌ 無い** |
| SDフォント `symbols` / `reading` プリセット | 0x25A0-0x25FF を含む (`:62-63`, `:70-71`) | ✅ ある |

**`▶` を出すには、ユーザーが `symbols` か `reading` を含めてフォントを焼き直す必要がある。**
配布物として成立しない。

### 代替案

| 候補 | 判定 |
|---|---|
| **`>` (U+003E)** | **✅ 採用推奨。**チェックボックス記法 `- [ ]` 自体が ASCII なので、**その行が描画できているなら `>` も必ず描画できる。**論理的に破綻しない |
| `→` (U+2192) | 組込フォントには有るが、**SDフォントの `cjk` プリセットには無い。**ユーザーのフォント構成に依存する |
| `*` / `#` | 動くが、Markdown の記法と紛らわしい |
| `●` (U+25CF) | `▶` と同じ理由で不可 |

**`> ` (`>` + 半角スペース) を行頭マーカーとする。**

---

## 4. 【論点 #4】`[ ]` と `[x]` の advance 幅 — 新たなリスクが判明

### 設計メモが見落としていた影響

メモは「幅が変わると折り返しが変わる」までは書いているが、**その先に起きることが書かれていない。**

`render()` は `pageOffsets[currentPage]` から**毎回ページを読み直す** (`TxtReaderActivity.cpp:336-339)`。
一方、次ページの開始位置は `pageOffsets[currentPage+1]` に**固定で記録済み**である。

```cpp
// TxtReaderActivity.cpp:336-339
size_t offset = pageOffsets[currentPage];
size_t nextOffset;
currentPageLines.clear();
loadPageAtOffset(offset, currentPageLines, nextOffset);   // ← nextOffset は捨てられる
```

したがって、トグルによって**そのページの1行が2行に折り返されるようになると**:

1. `linesPerPage` は固定なので、ページに収まるソース文字数が減る
2. しかし次ページは元の `pageOffsets[currentPage+1]` から始まる
3. **差分のテキストが、どのページにも表示されなくなる**

### 発生条件は狭い

- `advance('x') > advance(' ')` であること (**等幅フォントなら発生しない**)
- かつ、そのソース行の幅が `viewportWidth` の直下 `advance('x') - advance(' ')` px 以内に入っていること

UDEVGothic は等幅なので **ASCII の `x` と ` ` は同幅**。附録D の構成 (8/10/12pt が UDEVGothic、14/16/18pt が Noto Serif JP Medium) では、**明朝を選んだサイズでのみ発生しうる。**

### 判定方法 (実機ログ1回で済む)

`initializeReader()` に一時的に以下を仕込み、シリアルログを1回見る。

```cpp
LOG_INF("TRS", "advance space=%d x=%d",
        renderer.getTextAdvanceX(cachedFontId, " ", EpdFontFamily::REGULAR),
        renderer.getTextAdvanceX(cachedFontId, "x", EpdFontFamily::REGULAR));
```

| 結果 | 対応 |
|---|---|
| **一致** | 対策不要。段階1はこのまま進める |
| **不一致** | 段階1では許容し、既知の制約として記録する。根治は段階2で「計測時のみ `[x]` → `[ ]` に正規化」 |

**不一致でも段階1を止める理由にはならない。**発生条件が狭く、被害はそのページの再描画で復旧する (ページを離れて戻れば `loadPageAtOffset` が正しい行を返す。ただし取りこぼした行は次ページ側にも現れない)。

---

## 5. 【論点 #2】AA 省略 — 見積もりの訂正

### 「1591ms → 100ms」は成り立たない

設計メモ §4.2 の表そのものが、100ms を否定している。

| 処理 | 実測 | トグル時 | 判定 |
|---|---|---|---|
| txt_load_page | 233ms | 省略可 | ⚠️ **要フラグ。**`render()` は無条件に呼ぶ (L336-339) |
| txt_prewarm | 269ms | **省略不可** | ⚠️ 後述 |
| txt_scan_pass | 1ms | prewarm と一体 | — |
| txt_bw_draw | 50ms | 全行必要 | 部分更新が無いので1行に縮小できない |
| txt_aa | 422ms | **省略可** | ✅ `SETTINGS.textAntiAliasing` の判定を1回だけ潰す (L404) |
| **txt_eink** | **548ms** | **省略不可** | **物理時間** |
| txt_save_progress | 63ms | 省略可 | ページは変わらないので不要 |

**現実的な下限 = 269 + 50 + 548 = 約 870ms。**
`txt_prewarm` も外せると仮定しても **約 600ms** で、548ms の E-Ink 転送が床になる。

### `txt_prewarm` を外してはいけない理由

`renderPage()` (`TxtReaderActivity.cpp:392-407`) のコメントに明記がある。

```cpp
auto scope = fcm->createPrewarmScope();
renderLines();            // scan pass — text accumulated, no drawing
scope.endScanAndPrewarm();
...
// scope destructor clears font cache via FontCacheManager
```

**スコープの destructor がフォントキャッシュを破棄する。**
前回の `render()` が終わった時点でキャッシュは空なので、prewarm を飛ばすと全グリフがミスし、
F-6 で潰したスラッシングが再発する。**269ms を払うのが正しい。**

### 実機で確認すべきこと (指示書に含めた)

1. AA を飛ばした行がジャギーに見えるか。**読める程度か、目に付くか**
2. トグル前後で他の行の見た目が変わらないか (AA 付きで描いた画面の上に BW を重ねるため、**残像の出方**が問題になりうる)
3. 体感 870ms が許容できるか

> 許容できない場合の逃げ道はメモ §7 #2 の通り「一定時間操作がなければ AA 付きで描き直す」。
> ただし E-Ink がもう一度 548ms 光るので、**連続トグル時はかえって遅く見える**可能性がある。

---

## 6. 設計メモへの訂正 4件

### A. `render()` は毎回ページを読み直す

メモ §4.2 は「トグル時は `currentPageLines` が残っている」ことを前提にしているが、
`render()` は `initialized` に関係なく毎回 `loadPageAtOffset()` を呼ぶ (`TxtReaderActivity.cpp:338-339`)。

**メンバに「今回の描画はリロード不要」フラグを持たせない限り、233ms は消えない。**
段階1では**フラグを入れず、素直に読み直すことを推奨する。**理由:

- 読み直せば**書き込みが実際にファイルへ反映されたかを画面が証明する**
- 状態変数が1つ減る
- 233ms は全体 (約1100ms) の2割で、E-Ink の548msの前では支配的でない

### B. 電源短押しの選択肢は enum の末尾に足す

```cpp
// CrossPointSettings.h:133
enum SHORT_PWRBTN { IGNORE = 0, SLEEP = 1, PAGE_TURN = 2, FORCE_REFRESH = 3, FOOTNOTES = 4, SHORT_PWRBTN_COUNT };
```

同ファイル L138-141 の `LONG_PRESS_MENU_FUNCTION` に、この種の enum の運用規則が明記されている。

> Persisted in settings.json by index: any new function MUST use a value >= 2 and be appended at the END of the enumValues array in SettingsList.h, otherwise the stored indices shift and existing saves are silently misinterpreted.

**`CHECKBOX = 5` を末尾に追加する。**`SettingsList.h:296-299` の `enumValues` 配列も末尾に足す。

```cpp
// SettingsList.h:298
{StrId::STR_IGNORE, StrId::STR_SLEEP, StrId::STR_PAGE_TURN, StrId::STR_FORCE_REFRESH, StrId::STR_FOOTNOTES},
```

### C. `O_RDWR` の前例は存在しない

メモ §3.3 は `Section.cpp:705-710` を前例として挙げているが、**当該行はコメントである。**

```cpp
// Section.cpp:705  ← これはコメント
// The .bin is open O_RDWR for the build. Read the already-written page, then restore
```

実際に `Section` がファイルを開いているのは `Storage.openFileForWrite()` (`:302`, `:337`) で、
**リポジトリ全体で `O_RDWR` を引数に渡している箇所は1つも無い。**
`oflag` 付きの唯一の実例は `Dictionary.cpp:339` の `Storage.open(DICT_TMP_FILE, O_WRITE | O_CREAT | O_TRUNC)`。

`openFileForWrite()` は SDK 側 (`freeink-sdk`) の実装で、名前からして truncate 系である可能性が高い。
**`Storage.open(path, O_RDWR)` を使い、コンパイルと実機の両方で最初に通すこと。**
`O_RDWR` は `common/FsApiConstants.h` (SdFat 由来) 経由で `HalStorage.h:33` の既定引数と同じ型で渡せるはずだが、**未検証。**

### D. `Txt` に書き込み API は無い (メモの記述は正しい)

`Txt.h` は `readContent()` (`:33`) のみ。`Txt::readContent()` の実装 (`Txt.cpp:173-189`) は**呼ばれるたびに `openFileForRead` している**ので、書き込みも同様に「開いて・seek して・書いて・閉じる」で揃う。

---

## 7. 確認できたその他の事実 (実装に効くもの)

| 事実 | 出典 |
|---|---|
| `.md` 判定は `FsHelpers::hasMarkdownExtension()` が既にある | `FsHelpers.cpp:174`、`ReaderActivity.cpp:24-27` |
| `TxtReaderActivity::loop()` は Back とページ送りしか見ていない。**`Confirm` は完全に空き** | `TxtReaderActivity.cpp:62-87` |
| 電源短押しは既に `ReaderUtils::detectPageTurn()` が `PAGE_TURN` を消費している | `ReaderUtils.h:61-62` |
| 電源短押しをアクティビティ側で分岐する前例 (脚注) | `EpubReaderActivity.cpp:580-600` |
| 依存設定を条件付きで隠す前例 (`pwrBtnFootnoteBack`) | `SettingsActivity.cpp:58-61` |
| `loadPageAtOffset()` は `buildPageIndex()` からも呼ばれる。**チェックボックス収集を無条件で足すとインデックス構築が重くなる** | `TxtReaderActivity.cpp:146` |
| 折り返しループはソース行内のバイト位置を `lineBytePos` で追跡済み | `TxtReaderActivity.cpp:232, 246, 280` |
| 描画は視覚行1本につき `drawText` 1回 | `TxtReaderActivity.cpp:386` |
| 新規 UI 文字列は `english.yaml` が正本。ここに無いキーは無視される | 仕様書 §6.1 |

---

## 8. 段階1 の設計 (更新版)

以上を踏まえ、`f2-markdown-checkbox.md` §3 の3部品を次のように確定する。

### ① 行 → ファイル位置

`loadPageAtOffset()` に**任意の out パラメータ**を追加し、`render()` 経路だけ非 null を渡す。
`buildPageIndex()` は `nullptr` を渡す。**インデックス構築のコストは1バイトも増えない。**

```cpp
struct CheckboxRef {
  int lineIndex;      // currentPageLines 内の視覚行番号
  size_t markOffset;  // '[' の中身1バイトの絶対オフセット
  bool checked;
};
bool loadPageAtOffset(size_t offset, std::vector<std::string>& outLines, size_t& nextOffset,
                      std::vector<CheckboxRef>* outCheckboxes = nullptr);
```

検出はソース行を切り出した直後 (`TxtReaderActivity.cpp:229` の `std::string line(...)` の後)。
ソース行頭の絶対オフセットは `offset + pos`。折り返しの前なので `lineIndex` は `outLines.size()`。

判定は正規表現不要の手書きで足りる。

```
先頭の ' ' / '\t' を読み飛ばす
→ '-' / '*' / '+'  → ' '  → '['  → ' ' / 'x' / 'X'  → ']'
```

### ② カーソル

- メンバ `int selectedCheckbox = -1;`
- `render()` でページ読込後、`pageCheckboxes` が空なら `-1`、そうでなければ `0` (ページ先頭)
- 移動は `Button::PageBack` / `PageForward` を使う。**`SIDE_BUTTONS_DISABLED` と向き反転が自動で効く** (§2)
- 端を超えたらページ送りに委譲

### ③ マーカーとカーソル欄

`.md` を開いたときだけ、**ファイル全体で**左に `"> "` 分の幅を確保する。

```cpp
// initializeReader() 内、viewportWidth を求めた直後
markerWidth = isMarkdown ? renderer.getTextAdvanceX(cachedFontId, "> ", EpdFontFamily::REGULAR) : 0;
viewportWidth -= markerWidth;
```

- `viewportWidth` はキャッシュ検証項目 (`TxtReaderActivity.cpp:491`) なので、**既存の `.md` キャッシュは1回だけ自動再構築され、以後は整合する。`CACHE_VERSION` を触らなくてよい**
- `renderPage()` では全行の `x` に `markerWidth` を足し、選択行だけ `x = cachedOrientedMarginLeft` に `"> "` を描く
- **折り返しは一切動かない** (`viewportWidth` が最初から狭いため)

### ④ 書き戻し

```cpp
HalFile f = Storage.open(txt->getPath().c_str(), O_RDWR);
if (!f) { LOG_ERR(...); return false; }
if (!f.seek(cb.markOffset)) { f.close(); return false; }
const char c = cb.checked ? ' ' : 'x';
const bool ok = (f.write(&c, 1) == 1);
f.flush();
f.close();
```

**失敗したら画面を変えない。**`requestUpdate()` を呼ばなければ元の表示のままになる。

---

## 9. 残る未確認事項

| # | 内容 | 潰し方 |
|---|---|---|
| 1 | `O_RDWR` が `Storage.open()` に渡せるか | **ビルドで判明。**通らなければ SDK 側の `openFileForWrite` を読む |
| 2 | `advance(' ')` と `advance('x')` が一致するか | 起動時ログ1回 (§4) |
| 3 | AA を飛ばした見た目 | 実機。写真で判断 (§5) |
| 4 | トグル1回の実測時間 | 実機。既存の計装 (`lib/Instrument/`) は未移植なので、`millis()` の差分ログで足りる |
| 5 | SD 書き込み保護・カード抜けの挙動 | 実機。SD を抜いてトグルし、無言で失敗しないことを確認 |

---

---

## 10. TXT の最終ページの挙動 — 終了画面は無い

**`TxtReaderActivity` は `EndOfBookOptions` を使っていない。**

| リーダー | 最終ページで「次へ」 |
|---|---|
| EPUB | `EndOfBookOptions` (`EpubReaderActivity.cpp:498-513`)。次の本 / ホーム / 最終ページに留まる / 再描画 を選べる |
| XTC | 同上 (`XtcReaderActivity.cpp:83-93`) |
| **TXT** | **`onGoHome()` を直接呼ぶ** (`TxtReaderActivity.cpp:79-86`)。確認なし。即座にファイルが閉じてホームへ戻る |

```cpp
// TxtReaderActivity.cpp:79-86
} else if (nextTriggered) {
  if (currentPage < totalPages - 1) {
    currentPage++;
    requestUpdate();
  } else {
    onGoHome();     // ← 終了画面を経ずにホームへ
  }
}
```

`Activity::onGoHome()` は `activityManager.goHome()` の薄いラッパ (`Activity.cpp:13`)。

### F-2 への影響

カーソルが末尾を超えたときページ送りに落とす設計だと、
**最終ページの最後のチェックボックスで、押した瞬間にファイルが閉じる。**

ページ送りしかない現状では「読み終わった → 閉じる」で自然だが、
F-2 は**書き込みを伴う操作の最中**に同じことが起きる。事故の重さが違う。

**→ 最終ページの末尾だけは落とさず、そこで止める。**
原則 (「カーソルが実際に動くときだけ消費する」) は保たれる。落としても
カーソルもページも動かず、アクティビティが終了するだけだからである。

---

## 11. 部分更新 (窓更新) の実現性 — 想定より近い

> 仕様書 §6.2 は「第2段階 (任意)」としていたが、**配線作業は50行前後の見込み。**

### 各層の状態 (SDK `Free-Ink/freeink-sdk` main を実査)

| 層 | 状態 |
|---|---|
| `FreeInkDisplay::displayWindow(x, y, w, h, turnOffScreen)` | **実装済み** (`FreeInkDisplay.cpp:753-770`)。ドライバ実体は `Ssd1677Driver.cpp:491-553`。ヘッダ (`FreeInkDisplay.h:222`) に `EXPERIMENTAL` と注記 |
| `HalDisplay::displayWindow` | **存在しない。**新規に薄い委譲を書く必要がある |
| `GfxRenderer::displayWindow` | 宣言が `GfxRenderer.h:181` にコメントアウト。**座標変換 `screenRectToAlignedMemRect()` は既に本番稼働中** (`GfxRenderer.cpp:266-299`、`readFramebufferRegion`/`writeFramebufferRegion` が使用)。8px 整列も向き回転も実装済み |

### ⚠️ 罠1: グレースケール中は逆に遅くなる

```cpp
// Ssd1677Driver.cpp:497-504
if (_inGrayscaleMode) {
  displayImpl(bus, fb, nullptr, RefreshMode::Half, turnOff, /*async=*/false);
  return;   // 窓ではなく全画面 HALF リフレッシュ
}
```

`HALF_REFRESH` は **1720ms** (`HalDisplay.h:16`)。
AA が有効だとページ描画の最後が `displayGrayBuffer()` なのでパネルは常にグレースケール状態にあり、
**そこで窓更新を呼ぶと現状より遅くなる。**

**対策**: `.md` を開いている間は AA を無効にする。パネルがグレースケールに入らなくなる。

同様に `FreeInkDisplay::displayWindow` は `_inverted || _inversionDirty` (ダークモード) でも
`displayBuffer(FAST_REFRESH)` に落ちる (`FreeInkDisplay.cpp:757-763`)。

### ⚠️ 罠2: 差分の基準が更新されない

通常経路 (`FreeInkDisplay.cpp:680-686`) は表示後に `swapBuffers()` を呼び、
`frameBufferActive` を「直前に表示したフレーム」として保つ。
**`displayWindow` はこれを呼ばない。**ドライバ側も `prev != nullptr` の経路では
リフレッシュ後の RED 再シードを行わない (`Ssd1677Driver.cpp:545-549` は `prev == nullptr` のときだけ)。

結果、次の全画面 FAST リフレッシュは**トグルした文字の位置だけ古い基準と差分を取る。**
理屈の上では「既に正しいピクセルをもう一度駆動する」だけで無害だが、
**残像として出るとすればここ。コードを読んで確定できる種類の話ではない。**

### 着手順の判断

**F-2 と同時に着手しない。**

窓更新が動いているかを判定するには、変化する矩形が既にわかっている画面が要る。
F-2 が未完成だと矩形自体が無く、「窓が出ない」のか「トグルが動いていない」のかを切り分けられない。

**F-2 段階1 (全画面再描画のまま) → 窓更新の配線**、の順とする。
窓更新が実機で通れば、AA 省略・`loadPageAtOffset` 省略といった細工は**まるごと不要になる。**

---

## 12. §9 の未確認事項の更新

| # | 内容 | 状態 |
|---|---|---|
| 1 | `O_RDWR` が `Storage.open()` に渡せるか | **未確認。**ビルドで判明する |
| 2 | `advance(' ')` と `advance('x')` の一致 | 未確認。起動時ログ1回 |
| 3 | AA を飛ばした見た目 | 未確認。実機 |
| 4 | トグル1回の実測時間 | 未確認。実機 |
| 5 | SD 書き込み不可時の挙動 | 未確認。実機 |
| 6 | `wasReleased()` を同一フレームで複数回読めるか | **✅ 解決。**`InputManager.cpp:456` は `return releasedEvents & (1 << buttonIndex);` の const 関数で、読み取りで状態を消さない。**「消費しない → ページ送りに落ちる」はそのまま成立する** |

---

## 改訂履歴

| 版数 | 日付 | 内容 |
|---|---|---|
| v0.1 | 2026-08-13 | 初版。`c1f42145` のソース実査により §7 の5件に回答。設計メモの訂正4件と、advance 幅に起因するページ取りこぼしリスクを新たに指摘 |
| **v0.2** | **2026-08-13** | **§10 追加: TXT には終了画面が無く最終ページで即 `onGoHome()`。§11 追加: SDK 実査により窓更新の配線が50行前後と判明。罠2件を特定。§12 で `wasReleased` の非消費を確認済みに更新** |
