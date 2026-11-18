# g4 — TLS が入らない：フォントのダウンロードと OTA (2026-11-16)

> ★ **この文書は調査のみ。修正は入れていない。**
> 事実（コードと設定から出した数字）と、選択肢だけを書く。
>
> 実機の記録は羅闇さんが読んだもの:
>
>     4404 net-guard        free=137,088 max=114,676 need=110000/50000 ok
>     8798 net-join-before  free=110,840 max= 69,620
>     crash_report.txt: 1.5.0-jp-dev-8bc9cbd0 / PANIC / abort() at PC 0x421b36d7
>     ログ末尾: [HTTP] Downloading: .../PretendardMedium_10.cpfont
>               [HTTP] wolfSSL GET: https://github.com/...
>               → abort

---

## 0. まず abort の正体

`0x421b36d7` を `.pio/build/default/firmware.elf` で解決した:

    0x421b36d7: get_ttype_entry(lsda_header_info*, unsigned long)
                at libgcc/unwind-pe.h:77
     (inlined by) get_ttype_entry at libsupc++/eh_personality.cc:95

★ **EPUB の件（`0x421b36d5`）と同じ場所である。**
これは **C++ の例外巻き戻し機構**で、意味はひとつしかない:

> 誰かが例外を投げた。`-fno-exceptions` なので受け手が無い。
> → `std::terminate()` → `abort()`

投げ得るのはほぼ `operator new` の `std::bad_alloc` である
（`operator new` は libstdc++ の中にあり、我々の `-fno-exceptions` は
**そのコードには掛かっていない**）。

★ **つまりこれは OOM である。**そして:

- wolfSSL は C で、確保は `XMALLOC` → **NULL を返す**。
  `SecureClient.cpp` はそれを受けて `return 0` する（`CTX alloc failed` 等）。
  **wolfSSL 自身の確保失敗なら abort にはならない。**
- 我々の `new (std::nothrow)` も **abort にならない。**

★ **したがって落ちたのは「素の `new`」か、`std::string` / `std::vector` の成長である。**
TLS が失敗したのではなく、**TLS が食った結果、別の誰かが落ちた**可能性が高い。
★ **どの確保かはまだ特定できていない。**次節の数字は「TLS がいくら食うか」であって、
「何が落ちたか」ではない。区別して読んでいただきたい。

---

## 1. (a) wolfSSL が要るメモリ — 設定値と実測

### 1.1 設定（`platformio.ini` と Arduino-wolfSSL の `user_settings.h`）

| 設定 | 値 | 意味 |
|---|---|---|
| `LARGE_STATIC_BUFFERS` | ★ **未定義** | 16KB の静的バッファを持たない |
| `RECORD_SIZE` | **128** | 上の帰結（既定の「小さい静的バッファ」） |
| `STATIC_BUFFER_LEN` | ★ **5** | 静的部分は**レコードヘッダだけ**。残りは全部ヒープ |
| `MAX_RECORD_SIZE` | **16,384** | 規格の上限。そのまま |
| `MAX_MSG_EXTRA` | **70** | `38 + WC_MAX_DIGEST_SIZE` |
| `WOLFSSL_SMALL_STACK` | 定義あり | 大きな一時領域をスタックからヒープへ移す |
| `MICRO_SESSION_CACHE` | 定義あり | セッション表は最小 |
| `WOLFSSL_HAVE_SP_ECC` / `SP_SMALL` | 定義あり | P-256 を固定長で（既に手当て済み） |
| `HAVE_CURVE25519` ＋ `UseKeyShare(X25519)` | 定義あり | TLS1.3 の鍵共有を X25519 に固定（既に手当て済み） |
| ★ `HAVE_MAX_FRAGMENT` | ★ **未定義** | ★ **後述。最大の発見** |

★ **数字は全部、プローブをビルドして実測した**（§8.8-26 に従い `sizeof` を推定しない）:

| 対象 | 実測 |
|---|---|
| `sizeof(WOLFSSL)` | **892 B** |
| `sizeof(WOLFSSL_CTX)` | **1,732 B** |
| `sizeof(Arrays)`（ハンドシェイクの秘密群） | **3,356 B** |
| `sizeof(DecodedCert)`（証明書1枚の解析） | **884 B** |
| `MAX_RECORD_SIZE + MAX_MSG_EXTRA` | ★ **16,454 B** |

### 1.2 ハンドシェイク中の最大の「連続」要求

★ **16,454 B の 1 回の malloc である。**

`STATIC_BUFFER_LEN = 5` なので入出力バッファは**丸ごとヒープ**で、
`GrowInputBuffer()` が**到着したレコードの大きさに合わせて確保し直す**
（`internal.c:10757`）。サーバーが既定の 16KB レコードを送れば、
**16,454 B の連続領域**を要求する。入力と出力で**それぞれ**なので、
最悪時は **約 33 KB の連続領域を 2 枚**。

合計の目安（連続要求ではなく総量）:

    WOLFSSL           892
    WOLFSSL_CTX     1,732
    Arrays          3,356
    入力バッファ   16,454   ★ 連続
    出力バッファ   16,454   ★ 連続（送信は小さいので実際は伸びないことが多い）
    証明書の解析      884 × 鎖の枚数（GitHub は 2〜3 枚）
    ─────────────────────
    ざっと        23〜40 KB、うち ★ 16.5KB が 1 枚の連続領域

fork の実測（matcha-reader #584）は「**ハンドシェイクに約 20 KB**、
最大ブロック 12 KB では **-125 (MEMORY_E)**」で、**この数字と整合する。**

### 1.3 ★ 既に書かれている緩和策が、無効化されている

[freeink-sdk/libs/network/SecureNet/src/SecureClient.cpp:112](../freeink-sdk/libs/network/SecureNet/src/SecureClient.cpp)

```cpp
#ifdef HAVE_MAX_FRAGMENT
  // Ask the peer to cap TLS records at 2KB (RFC 6066 max_fragment_length).
  // ... a default 16KB record demands a ~17KB contiguous allocation per record
  // -- measured failing (MEMORY_E mid-download) at the ~20KB free heap ...
  wolfSSL_UseMaxFragment(ssl, WOLFSSL_MFL_2_11);
#endif
```

★ **`HAVE_MAX_FRAGMENT` は、この `#ifdef` 以外のどこにも現れない。**
`platformio.ini` にも `user_settings.h` にも無い（両方 grep した）。

> **つまり 17KB の連続要求を 2KB に下げる手当ては、
> コメントと一緒に書かれていて、コンパイルされていない。**

★ これは §8.8-32（狼少年の対照実験）と同じ形の罠である ──
**無効な緩和策はコードを読んだ者に「対処済み」と読ませる。**
今回まさに、私も最初はそう読んだ。

---

## 2. (b) OTA は同じ経路か — ★ 同じ。ただし下流は別

★ **ダウンロードは完全に同じ経路である。**

| | 経路 |
|---|---|
| フォント | `FontDownloadActivity` → `HttpDownloader::downloadToFile` → `runGetWolf` → wolfSSL |
| ★ OTA 本体 | `OtaUpdater::installUpdate` → ★ `HttpDownloader::fetchUrl` → `runGetWolf` → wolfSSL |
| ★ OTA の版確認 | `OtaUpdater::checkForUpdate` → ★ `HttpDownloader::fetchUrl` → 同上 |

[OtaUpdater.cpp:118](../src/network/OtaUpdater.cpp) のコメントが明言している ──
「esp_https_ota は mbedTLS 固定で TLS1.3 を話せないので、
**自分で OTA パーティションを回し、ファームウェアは HttpDownloader で流す**」。

★ **よって OTA も同じ理由で落ちる可能性が高い。**
しかも OTA は**版確認の段ですでに TLS を 1 回張る**ので、
「更新があります」の画面に到達する前に落ち得る。

### ★ ついでに見つかった訂正 — `FirmwareFlasher.h` のコメントは誤り

> 「OTA first downloads the firmware to an SD-card cache file, then calls this.」

★ **そうなっていない。**`OtaUpdater::installUpdate` は
`esp_ota_begin` → `esp_ota_write`（受信コールバックの中で直接）→ `esp_ota_end`
→ `esp_ota_set_boot_partition` で、**`flashFromSdPath()` を通らない。**
SD 経由の更新だけが通る。

★ **これは今回の (a) の実装に直接影響した** ──
`archiveCrashReportForUpdate()` を `flashFromSdPath` の 1 箇所に置けば
両経路を覆える、とコメントを信じて書いた。**覆えていなかった。**
`OtaUpdater::installUpdate` 側にも置いた。**コメントは計器と同じで、
古くなっても何も言わない**（§8.8-32 の一般形）。

### ★ OTA は TLS と書き込みを同時にやっている

`esp_ota_write` は**受信コールバックの中**で呼ばれる。
つまり wolfSSL のバッファが立っている最中にフラッシュ書き込みが走る。
Phase E チェックリスト #9 が言う「TLS と書き込みの競合」は、
**構造としては実在する**（下の (d) で、ただし出典は別件だったと判明）。

---

## 3. (c) 閾値は経路ごとに変えられるか — ★ 変えられない

[src/network/NetworkHeapGuard.cpp](../src/network/NetworkHeapGuard.cpp)

```cpp
constexpr uint32_t MIN_FREE = 110000;
constexpr uint32_t MIN_MAX_BLOCK = 50000;
...
const bool enough = freeHeap >= MIN_FREE && maxBlock >= MIN_MAX_BLOCK;
```

★ **ファイル内の `constexpr` 2 本で、全経路が同じ値を使う。**
`Target` は**再起動先の画面**（FileTransfer / Home）を選ぶだけで、
**閾値には触っていない。**私が書いた構造の欠落である。

★ **さらに重要:** 今回の落ち方は閾値では止められなかった。
関門は **Wi-Fi に入る所**（`startWifiScan` と Web サーバーの `onEnter`）にあり、
**ダウンロードの時点には無い。**`net-guard ... ok` を通ったあと、
**同じ関門を二度通らずに**ダウンロードへ進んでいる。

    4404 net-guard  free=137,088  ← ここは通った
    8798 net-join   free=110,840  ← Wi-Fi 自体が 26KB 食った
         → ダウンロード開始（関門なし）→ abort

★ **Wi-Fi に入った後の空きは、入る前の空きから 26KB 以上下がる。**
入口の閾値 110,000 は「入口で 110KB あること」しか言っておらず、
**「TLS を張る時点で何KBあること」を何も保証していない。**

---

## 4. (d) 上流に同種の報告があるか — ★ ある。しかもほぼ同じ

| | 内容 |
|---|---|
| ★ [#3391](https://github.com/crosspoint-reader/crosspoint-reader/issues/3391) | **X3 が「Downloading font list」で abort。**1.6.0 と nightly 両方。`PANIC` / `abort()`、abort PC `0x421c3453` / `0x421c3441`。★ 報告者が **「`-fno-exceptions` が確保失敗を `abort()` にする」と明記**している ── **我々の読みと一致。**ヒープの数値は未記録。2026-09-16 に closed（どの変更で直ったかはページに出ていない） |
| [#3392](https://github.com/crosspoint-reader/crosspoint-reader/issues/3392) | 「Font download always fails (1.6.0 final, X4 Pro)」。#3391 の親 |
| [PR #3377](https://github.com/crosspoint-reader/crosspoint-reader/pull/3377) | 「Improve font download memory handling and free OPDS catalogs on download」 |
| [PR #2475](https://github.com/crosspoint-reader/crosspoint-reader/pull/2475) | **1.5.0 に入っている** 「oom exceptions for OPDS, KOSync, and OTA via wolfssl」 |
| ★ [matcha-reader PR #584](https://github.com/eszter007/matcha-reader/pull/584)（fork） | ★ **実測つきで最も近い。**更新確認の前に SD フォントが常駐したままで **空き 18KB / 最大ブロック 12KB**、ハンドシェイクが **wolfSSL -125 (MEMORY_E)** で失敗。**「ハンドシェイクに約 20KB 要る」**。TLS1.2 での再試行では空きが **約 1KB** まで落ちた。**Wi-Fi を start する前に** SD フォントを解放して **56KB / 51KB**、**370ms で成功**。`OtaUpdateActivity` 内に置き、画面を出るとき再起動してフォントを戻す |

### ★ チェックリスト #9 の根拠は訂正が必要

#9 は「ロック品 X4 で OTA が常に失敗する既知事例（**RAM不足による TLS と書き込みの競合**）」と書いている。
出典は [#1918](https://github.com/crosspoint-reader/crosspoint-reader/issues/1918) と思われるが、
★ **#1918 は報告者が最初 RAM 不足を疑い、最終的に「イメージ検証のバグ（#1786）であって
メモリではない」と結論している。**

★ **よって #9 の括弧内の理由は誤り。**ただし:

- **結論（1.2.0 系を入れない）は変える必要がない** ── 理由が別なだけである
- **そして「TLS と書き込みの競合」は、理由としては誤りだが
  現象としては本物**（§2 の最後、`esp_ota_write` が受信コールバック内）

★ この 2 つを混ぜないこと。**正しい結論を間違った理由で守っている状態**で、
理由を直さないと、次に「メモリを直したから 1.2.0 でもよい」と読まれる。

---

## 5. 選択肢（★ 実装しない。並べるだけ）

安い順。**どれも排他ではない。**

| | 案 | 効果 | 代償 | 根拠 |
|---|---|---|---|---|
| ★ **1** | **`HAVE_MAX_FRAGMENT` を定義する** | 連続要求 **16.5KB → 約 2KB** | Flash 微増。無視するサーバーは従来どおり | ★ **コードは既に書かれている。define 1 行。**最も安く最も効く |
| **2** | **ダウンロードの直前にも関門を通す** | 「入口は通ったが本番で足りない」を塞ぐ | 再起動が増える | §3。いま構造的に抜けている |
| **3** | **閾値を経路ごとに持たせる** | TLS 経路に実測の値を当てられる | 値を 2 組測る必要がある | §3。`Target` に閾値を持たせれば済む |
| **4** | **ダウンロードの前に SD フォントを解放する** | fork の実測で **18KB → 56KB** | 戻すのに再起動（fork もそうしている） | ★ matcha-reader #584 |
| **5** | **素の `new` を潰す** | abort を**扱える失敗**に変える | 広い。どこが落ちたか先に特定が要る | §0。★ **ただしこれは「足りない状態にしない」ではない** ── 焼き11 で学んだとおり、順序は 1〜4 が先 |

★ **推すのは 1 → 2 → 4 の順である。**
1 は**既に書かれた意図を有効にするだけ**で、連続要求という
いちばん厳しい制約を 8 分の 1 にする。
2 は構造の穴で、埋めないと 1 や 4 をやっても「次の経路」で同じ目に遭う。

★ **測り直す必要があるもの**: 1 を入れたあとの
`epub-css-*` 相当の計器 ── すなわち **TLS を張る直前と直後の `free` / `max`**。
いま計器が `net-join-before` で止まっており、
**ハンドシェイクの前後が測れていない。**§8.8-36 のとおり、
計器は**抱えている場所**に要る。

---

## 6. (e) 配布との接続 — Pretendard は韓国語のフォントである

★ 落ちたのは `PretendardMedium_10.cpfont` である。
上流のフォントサーバー（`crosspoint-fonts`）の品揃えに
**日本語の UD フォントは無い**。

配布時に `FONT_MANIFEST_URL` を自分の置き場へ向ける話
（[FontDownloadActivity.h:17](../src/activities/settings/FontDownloadActivity.h) の
`#ifndef FONT_MANIFEST_URL`、**ビルドフラグで差し替えられる**）と
直接つながるので、Phase E のチェックリストに項目として足した。

★ **ただし順序に注意**: 置き場を自分に向けても、
**TLS が入らない問題はそのまま残る**（同じ `HttpDownloader` を使う）。
**#18 を先に直さないと、自分の置き場でも落ちる。**

---

## 7. ★ 案1 を入れた (2026-11-17)

### 定義した場所と理由

[platformio.ini](../platformio.ini) の wolfSSL フラグ群、`-DHAVE_SNI` の隣。

★ **`user_settings.h` ではない理由**: あのファイルは
`.pio/libdeps/default/Arduino-wolfSSL/` の中、すなわち**依存パッケージの一部**で、
**更新のたびに作り直される。**そこへの編集は**警告なしに消える。**

★ **`platformio.ini` の `build_flags` はライブラリの翻訳単位にも届く。**
これは推測ではなく、**同じファイルに証拠がある** ── すぐ上の
`HAVE_FFDHE_2048` / `HAVE_CURVE25519` のコメントが
「ここで重複定義すると**ライブラリの全翻訳単位から**
マクロ再定義の警告が出る」と書いている。

### ★ ライブラリ側に届いていることの確認（3 通り、全部機械）

**(1) 届かなければコンパイルが通らない。**
wolfSSL は**宣言も同じ `#ifdef` の中**に置いている:

    wolfssl/ssl.h:3938   WOLFSSL_API int wolfSSL_UseMaxFragment(WOLFSSL*, unsigned char);
    #endif /* HAVE_MAX_FRAGMENT */   ← 3942 行

    src/ssl.c:3122       #ifdef HAVE_MAX_FRAGMENT
    src/ssl.c:3125       int wolfSSL_UseMaxFragment(WOLFSSL* ssl, byte mfl)

★ **アプリ側だけに届いた場合は宣言も無いので「not declared in this scope」で落ちる。**
通ったこと自体が片側だけでない証拠になる。

**(2) `nm` で前後を突き合わせた。**

| | `wolfSSL_UseMaxFragment` | `TLSX_UseMaxFragment` |
|---|---|---|
| 焼き12 の `.elf`（定義前） | ★ **0 個** | ★ **0 個** |
| 今回（定義後） | ★ `T` あり | ★ `T` あり |

**(3) wolfSSL 自身のオブジェクトに入っている。**
`libf9f/Arduino-wolfSSL/src/tls.c.o` に `T TLSX_UseMaxFragment` ──
**拡張の機構そのものがライブラリ側でコンパイルされている。**

### Flash の増分

| | 焼き12 | 今回 | 差 |
|---|---|---|---|
| RAM | 50,412 | **50,420** | ★ **+8 B** |
| Flash | 5,663,889 | **5,664,657** | ★ **+768 B** |

★ **+768 B。**計器（4 点）も同じ差に含まれている。

### ★ 効くかどうかは、サーバー次第である（正直に書く）

`max_fragment_length` は**お願い**であって命令ではない。
RFC 6066 を無視するサーバーは 16KB の記録を送り続け、
その場合は**従来どおり 16,454 B の連続要求が起きる。**

★ **GitHub と、そのリダイレクト先の CDN が従うかは分からない。**
だから次節の計器を入れた ── **数字で見るまで「直った」と書かない。**

---

## 8. ★ 張る瞬間の計器

`SecureClient` に 4 点。`/heap.txt` に出る:

| タグ | いつ | 見るもの |
|---|---|---|
| `tls-before` | TCP 接続後、`wolfSSL_connect` の直前 | ハンドシェイクが始まる床 |
| `tls-handshake` | ハンドシェイク完了直後 | ★ **山。**証明書鎖を受けて入力バッファが伸びきった所 |
| `tls-record` | 最初の応用記録を復号した直後 | ★ **ここが案1 の答え** |
| `tls-after` | セッション解放後 | 戻ったか |

各行の `detail` に ★ **`maxfrag=2048`**（未定義なら `maxfrag=0`）が付く ──
**どちらのファームウェアが書いた行かをログだけで判別できる**ようにした
（§8.8-32 の「基準がいつのものかを実行時に言う」）。
`tls-record` には受けた記録の `bytes=N` も付く。

### ★ 設計上の判断

- **関数ポインタで渡す**（`std::function` を使わない）──
  CLAUDE.md の方針どおり。ヒープを測る場所でヒープを確保する実装は採れない
- ★ **SDK にアプリの診断を持ち込まない** ──
  `freeink-sdk` 側は `HeapProbe` という関数ポインタを 1 本持つだけ。
  `heapdiag::sample` への橋渡しは `HttpDownloader.cpp`（アプリ側）に置いた
- ★ **外から測れないので内に置くしかなかった** ──
  受信バッファは `wolfSSL_read` の**中で**伸びて解放されるので、
  アプリが呼ぶ 2 つの呼び出しの**あいだ**に山が来る。§8.8-36 のとおり
- `tls-after` は**セッションが実在したときだけ**出す ──
  `stop()` は接続試行の先頭でも呼ばれるので、
  無かったものの解放を 1 行出すと**解放に見えて解放でない行**になる

### ★ 案1 の前後で何を見るか

    tls-handshake free=... max=...  maxfrag=2048
    tls-record    free=... max=...  maxfrag=2048 bytes=2048   ← 2048 なら従った
    tls-record    free=... max=...  maxfrag=2048 bytes=16384  ← 無視された

★ **連続要求の最大は `max` の落ち込みで見る。**
`tls-before` の `max` と `tls-handshake` の `max` の差が、
**その 1 枚の確保の大きさ**である。16.5KB 落ちれば効いておらず、
2KB 程度なら効いている。

### ★ 案2 の要否は、この数字を見てから

同意いただいたとおり、**案1 で足りるなら案2（再起動が増える）は要らない。**
閾値が経路ごとに変えられない件（`NetworkHeapGuard.cpp` のファイル内
`constexpr` 2 本、`Target` は再起動先の画面だけを選ぶ）は
**案2 を入れるときに一緒に直す。今回は記録のみ。**

---

## 9. ★ コメントを根拠にした手当ての洗い出し

呼び出し元を**数えた**（コメントを読まずに）:

    grep -rn "flashFromSdPath" --include=*.cpp src    → 1 件
    grep -rn "sameAsRunning"   --include=*.cpp src    → 1 件

| 対象 | コメントの主張 | ★ 実際 | 影響 |
|---|---|---|---|
| `flashFromSdPath` | 「SD と OTA の**両方がここに来る**」 | ★ **SD だけ。呼び出し元 1 箇所** | crash_report の退避が OTA を覆っていなかった（`4ea6064a` で修正） |
| ★ `sameAsRunning` | STATUS #13「**共有済み**、要るのは確認の段だけ」 | ★ **呼び出し元 1 箇所。しかも `const char* sdPath` を取る** | ★ **OTA は SD にファイルを置かないので、そのままでは呼べない。**STATUS #13 を訂正した |
| `runningPartitionChipId` | （主張なし） | ★ **本当に共有**（`FirmwareFlasher.cpp:145` と `OtaUpdater.cpp:156`） | 問題なし |
| 第 3 の書き込み経路 | ─ | ★ **無い**（`esp_ota_*` / `ota_boot::switchTo` の grep で 2 ファイルのみ） | Web からの焼きは存在しない |

`FirmwareFlasher.h` の当該コメントは、**誤りであることと、
それを信じて何が起きたか**を書いた形に差し替えた
（消すと、次の人が同じ推論をして同じ罠に落ちる）。


---

## 10. ★ 計器は submodule の中にある（版文字列が `-dirty` になる）

★ **`SecureClient` は `freeink-sdk` の中で、これは git submodule である。**
`origin` は**上流**の `https://github.com/Free-Ink/freeink-sdk.git`、
`a485dc46` で detached。**こちらから commit できない。**

| | |
|---|---|
| 作業ツリーの編集 | 動く（ビルドに入る）が ★ **どこにも記録されない** |
| `git submodule update` | ★ **黙って捨てる** |
| 親リポジトリでの見え方 | `M freeink-sdk` |
| ★ 版文字列 | ★ **`-dirty` が付く** |

★ **`-dirty` が付くのは正しい。**バイナリに**どのコミットにも無いコード**が
入っているので、§8.8-34 の仕掛けが**意図どおり働いている。**
抑制してはいけない。

### 取った措置

`patches/freeink-sdk-tls-heap-probe.patch` として**親リポジトリの履歴に入れた。**
submodule の作業ツリーが消えても、仕事は残る。
→ [patches/README.md](../patches/README.md)

### ★ 本当の解決は羅闇さんの判断である

`freeink-sdk` を**自分の fork に向け替え**、そこへ commit して
親にポインタを記録すれば、この patch は普通のコミットになり
`-dirty` も消える。★ **これはリポジトリ構成の決定なので、
クラッシュを直すついでに黙ってやることではない。**

★ **急ぐ必要はない** ── 計測のためだけなら、`-dirty` のまま焼いて構わない。
むしろ**ラベルが付いているほうが安全**である（配布物と区別が付く）。

---

# 第2部 — 焼き13 の結果：原因が入れ替わった (2026-11-17)

★ **案1 は効いた。**焼き12 は最初の `wolfSSL GET` で即死していたが、
焼き13 は**ハンドシェイクが3回とも成功**した。

★ **そして原因が移った** ── 「TLS が入らない」ではなく
「**ダウンロードを重ねるごとにヒープが悪化する**」。

| 回 | `tls-before` の空き | 最大ブロック | 結果 |
|---|---|---|---|
| ① | 46,780 | **42,996** | 完走 |
| ② | 44,664 | **34,804** | 完走 |
| ③ | 23,100 | ★ **9,204** | ★ **abort** |

★ **空きは戻るのに最大ブロックが戻らない** ── 教科書どおりの断片化である。
①の `tls-after` は `free=46,804`（**+24 B、ほぼ完全に戻っている**）なのに
`max=18,420`（**−24,576 B**）。

## 10. (e) `0x421b3da9` の解決

    0x421b3da9: __cxxabiv1::__terminate(void (*)())
                at libsupc++/eh_terminate.cc:45

★ **前回（`0x421b36d7` = `eh_personality.cc`）の 1 段下**である。
同じ連鎖の別の地点を捉えただけで、**意味は同じ**:
投げられた例外が受け手を見つけられず `terminate` に来た ──
**素の `new` の `std::bad_alloc`。**

★ **TLS が失敗したのではない。**wolfSSL は `XMALLOC` が NULL を返し
`SecureClient` が `return 0` する経路を持っており、abort にはならない。
**TLS が食った結果、別の誰かが落ちた。**

## 11. (b) 接続を張り直しているか — ★ 張り直している。しかも設計を裏切っている

[SecureHttpClient.h:11-14](../freeink-sdk/libs/network/SecureNet/include/SecureHttpClient.h) の宣言:

> Connections are kept alive and reused across requests to the same
> scheme://host:port (see setReuse), so a burst of requests — an OPDS crawl,
> a sync exchange, **a multi-file download** — pays for **one TLS handshake
> instead of one per request**.

★ **機能は実装されている**（`_reuse = true` が既定、`keep-alive` ヘッダ、
`closeConnection()` の条件分岐も全部ある）。

★ **呼び出し側が無効にしている。**`runGetWolf` は

    for (int hop = 0; hop <= MAX_REDIRECTS; ++hop) {
      freeink::SecureHttpClient http;      ← ★ ループの中で構築

**毎ホップごとに生まれて死ぬ**ので、keep-alive のソケットは
**リダイレクト 1 回ごとに捨てられる。**さらに `downloadToFile` は
**ファイルごとに `runGetWolf` を呼ぶ**ので、使い回しは一度も起きない。

★ **GitHub は CDN へリダイレクトするので、1 ファイルにつき TLS セッションは 2 本。**
3 ファイルで 6 本。羅闇さんの観測（3 回の `tls-before`）は
**完走した分だけ**が見えている数字である。

★ **これは §8.8-37 と同じ形である** ── 機能は書かれていて、効いていない。
あちらは `#ifdef` で、こちらは**オブジェクトの寿命**で無効化されている。

## 12. (a) `tls-after` で最大ブロックが戻らない — 何が残るか

`SecureClient::stop()` は 3 つとも解放している（コードで確認）:

    wolfSSL_free(_ssl)        → WOLFSSL 892 B ＋ 入出力バッファ
    wolfSSL_CTX_free(_ctx)    → WOLFSSL_CTX 1,732 B
    _transport.stop()         → WiFiClient

★ **CTX は使い回していない** ── `connectWithMethod` が毎回 `wolfSSL_CTX_new` する。
これは**無駄だが漏れではない**（毎回解放されている）。

★ **戻らないものの最有力候補は lwip である。**`sdkconfig.default` の実測値:

    CONFIG_LWIP_TCP_SND_BUF_DEFAULT = 5,744
    CONFIG_LWIP_TCP_WND_DEFAULT     = 5,760
    CONFIG_LWIP_TCP_MSS             = 1,436
    CONFIG_LWIP_MAX_SOCKETS         = 16

★ **1 ソケットあたり送受信で約 11.5 KB**、加えて PCB。
そして **TCP は閉じたあと PCB を TIME_WAIT で保持する**（lwip の既定 MSL は 60 秒）。

★ **ダウンロードは 4.4 秒・8.8 秒……と 60 秒の中で起きている。**
つまり**前の接続の PCB が生きたまま次の接続が張られる。**
①の `tls-after` で `max` が 24,576 B 落ちて戻らないのは、
**この積み上がりと整合する。**

★ **ただしこれは仮説である。**lwip の PCB は確保したことを我々に言わない。
確かめるには **`tls-after` の後にもう 1 点**（あるいは
`esp_get_free_internal_heap_size` と `lwip_stats`）が要る。
**今は「整合する」までしか言えない。**

## 13. (c) SD への書き込みバッファ — ★ 寄与は小さい

| | 1 ダウンロードあたり |
|---|---|
| `HalFile::Impl` | `std::make_unique` 1 個（数十 B）、`close()` で解放 |
| SdFat のセクタキャッシュ | ★ **マウント時に確保、使い回し**（1 ダウンロードごとではない） |
| `Sink::write` / `Sink::progress` | ★ **`std::function` 2 本** ── 閉包をヒープに置く（各数十 B） |

★ **大きな per-download バッファは無い。**
`std::function` 2 本は CLAUDE.md の方針に反するが、**規模が違う**
（数十 B 対 11 KB）ので、ここが原因ではない。

## 14. (d) 進捗画面のグリフ読み込み — ★ 機構としては本物。しかも前例がある

★ **`FontDownloadActivity` の進捗コールバックは、受信チャンクごとに
`requestUpdate(true)` を呼んでいる**（`READ_CHUNK = 2048` なので
200 KB のフォントで **約 100 回**）。

★ **同じ問題を OTA 側は既に手当てしている**
（[OtaUpdater.cpp:167](../src/network/OtaUpdater.cpp)）:

> Fire the callback only on whole-percent change. **Per-chunk updates wake the
> render task, whose framebuffer work contends with TLS on the internal arena**,
> and e-ink can't repaint faster than a percent tick anyway.

★ **フォント側には、この手当てが無い。**

そして描画で確保されるものは**短命ではない**:
[SdCardFont.h:298](../lib/EpdFont/SdCardFont.h) ──
「amortizes SD reads. **Cleared only on font unload or `clearPersistentCache()`**」。

★ **つまりグリフと送り表は常駐する。**
`net-guard` が入口で `releaseFontCaches(true)` を呼んで空にしているので、
**ダウンロード中に 0 から積み直される** ── TLS と lwip の大きな塊の
**あいだに、長命の小さな塊が挟まる。**

★ **これが「空きは戻るのに最大ブロックが戻らない」の素直な説明である。**
解放された 11 KB の穴の中に、**解放されないグリフが座っている。**

## 15. (3) 計器を直した — `bytes=1` は私の誤りだった

★ **`bytes=` はレコードの大きさではなく `wolfSSL_read()` の戻り値だった。**
`SecureHttpClient` は**応答ヘッダを 1 バイトずつ**読む
（`line += static_cast<char>(ch)`、:494）ので、
**最初の read は必ず 1 バイト**である。3 回とも 1 が出たのは正しく、
**答えていた問いが違った。**§8.8-29（1 が何を数えているか）に当たる。

### 本物の数字が取れた

`ssl->buffers.inputBuffer.bufferSize` を**直接読む**ようにした
（`wolfssl/internal.h` を include。公開 API に無い）。
★ **これが「GitHub が 2KB の要求に従ったか」の答えである** ──
`GrowInputBuffer()` が**相手が送ってきた最大のレコードに合わせて**
この値を決めるので:

    inbuf=2117 前後  → ★ 従った
    inbuf=16454      → ★ 無視された

★ wolfSSL の版が上がってこのフィールドが動いたら**コンパイルが通らなくなる** ──
それが正しい壊れ方である（§8.8-30、**黙って違う場所を読む計器は無いより悪い**）。

### 出る行（焼き14）

    tls-before    free=... max=...  s=1 maxfrag=2048 inbuf=0     outbuf=0
    tls-handshake free=... max=...  s=1 maxfrag=2048 inbuf=N     outbuf=M
    tls-record    free=... max=...  s=1 maxfrag=2048 inbuf=N     outbuf=M read=1
    tls-after     free=... max=...  s=1 maxfrag=2048 dfree=+24 dmax=-24576

★ 変えたもの 4 つ:

1. ★ **`inbuf=` / `outbuf=`** ── 受信バッファの実寸。**これが正式な指標**
2. ★ **`s=N`（セッション番号）** ── 回を重ねるごとの悪化が本題になったので、
   **何本目かを言わない行は読めない**
3. ★ **`tls-after` は差で出す**（`dfree` / `dmax`）──
   「**戻るべきものが戻っているか**」が 1 行で分かる。
   30 行離れた 2 行を引き算させない（指示 4 への対応）
4. **`bytes=` → `read=`** に改名し、**レコード長ではない**と明記

### ★ 8,192 という値について — 分からない

★ **正直に書く: `tls-before` → `tls-handshake` の `max` の減り 8,192 を、
特定の 1 個の確保に結び付けられていない。**

分かっていること:

- 2048 を要求したのに 8,192 落ちているので、**要求が通ったとしても
  受信バッファ 1 枚では説明できない**
- `max` の減りは「1 枚の確保の大きさ」とは**限らない** ──
  最大ブロックの**内側**に小さな確保が置かれれば、
  そのブロックは 2 つに割れ、`max` は**確保量よりずっと大きく**落ちる
- ★ **同じ行で `free` は 2,080 B **増えて** いる**（46,780 → 48,860）。
  ハンドシェイクは**正味で返している** ── 一時領域を解放したうえで、
  どこかに小さな杭を打った形である
- 候補: lwip の PCB / 受信 pbuf（§12）、`FP_MAX_BITS=8192` に由来する
  fast-math の一時領域（1 個 2,048 B）、証明書鎖の複製

★ **`inbuf=` が入った焼き14 で決まる** ── `inbuf` が 2KB 台なら
8,192 は受信バッファではなく、断片化か lwip である。

★ **指標としては「`max` の減り」を正式にする**（指示 3 のご提案どおり）。
ただし上の 2 番目の理由から、**これは「確保の大きさ」ではなく
「連続領域がどれだけ失われたか」と読むこと。**より厳しい側の数字である。

## 16. 案（★ 実装しない）

安い順。**(d) が最も安く、前例がある。**

| | 案 | 効果 | 代償 | 根拠 |
|---|---|---|---|---|
| ★ **A** | **進捗の更新を整数%に絞る**（OTA と同じ） | 描画回数 約100 → 約100分の1。**長命グリフの挟まりが減る** | 進捗バーが粗くなる（e-ink は毎%も描けない） | ★ **OTA に同じ手当てが実装済み。**[OtaUpdater.cpp:167](../src/network/OtaUpdater.cpp) |
| ★ **B** | **`SecureHttpClient` をループの外へ出す** | ★ **設計が意図した keep-alive が初めて効く。**TLS セッション 6 本 → 1〜2 本 | リダイレクト先ホストが変わる回は結局張り直す（GitHub→CDN） | ★ **ヘッダが「multi-file download は 1 回のハンドシェイクで済む」と宣言している** |
| **C** | **ダウンロードの前にフォントキャッシュを解放** | 挟まった長命の塊を落とす | ファイルごとに再読込（遅くなる） | matcha-reader #584 と同じ考え |
| **D** | **ファイルごとに新しい `SecureHttpClient`＋間に待ち** | TIME_WAIT を抜けさせる | 1 ファイルごとに最大 60 秒 ── ★ **論外** | § |
| **E** | **lwip のソケットバッファを削る** | 1 接続の代償そのものを下げる | 転送速度が落ちる。全ネットワーク機能に影響 | `TCP_SND_BUF`/`TCP_WND` |

★ **推すのは A → B** である。

- **A** は**他人が同じ問題を同じコードベースで既に解いている** ──
  真似るだけで、リスクがほぼ無い
- **B** は**書かれた意図を有効にするだけ**（§8.8-37 と同じ構図）。
  ただし `runGetWolf` のリダイレクト処理に手が入るので A より重い
- ★ **C は A と B を測ったあとで判断する。**
  グリフを落とすと**描画が遅くなり、進捗が余計に遅れる**ので、
  A で描画回数が減っていれば**不要になる可能性がある**

★ **測ってから決める。**焼き14 の `inbuf=` と `dmax=` が揃えば、
「受信バッファか／断片化か／lwip か」が切り分けられる。

---

## 17. ★ 案A を入れた（焼き14） (2026-11-17)

### 刻みは 5%。根拠

| | |
|---|---|
| 真似た元 | ★ [`OtaUpdater.cpp:167`](../src/network/OtaUpdater.cpp)「Fire the callback only on whole-percent change」。**コメントで参照している** |
| 刻み | ★ **5%**（1ファイルにつき**最大20回**の描画。従来は約100回） |
| 従来 | `READ_CHUNK = 2048` なので 200KB のフォントで**約100回** |

★ **なぜ 1% ではなく 5% か** ── 進捗バーは「内容幅 − 両側の余白」で、
**最も狭い向きでも約 450 px**。1% は **約 4.5 px** で
**バー自身の枠線より細く**、しかも e-ink の更新は 1〜2 秒かかるので
**100 回は原理的に描けない。**5% なら **約 22 px 動く** ── 動いて見える。

★ **小さいファイルは従来どおり**: 5% が 1 チャンクより小さいファイルでは
毎チャンク報告になる（刻みが効くのは大きいファイルだけ）。

### ★ なぜフォント側では OTA より重要か

描画はグリフを読む。そしてグリフと送り表のキャッシュは**常駐する** ──
[`SdCardFont.h:298`](../lib/EpdFont/SdCardFont.h)
「Cleared only on font unload or `clearPersistentCache()`」。

★ **`net-guard` が入口で空にしているので、ダウンロード中に 0 から積み直される。**
TLS セッションが**取って返す短命の塊のあいだに、長命の塊が挟まる。**
焼き13 の数字がそれを示している ──
**空きは 24 バイト差まで戻り、最大ブロックは 42,996 → 34,804 → 9,204。**

### ★ OTA の真似をしなかった 1 点 — キャンセル

OTA はコールバック**全体**を絞っている（あちらのコールバックは描画だけ）。
★ **こちらは同じコールバックで Back ボタンも拾っている。**
全体を絞ると**キャンセルの判定が 5% ごとになり、
利用者はボタンを押し続けないと気付かれない。**

よって:

    mappedInput.update() と Back の判定 → ★ 毎チャンク（絞らない）
    requestUpdate(true)               → ★ 5% 刻み

キャンセルが押されたときは**その場で 1 回描く**（待たせない）。

### 止まって見えないことの確認

バーの上の行は `"<family> (3/11)"` と出る
（[`FontDownloadActivity.cpp:643`](../src/activities/settings/FontDownloadActivity.cpp)）。
★ **1 ファイルのバーが刻みの谷にいる間も、ファイル数のほうが進む。**
`.cpfont` は 1 つが大きくないので、**同じ百分率に長く留まらない。**

### 焼き14 で見る列

★ 焼き13 と**同じ列**を並べれば効果が分かる:

| 回 | 焼き13 `max` | 焼き14 `max` |
|---|---|---|
| ① `tls-before` | 42,996 | ? |
| ① `tls-after` | **18,420**（`dmax=−24,576`） | ? |
| ② `tls-before` | 34,804 | ? |
| ③ `tls-before` | **9,204** → abort | ? |

★ `s=N` と `dmax=` が入ったので、**悪化が 1 行で読める。**

★ **案B（`SecureHttpClient` をループの外へ）は入れていない。**
A を測ってから判断する。

---

# 第3部 — 焼き14 の結果と案B (2026-11-18)

## 18. ★ 案1 は完全に成功。受信バッファは犯人ではなかった

    tls-record ... inbuf=2065

★ **16,454 ではなく 2,065。GitHub は `max_fragment_length` に従った。**
§1.1 で立てた「連続要求 16.5KB」は**消えた。**

★ **しかしそれで直らなかった:**

    s=1 tls-after  dfree=-8  dmax=-23,552

★ **2KB のバッファなのに、最大連続ブロックが 23.5KB 失われ、
空きはほぼ完全に戻っている。**
失われているのは**バッファではなく連続性**である。

| 回 | 入る時の空き | 入る時の最大 | 結果 |
|---|---|---|---|
| s=1 | 46,696 | **42,996** | 完走（`dmax=−23,552`） |
| s=2 | 44,760 | **32,756** | 完走 |
| s=3 | 22,920 | **16,372** | 完走（`dmax=−9,984`） |
| s=4 | 21,200 | **16,372** | ★ **ハンドシェイクに到達せず** |
| s=5 | 21,200 | **15,348** | ★ **同じ** |

★ **s=4 以降は `tls-before` から `tls-after` へ直行している**
── `tls-handshake` が出ていない。
**TLS を張る前に（あるいは張る途中で）諦めている。**

★ **そしてクラッシュしなくなった**（「Download failed」で止まる）。
案A と nothrow の成果である。**落ちるより止まるほうがよい。**

★ **案A の効果は小さかった**（`dmax` が −24,576 → −23,552、**4%**）。
グリフは寄与していたが**主犯ではない。残すが、これで終わりではない。**

## 19. ★ 案B を実装した

### 何をしたか

    for (int hop = 0; hop <= MAX_REDIRECTS; ++hop) {
      freeink::SecureHttpClient http;      ← ループの外へ出した

さらに**ファイルをまたいだ使い回し**も入れた
（ご指摘のとおり、そちらのほうが効く）:

    HttpDownloader::KeepAliveScope keepAlive;   // ← downloadFamily() の先頭
    for (size_t i = 0; i < family.files.size(); i++) { ... }

`KeepAliveScope` が生きている間、`HttpDownloader` の中の
**1 本の `SecureHttpClient` が共有される。**

★ **`begin()` は接続を閉じない**（コードで確認） ──
ヘッダ・本文・状態だけを戻し、閉じるか使い回すかは
`ensureConnected()` が `scheme`/`host`/`port` を比べて決める
（`connectionMatches()`）。**だから同じ物を使い回すだけで keep-alive が効く。**

### ★ 何本に減る見込みか

★ **ホストが変わるリダイレクトは結局張り直しになる** ──
`connectionMatches()` が正しく拒否する。

| | 焼き14 | ★ 焼き15 の見込み |
|---|---|---|
| 1 ファイルあたり | **2 本**（github.com ＋ CDN） | 1 本目: 2 本／2 本目以降: ★ **0 本** |
| N ファイルの族 | ★ **2N 本** | ★ **2 本** |

★ **根拠**: 族のファイルは**同じリリース URL の下**にあるので、
リダイレクト先の CDN ホストは**同じはず**である。
2 ファイル目の `github.com` への要求も、1 ファイル目で張った
`github.com` の接続に当たる。

★ **「はず」である。**CDN が**ファイルごとに別のホスト名を返す**なら
減らない。**だから s=N で数える。**

### ★ 確かめ方 — `s=N` を数えるだけでよい

`s=N` は `tls-before` で増える。`tls-before` は
**ハンドシェイクを張ろうとするときだけ**通る。
よって:

    1 族のダウンロード中の tls-before の行数 = TLS セッションの本数

| 読み | 意味 |
|---|---|
| ★ **2 行**（`s=1`,`s=2`） | ★ **案B が意図どおり効いた** |
| 2N 行 | 効いていない（CDN のホストが毎回違う等） |
| その間 | 一部だけ効いた |

あわせて `http-keepalive-open` / `http-keepalive-close` を出すので、
**族の始まりと終わりが log 上で挟める。**

### 設計上の判断

★ **`KeepAliveScope` は RAII で、デストラクタが閉じることが目的である**
（整頓ではない）── 待機中の keep-alive 接続は
**wolfSSL のセッション ＋ lwip のソケットバッファ（5,744 + 5,760 B）**を
抱え続ける。**これは今まさに削ろうとしている代償そのものである。**
だから**static にも長命にもしない。**

★ **確保に失敗したら従来どおりに落ちる** ──
`makeUniqueNoThrow` が null なら `LOG_ERR` して
**1 要求 1 接続に戻る**（機能低下、停止ではない）。

## 20. (3)(a) TIME_WAIT の検証 — 自動で測る形にした

★ **`CONFIG_LWIP_TCP_MSL = 60000` を確認した**（`sdkconfig.default:2125`）。
lwip は閉じた接続の PCB を `tcp_tw_pcbs` に移し、**60 秒保持する。**
★ **焼き14 のダウンロードは全部その 60 秒の中で起きている。**

手順を人にやらせるのではなく、**画面が自分で測る形**にした:

族のダウンロードが終わった時点を 0 として、
`/heap.txt` に `dl-cooldown` を **4 回**出す:

    dl-cooldown  free=... max=...  t=0s  pcb act=.. tw=.. bound=..
    dl-cooldown  free=... max=...  t=15s pcb act=.. tw=.. bound=..
    dl-cooldown  free=... max=...  t=30s pcb act=.. tw=.. bound=..
    dl-cooldown  free=... max=...  t=65s pcb act=.. tw=.. bound=..   ← ★ MSL の直後

★ **やっていただくこと: ダウンロードが終わったあと、
その画面に 70 秒ほど留まる**（戻らない）。それだけである。

★ **読み方 ── どちらの答えも収穫である:**

| 観測 | 結論 |
|---|---|
| ★ `t=65s` で `max` が戻り、`tw=0` になる | ★ **TIME_WAIT で確定。**案は「接続本数を減らす」で正しい |
| `max` が 65 秒でも戻らない | ★ **TIME_WAIT ではない。何かが漏れている** ── 仮説が外れ、探す先が変わる |
| `t=0s` で既に戻っている | 失われていたのは接続ではなく**ダウンロード中だけの何か** |

`t=0s` と `t=15s` / `t=30s` を置いたのは、
★ **「65 秒で戻った」と「もっと早く戻っていたのに遅く見ただけ」を
区別するため**である。

### 実装上の注意

`loop()` から出している（タスクを作っていない）──
**積むスタックが無く、終わったあとの画面は暇だから。**
`millis()` の差で判定し、4 点出し終わったら**armを解除して見るのをやめる。**

## 21. (3)(b) lwip のメモリ使用量 — ★ バイトは取れない。接続数は取れる

★ **取れないもの: バイト数。**

    sdkconfig.default:2077   # CONFIG_LWIP_STATS is not set

★ **lwip の計測機構そのものがコンパイルされていない。**
`lwip_stats` と `MEMP_*` の計数は**この build に存在しない。**
有効にするには **lwip を再ビルドする**ことになり、別の判断である。
★ **よって「lwip が何バイト持っているか」は取れない。取れないと書く。**

★ **取れるもの: 接続の数と状態。**
これは統計を要らない ── lwip が**リストそのもの**を宣言している:

    lwip/priv/tcp_priv.h:336   extern struct tcp_pcb *tcp_bound_pcbs;
    lwip/priv/tcp_priv.h:338   extern struct tcp_pcb *tcp_active_pcbs;
    lwip/priv/tcp_priv.h:341   extern struct tcp_pcb *tcp_tw_pcbs;   ★ TIME-WAIT

SDK の include ツリーに**実際に存在する**ことを確認した。
`src/network/NetDiag.{h,cpp}` で**連結リストを数える**:

    pcb act=N tw=M bound=K

★ **`tw=` がこれを書いた目的である** ── **閉じたのに lwip が抱えている接続の数。**

### 設計上の注意

- ★ **private ヘッダを意図して使っている。**公開 API に手段が無く、
  `CONFIG_LWIP_STATS` 無しの build ではここしか答えが無い。
  SDK の更新で名前が変われば**コンパイルが通らなくなる** ──
  **診断としては正しい壊れ方**である（§8.8-30）
- ★ **`LOCK_TCPIP_CORE()` を取って歩く** ──
  リストは TCP/IP タスクのもので、
  `CONFIG_LWIP_TCPIP_CORE_LOCKING=y` がこの build で有効
- 歩数に上限（64）を置いた ── **壊れたリストで診断が無限に回らないように**
  （`CONFIG_LWIP_MAX_SOCKETS` は 16 なので十分上）

## 22. ★ まだ分かっていないこと（推測で埋めない）

1. ★ **`dmax` が −23,552 になる正体。**受信バッファ（2,065 B）でも
   グリフ（案A で 4% しか動かなかった）でもない。
   **lwip か、まだ見ていない誰かである。**(3)(a)(b) がこれに答える
2. ★ **s=4 以降が `tls-handshake` に到達しない理由。**
   `max=16,372` は wolfSSL の `CTX`(1,732) + `SSL`(892) には十分なので、
   **確保に失敗したのは別の何か**である。
   `tls-before` は出て `tls-handshake` が出ないので、
   **`wolfSSL_connect` のループの中**で諦めている
   ── TCP 接続自体が張れていない可能性もある
   （`_transport.connect()` の失敗は `tls-before` より前なので、
   そこで落ちているなら `tls-before` も出ない ── **出ているので TCP は張れている**）
3. ★ **8,192 / 23,552 という数の由来。**§15 の留保はそのまま有効:
   `max` の減りは**1 個の確保の大きさではなく、失われた連続性**である

---

# §23 焙き15 の結果 — 落ちた場所が変わった（案B は無罪）

## 23.1 何が起きたか

版は `1.5.0-jp-dev-5619742d-dirty`。`abort()` at PC `0x421b427d`
= `__cxxabiv1::__terminate` ──
焙き13 の `0x421b3da9`、焙き12 の `0x421b36d7` と**同じ鎖の別の点**。
素の `new` の `bad_alloc` である。

★ **フォント本体のダウンロードには一度も入っていない。**
ログは `Discovery complete: 3 families` で終わる。

## 23.2 案B が無罪である根拠

| | 焙き14 | 焙き15 |
|---|---|---|
| `net-join-before` 空き | 100,428 | **91,048**（★ −9,380） |
| 一覧取得後の `max` | 27,636 | ★ **27,636（1バイトも違わない）** |
| 一覧取得後の空き | 52,396 | **45,612**（−6,784） |
| s=1 `dmax` | −24,576 / −23,552 | −25,088 |
| s=2 `dmax` | −5,120 / +0 | ★ **+3,072** |

★ **接続が 1 本も無い時点ですでに 9,380 少ない。**
案B は原因になれない。
さらに `KeepAliveScope` は `downloadFamily()` の中にしか無く、
一覧の取得はその外なので、
★ **案B は焙き15 で一度も実行されていない**
（`http-keepalive-*` と `dl-cooldown` が 0 行）。**判定は保留。**

`net-join-before` の空きはログ全体で
**62,356 〜 110,900** と 48,544 もらつく（低い側は直前に `reader-exit-*`）。
★ **版による後退ではない。**
ただし ★ **9,380 の正体は分からない** ──
起動から `net-join-before` までに計器が無い。
549 ms 前のグリフ overflow（slot 101/128）は状況証拠であり、証拠ではない。

## 23.3 落ちていた場所 ── 一覧の構築

`Discovery complete` は `fontInstaller_.refreshRegistry()` で、
これは **`JsonDocument doc` が生きている真ん中**で呼ばれていた。
その直後の解析ループに計器が 1 つも無い。

実物のマニフェストを取って数えた（28.5 KB）:

| | |
|---|---|
| 族 | ★ **35**（画面に出た 3 つは「入っている族」だけ） |
| ファイル項目 | ★ **150** |
| style 文字列 | 117 |
| 最長の説明文 | 80 文字 |

★ **解析ループの確保回数は約 382 回**（うち 178 回が `std::string`）。
★ **うち 203 回は vector の増殿** ──
`families_.reserve()` はあるのに、内側の `styles` / `files` に無かった。
CLAUDE.md の資源プロトコル 7 番に真正面から反する。

## 23.4 焙き16 で入れたもの

### 計器 3 点

`manifest-parsed`（`json=N bytes`、文書の値段を初めて計る）/
`manifest-families`（`families=N files=M`、文書と一覧が両方常駐する山）/
`registry-after`（文書が消えたあと）。
★ 族とファイルの数を出しているのは、
**「マニフェストが増えた」と「端末の余裕が減った」を区別するため**。

### 関门

★ `ensureHeadroom` は `WifiSelectionActivity::startWifiScan()` にあり、
そのコメントには「九つの経路がここを通る」と書いてあった。
**その文は正しくなかった** ──
`onEnter()` は最後に繋いだ網が届けば `tryAutoConnectCredential()` で戾る。
関门を `onEnter()` に移した（→ §8.8-41）。

さらに `FontDownloadActivity::onEnter()` と `OtaUpdateActivity` が
**関门の前に自分で `WiFi.mode(WIFI_STA)` を呼んでいた** ──
ステーションモードは 45,056 B の連続確保なので、
**関门を越して最も大きい確保をしていた。**
どちらも冗長（`WifiSelectionActivity` の両経路が自分で設定する）なので削除。

### 第 2 の関门（再起動しない）

解析の直前で `free >= 50,000` を見る。
根拠は★ **実測 2 例だけ** ── 52,396 で成功、45,612 で abort。
★ **`max` は両方 27,636 で同じなので、意図して見ていない。**
足りないときは `tr(STR_NETWORK_LOW_MEMORY)` を
既存のエラー画面に出す（再起動しない ── Wi-Fi がすでに上がっている）。

### reserve と 寿命

★ **382 → 249 回**（vector の確保だけ見ると **203 → 70**）。
`JsonDocument` を独立したスコープに入れ、
`refreshRegistry()` と SD の存在確認（最大 150 回の open）をその外へ出した。
★ 解析に doc が要らないことは確認済み ──
`baseUrl_` も各 `ManifestFamily` の全フィールドも複写している。

## 23.5 まだ分かっていないこと

1. ★ **`dmax = −23,552 〜 −25,088` の正体**。受信バッファ（2,065 B）でも
   グリフ（4%）でもない。**lwip か、まだ見ていない誰か。**
2. ★ **`net-join-before` の 9,380 の差の正体**。計器が無い。
3. ★ **s=4 以降が `tls-handshake` に到達しない理由**（焙き14）。
4. ★ **s=1 だけ 30 秒待つ理由** → STATUS 項目 19 として起票。
5. ★ **案B の効果** ── 一度も実行されていないので測定値がゼロ。

★ **退路（`-DCP_HTTP_KEEPALIVE=0/1/2`）は未実装。**
案B が実際に走る回を 1 回見てから作る。

---

# §24 焙き16 — 一覧は直った、本体の転送が残った

## 24.1 一覧の修正の成績（実測）

```
manifest-parsed    free=39,384  json=29187 bytes
manifest-families  free=18,280  families=35 files=150   ← 山
registry-after     free=46,524  families=35 files=150   ← doc 解放
```

| | |
|---|---|
| ★ 解析ループが食う量 | **21,104 B** |
| ★ doc を渡して戻る量 | **+28,244 B** |

★ **クラッシュは消えた。**`crash_report.txt` は存在せず、
`crash_report.prev.txt`（4,629 B）だけがある ──
★ **更新時の退避（crash_report A）が初めて動いた証拠である。**

案B も初めて走った: `s=3`（github）→ `s=4`（CDN）の **2 本だけ**。

## 24.2 計器の誤り ── `pcb` が全部ゼロだった理由

焙き16 は両端で `pcb act=0 tw=0 bound=0` を出した。
★ **シンボルの解決は正常である** ── `nm` で確認済み
（`3fc9c900 B tcp_active_pcbs` 他、定義は 1 つだけ）。

★ **場所が違っていた。**
`KeepAliveScope` のコンストラクタとデストラクタは
**スコープを挟んでいるのであって、接続を挟んでいない**:

| 計器 | そのとき接続は | `act=0` は |
|---|---|---|
| `http-keepalive-open` | ★ **まだ張っていない**（client を確保しただけ） | ★ **正しい** |
| `http-keepalive-close` | ★ **`end()` で閉じたあと** | ★ **正しい** |

★ **これは §8.8-36 の再演である**
（「賭を確保する場所でなく、**保持している場所**に計器を置く」）。

### 直した形

- ★ デストラクタで **`end()` の前にも取る**（`http-keepalive-live`）──
  **正の対照。**ここで `act>=1` が出なければ本当に計器が壊れている
- ★ `end()` の後にも取る（`http-keepalive-close`）──
  **`act` が減り `tw` が増えるか**。TIME_WAIT の仮説が前後で読める
- ★ 本体の**最初の 1 バイトが来たとき**にも取る（`dl-first-bytes`）──
  ★ **接続が確実に荷物を運んでいる唯一の瞬間**
- ★ ゼロがゼロと言う: 全部 0 のとき `(walked, all lists empty)` を付ける。
  `lsn=` も追加。**「0」と「数えられなかった」が同じに見えないように**

## 24.3 本体の転送の失敗 ── 分かっていること

```
s=3 before free=31,812 max=15,348 → after max=7,156  dmax=-8,192
s=4 before free=29,496 max=15,348 → after free=37,900 max=9,204
→ Download failed: Pretendard_10.cpfont
```

| 事実 | 根拠 |
|---|---|
| ★ **何も書かれていない** | `s=4` の record から after まで **720 ms**。2,544,893 B には短すぎる |
| ★ **大きさの問題ではない** | `G:\.fonts\UDEVGothic` に **6 ファイル・計 27.5 MB（最大 7.7 MB）が完全に入っている** |
| ★ **SD の容量ではない** | 空き **7,647 MB** |
| 0 バイトの残骸 | `PretendardMedium_10.cpfont` と `MaruBuri_10.cpfont` が **0 B** で残っている |

★ **失敗の種類はこれまで記録されていなかった。**
`DownloadError` は 4 値しか無く、
★ **3 つの別の失敗が `HTTP_ERROR` に潰れている**
（負の status / 200 でも redirect でもない status / 本文が途中で切れた）。
区別は `LOG_ERR` にはあるが★ **シリアルは使えない。**

直した形: `dl-result why=<種類> status=N got=N want=N` を
**終わり方ごとに 1 行**。種類は
`ok` / `client-failed` / `bad-status` / `bad-redirect` /
`short-body` / `sd-write-failed` / `cancelled` / `too-many-redirects`。

★ **大きさか時間か** ── **まだ分からない。**
`dl-first-bytes` が出れば本文は始まっていた、
出なければ应答の時点で断られている。**それを測る。**

## 24.4 上流 #3391 / #3392 ── 同じ問題を別の方法で直している

| | |
|---|---|
| #3391 | 2026-09-16 「Closing as solved」のみ。★ **紐づいた commit / PR は無い** |
| #3392 | ★ **こちらが本体**。#3391 はこの sub-issue。`closed as completed in #3398` |
| PR #3398 | `fix: pack font manifest catalog into one arena`、★ **`a7df86fe` 2026-09-08 merge** |

★ **我々のベース（81028d58 / 2026-08-10）には入っていない**
（`git merge-base --is-ancestor` で確認）。
**465 コミットの中にある。**

### 上流の直し方との比較

| | 上流 #3398 | 我々（焙き16） |
|---|---|---|
| 文字列 | ★ **全部 1 つの arena、`uint32_t` offset** | `std::string` のまま |
| `files` | ★ **全族を 1 つの平たな表に** | 族ごとの vector（`reserve` 済み） |
| `styles` | ★ **削除（未使用だった）** | `reserve` して残している |
| JSON | ★ `DeserializationOption::Filter` で DOM を約 1KB 縮小 | 全部読んでいる |
| `refreshRegistry()` | ★ **ダウンロードの前** | ★ **doc を捨てた後** |
| 関门 | `MIN_TLS_FREE_HEAP=40000` ＋ `MIN_TLS_MAX_ALLOC=20000`、**一覧と族の両方** | 解析の前に `free>=50000` のみ |
| 成績（報告値） | ★ **236 → 4 ブロック、max 19,444 → 32,756** | 382 → 249 回 |

★ **上流はちょうどこの問題を、我々より徒底に直している。**
我々の `reserve` は確保回数を 35% 減らしたが、
arena は★ **98% 減らしている**。

★ **しかし上流には無いものが 1 つある**:
`MIN_TLS_MAX_ALLOC` のコメントに
「its ~17KB record buffer」と書いてある ──
★ **上流は `HAVE_MAX_FRAGMENT` を知らない。**
16.5 KB の前提で max の床を 20,000 に置いている。
**我々はそれを 2,065 B にしてある（§9）。これはそのまま出せる。**

### ★ 上流の関门は今回の失敗を捕まえていた

| | 焙き16 の実測 | 上流の床 | |
|---|---|---|---|
| 族の開始時の空き | **31,812** | 40,000 | ★ **下回っている** |
| 族の開始時の max | **15,348** | 20,000 | ★ **下回っている** |

★ **上流なら「Memory Error」で始めてすらいない。**
落ちないという点では我々と同じだが、
★ **原因を当てているという証拠にはならない** ──
関门は転送を通すためのものではない。

## 24.5 まだ分かっていないこと

1. ★ **本体の転送が何で落ちているか**。`dl-result` で次の焙きに分かる
2. ★ **`dmax = −8,192 〜 −25,088` の正体**（§23 から未解決）
3. ★ **s=1 だけ 30 秒待つ理由**（STATUS 19）
4. ★ **`deleteFamily()` が 0 バイトの残骸を消していない理由**。
   残骸があるせいで registry が「1 files」と数え、`hasUpdate` が立つ
