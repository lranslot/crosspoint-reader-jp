# layout_harness

A host (PC) harness for the two questions about Japanese text handling that
cannot be answered on the device, because the device's serial output does not
reach its user and a crash takes the evidence with it: **how much heap one text
conversion asks for in a single block**, and **where the page boundaries of a
book land**. It links the shipped converter and the shipped layout primitives —
`lib/Aozora/AozoraStrip.cpp`, `lib/Utf8/Utf8.cpp`,
`src/activities/reader/AozoraIndent.h`, `PageBudget.h` and `VerticalWrap.h` —
rather than a re-implementation of them, so a change to the firmware changes
what this measures. It exists because the same throwaway model had been rebuilt
in a scratch directory three times, and each rebuild was a chance for it to stop
describing the firmware.

What it does **not** contain is the page fill loop itself: that lives inside
`TxtReaderActivity`, behind a renderer and a font, and cannot be built on the
host. `pages` mode drives the shipped primitives with its own loop, written to
match the vertical-writing path of `TxtReaderActivity::loadPageAtOffset()`, and
deliberately leaves ruby and emphasis marks out so that every line costs the
same pitch. Its page counts are therefore **comparable to each other, not to the
device**. What it is for is the difference between two firmware revisions, which
is exactly what a controlled comparison needs.

## Build

```
build.bat                      # Windows, MSVC (what the numbers below were taken with)
cmake -S . -B build && cmake --build build     # anywhere else
```

## Use

```
layout_harness alloc <corpus-dir> [--no-reserve]
```

Replaces the global `operator new` and reports, per file and per conversion
path, the **largest single request** and the **peak bytes outstanding** for one
whole conversion, plus the largest chunk fed to and produced by the strip —
which is what the reserves have to cover. The largest single request is the
number that decides whether a conversion survives a fragmented heap: a 16 KB
request fails while 40 KB is still free. `--no-reserve` drops the up-front
reserves, which is the shape the firmware had up to 1.5.0-dev-main-38af4c6e, so
one binary can measure both sides of that change.

The converted bytes are written beside each input as `<name>.txt.converted`, so
a change that was meant to cost nothing can be checked with `cmp`.

★ The absolute figures are MSVC's, not the device's: MSVC grows a `std::string`
by half and libstdc++ doubles it, and MSVC applies that rule to `reserve()` too.
What transfers is the comparison, and the fact that a reserve asked for **once,
with a constant** requests the same size on both.

```
layout_harness pages <corpus-dir> [--no-carry]
```

Lays out every file at three vertical line spacings and prints one line per
(file, setting) with the page count and a hash of every boundary, then the
boundaries themselves. `--no-carry` turns off the indent carried across a page
boundary, which is the control for that change: a book with no indent notation
must produce identical output either way.

`baseline/pages-summary.txt` holds the summary lines for the revision named in
its header. To compare:

```
layout_harness pages <corpus-dir> | findstr "pages=" > now.txt
diff baseline/pages-summary.txt now.txt    # header lines differ; data should not
```

## Corpus

The corpus is **not** committed: two of the three books are large, and the point
of the tool is to run against whatever is on the card. Point it at a directory
of `.txt` files; encoding is detected per file the way `Txt::prepareEncoding()`
detects it. `G:\book` on the development card holds the five files the reported
numbers were taken from.
