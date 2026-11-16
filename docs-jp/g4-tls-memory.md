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
