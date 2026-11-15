# 下書き: upstream issue #2999 への追加報告

> ★ **未投稿。**原因が確定してから出す（焼き10 で `reset reason` と
> `/heap.txt` の 6 tag を取ったあと）。
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

---

## 本文（英語、投稿用）

**Same symptom here, with a narrower split and two distinct crash signatures.**

Reporting from a Japanese-localisation fork (based on 1.5.0, a few hundred
commits behind develop). None of our changes touch the network stack, Wi-Fi
initialisation, or the web server — they are text layout, fonts and i18n — so I
believe this is the same defect rather than something we introduced. Happy to
be told otherwise.

### What reproduces

On the File Transfer screen, three entries:

| entry | mode | result |
|---|---|---|
| Connect to network (join an existing Wi-Fi) | **station** | **crashes** |
| Calibre wireless device | **station** | **crashes** |
| Create hotspot | **access point** | **works** |

The two that crash are the two that use station mode. The access point has
never crashed for us. Both crashing entries go through
`CrossPointWebServerActivity`'s STA branch and then `WifiSelectionActivity`.

### Two signatures, not one

**(1) An allocation failure.**

```
Panic reason: abort() was called at PC 0x421b3191 on core 0
```

`0x421b3191` resolves, against the exact image that was running, to:

```
__cxxabiv1::__terminate(void (*)()) at libsupc++/eh_terminate.cc:45
```

With `-fno-exceptions` that is the path a failing `operator new` takes
(`__throw_bad_alloc` → `std::terminate` → `abort`). Another frame on the stack
is `esp_cleanup_r` at `newlib_init.c:42`, i.e. per-task newlib teardown, and the
stack also carries `0xA5A5A5A5` (freed fill) and `0xBAAD5678` (heap guard).

**(2) A reboot that never reached `panic_abort()`.**

The second signature has an **empty** panic reason. In this firmware the panic
string is written only by the `panic_abort` wrapper, so empty means the reboot
did not come through it at all — a brownout (`esp_brownout_isr` goes straight to
`esp_restart_noos`), an RTC watchdog, or a supply collapse. The log tail for
that run shows the web server had already served pages:

```
[51347] [WEB] handleClient active, server running on port 80
[58930] [WEB] Served root page
[60414] [DAV] GET /favicon.ico
[61348] [WEB] handleClient active, server running on port 80
```

So it is not a failure to start. It runs, serves, and then the device resets.

### The station-only work, from the code

The one substantial thing station mode does that the access point never does is
the scan:

- `WifiSelectionActivity` calls `WiFi.scanNetworks(true)` and then
  `networks.reserve(scanResult)` — one record per access point in range.
- The AP path calls `WiFi.softAP(...)` and never scans.

That single difference fits **both** signatures, which is why I suspect it is
one place and not two bugs:

- it allocates in proportion to how many access points are in range, and
- a full-band scan is the largest current draw the radio produces.

### Heap headroom measured on this device

Free heap on this hardware gets very low during ordinary reading, before any
networking:

| point | free | largest free block | lowest free since boot |
|---|---|---|---|
| after closing a book (vertical Japanese text) | 79,212 | 24,564 | **4,820** |

4,820 bytes is the floor `esp_get_minimum_free_heap_size()` reports. A scan that
allocates per access point, entered from that state, is a plausible place for
(1). I am not claiming it is proven.

### What I am adding to tell (1) from (2)

Rather than guess between them, the next build on this fork records:

- `esp_reset_reason()` in the crash report, above the panic reason, so
  `BROWNOUT` / `TASK_WDT` / `INT_WDT` / `SW` are distinguishable from `PANIC`
  instead of all presenting as an empty string;
- free / largest-block / minimum-free heap at six points on both paths
  (`net-sta-before`, `net-sta-mode`, `net-scan-start`, `net-join-before`, and
  `net-ap-before`, `net-ap-mode`), written to the SD card.

The decisive test is the same operation on USB power and on battery alone, from
a fresh boot both times. If it is (2), the reset reason will say `BROWNOUT` and
the heap figures will be unremarkable; if it is (1), the reset reason will say
`PANIC` and the scan sample will show the drop.

I will post the numbers here once I have them.

### Possibly related

- #4297 (open) — network uploads crash the device
- #4360 (open) — crash during sync/OTA in a network-stack assert
- #2810 (closed, fixed) — hotspot web page rebooted the device; the AP side of
  this area has had attention, the station side may not have
- #4285 (merged) — kept the web server within memory while plugins load

---

## 投稿前に確認すること

- [ ] 焼き10 の実測（reset reason と heap tag）を入れる。**取る前に出さない**
- [ ] `0x421b3191` の解決を、投稿時点の上流 develop でも同じ関数になるか確認する
      （libsupc++ の番地なのでビルドごとに動く。**我々のイメージに対する解決である**
      と明記してある）
- [ ] 4,820 B の数字が、日本語の縦書き（ルビ帯・索引）由来でないかを一言添える
      ── 上流の素の状態ではもっと余っている可能性がある。**こちらの数字だと書く**
- [ ] #2999 の報告者の端末が X3、こちらは X4 であることを書く
