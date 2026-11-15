# 下書き: upstream issue #2999 への追加報告

> ★ **未投稿。**原因は確定したが、投稿前の確認（末尾）が残っている。
>
> 宛先: https://github.com/crosspoint-reader/crosspoint-reader/issues/2999
> （open。"X3 crashes when checking for updates" ── wifi 接続のステップのあとに
> クラッシュして再起動）
>
> ★ **これは「我々の環境でも起きる」という報告ではなく、切り分けの材料**である。
> #2999 の報告者は原因を書いていない。こちらは AP/station の切り分けと
> コード上の根拠まで持っているので、それを足す形にする。
>
> ★ 日本語ローカライズのフォークからの報告であることを先に書く
> （上流が再現できない差分を持っている可能性を隠さないため）。
> 日本語化と無関係であることも明示する ── ネットワークの初期化経路は
> こちらで一切触っていない。
>
> ★ **焼き10 で (A) 確保失敗に確定した。**ブラウンアウトではない
> （`Reset reason: PANIC`、電池 88%）。本文は確定後の内容に差し替えてある。

---

## 本文（英語、投稿用）

**Same symptom here. It is a heap exhaustion, not a power fault, and it only
reproduces after the device has been used for reading.**

Reporting from a Japanese-localisation fork (based on 1.5.0, a few hundred
commits behind develop). None of our changes touch the network stack, Wi-Fi
initialisation, or the web server — they are text layout, fonts and i18n — so I
believe this is the same defect rather than something we introduced. Happy to
be told otherwise. Our device is an X4; #2999 reports an X3.

### What reproduces

On the File Transfer screen, three entries:

| entry | mode | result |
|---|---|---|
| Connect to network (join an existing Wi-Fi) | **station** | **crashes** |
| Calibre wireless device | **station** | **crashes** |
| Create hotspot | **access point** | **works** |

★ **The precondition is reading a book first.** From a fresh boot the station
path works. This is very likely why the report has been hard to act on: the
operation that crashes is not the operation that causes it.

### It is an allocation failure

```
Reset reason: PANIC
Panic reason: abort() was called at PC 0x421b3345 on core 0
```

`0x421b3345` resolves, against the exact image that was running, to:

```
__cxxabiv1::__terminate(void (*)()) at libsupc++/eh_terminate.cc:45
```

With `-fno-exceptions` that is the path a failing `operator new` takes
(`__throw_bad_alloc` → `std::terminate` → `abort`). Battery was at 88% and the
reset reason is `PANIC`, so a brownout is ruled out: `esp_brownout_isr` goes
straight to `esp_restart_noos` and would have reported `BROWNOUT`.

### Where it fails

The log tail ends at:

```
[WEBACT] Starting web server...
```

That is the line immediately before `webServer.reset(new CrossPointWebServer())`
in `CrossPointWebServerActivity::startWebServer()`. `CrossPointWebServer::begin()`
logs `Creating web server on port %d` as its own first statement, and that line
never appears — so the failure is in the **construction**, before `begin()` is
entered. Constructing it asks the heap for three blocks (sizes measured against
our build):

| block | bytes |
|---|---|
| the `CrossPointWebServer` object | 280 |
| `upload.buffer` (`UploadState`, `resize(4096)` in its constructor) | 4,096 |
| `fontUpload.buffer` (`FontUploadState`, same) | 4,096 |

Note that `new CrossPointWebServer()` is a bare `new`. Under `-fno-exceptions` a
bare `new` that cannot allocate does not return `nullptr` — it aborts the
device. So the symptom is a reboot rather than an error message, even though
the code immediately below it (`if (!server)`, `if (webServer->isRunning())`)
is written as though a failure could be reported.

### The heap, measured on the device

Free heap and largest free block, written to the SD card at tagged points:

| point | free | largest free block |
|---|---|---|
| fresh boot, entering station mode | 129,472 | **114,676** |
| fresh boot, after setting station mode | 75,868 | 69,620 |
| **after reading a book**, entering station mode | 76,764 | **22,516** |
| after setting station mode | 23,240 | 20,468 |
| before `WiFi.begin` | 57,392 | 19,444 |

Free heap is comparable in both cases; the **largest contiguous block is not**.
Reading a Japanese book leaves ~114 KB of contiguous space cut down to ~22 KB,
and the station path needs roughly 45 KB more than the access point does
(Wi-Fi + DHCP + lwIP + mDNS + the web server).

What scatters it, in our build, is long-lived and small: the on-demand glyph
cache holds up to 128 separately allocated glyph bitmaps, one `new[]` per
glyph, in a font object that outlives the reader screen; and the per-style
advance tables (up to 768 entries × 8 bytes × 4 styles) are deliberately kept
across layout passes. ★ Both of those are **ours** — a stock build renders no
CJK and will scatter less. But the shape of the problem is not ours: 4 KB
requests at web-server construction are being made from whatever the heap looks
like after the device has been used, and nothing on that path checks.

### Two things that would help regardless of the cause

1. `new CrossPointWebServer()`, `new DNSServer()` and the `new WebServer(port)`
   / `new WebSocketsServer(wsPort)` inside `begin()` are all bare `new`. With
   `-fno-exceptions` each is a potential abort. Using `new (std::nothrow)` and
   reporting the failure would turn this class of crash into a message — the
   error paths are already written.
2. The two 4 KB upload buffers are allocated by the constructor and held for
   the lifetime of the server, whether or not anything is ever uploaded.
   Allocating them when an upload starts would remove 8 KB of standing
   requirement from the moment that currently fails.

Neither is a fix for the fragmentation, but both move the failure from a reboot
to something a user can report.

### Possibly related

- #4297 (open) — network uploads crash the device
- #4360 (open) — crash during sync/OTA in a network-stack assert
- #2810 (closed, fixed) — hotspot web page rebooted the device; the AP side of
  this area has had attention, the station side may not have
- #4285 (merged) — kept the web server within memory while plugins load

---

## 投稿前に確認すること

- [x] 焼き10 の実測（reset reason と heap tag）を入れる。**取る前に出さない**
- [ ] `0x421b3345` の解決を、投稿時点の上流 develop でも同じ関数になるか確認する
      （libsupc++ の番地なのでビルドごとに動く。**我々のイメージに対する解決である**
      と本文に明記してある）
- [x] 断片化の数字が日本語（グリフ128ブロック・advance table）由来である可能性を
      本文に書く ── **上流の素の状態ではもっと余っている**と明示した
- [x] #2999 の報告者の端末が X3、こちらは X4 であることを書く
- [ ] ★ 上流の現行 develop で、`new CrossPointWebServer()` と
      `UploadState` の 4096 が**まだそのまま**かを確認する
      （提案2件は現行コードに残っている問題でなければ出せない）
- [ ] ★ 「本を読んだあとだけ再現する」を、上流の素のビルドでも確認できるか
      一言添える ── こちらでしか起きないなら、再現条件としては出せない
