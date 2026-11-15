# 同梱するライセンス表示

配布物（`update.bin` と手順書）に、**このディレクトリの中身をそのまま入れる。**

| ファイル | 中身 |
|---|---|
| `OFL-1.1.txt` | SIL Open Font License, Version 1.1 の本文。先頭に Yuko OTAWARA の著作権表示と Reserved Font Name が入っている（UDEV Gothic のリポジトリから取得したもの） |
| `FONTLOG.txt` | 焼き込む面 **CPGothic UI JP** の由来、改名の理由と出典、3 件の著作権表示、謝辞 |

★ **なぜ自分で用意しなければならないか。**UDEV Gothic の配布 zip
（`UDEVGothic_v2.0.0.zip`）には **TTF 25 本しか入っておらず、ライセンス文書が
1 つもない。**OFL 第 2 条は複製に著作権表示とライセンス本文を同梱することを
求めるので、再配布する側がリポジトリから取って同梱する必要がある。

## 焼き込む面の名前 ── `CPGothic UI JP`

UDEV Gothic は **Reserved Font Name** を持ち（`Copyright (c) 2022 Yuko
OTAWARA. with Reserved Font Name "UDEV Gothic"`）、OFL 第 3 条は

> 3) No Modified Version of the Font Software may use the Reserved Font Name(s)
> unless explicit written permission is granted by the corresponding Copyright
> Holder. **This restriction only applies to the primary font name as presented
> to the users.**

としている。**部分集合は Modified Version に当たる:**

> "it is not possible to preserve FE, and so pre-subsetting needs to be
> considered a Modified Version for which RFN restrictions apply."
> ── https://openfontlicense.org/webfonts-and-reserved-font-names/

★ **名前に `UDEV` を入れてはならない。**公式の手引き:

> "be creative and avoid names that reuse almost all the same letters in the
> same order or sound like the original."
> ── https://openfontlicense.org/how-to-modify-ofl-fonts/

`CPGothic UI JP` は `UDEV` の文字列を含まず、響きも似ていない。
**来歴は名前ではなく `FONTLOG.txt` と著作権表示に書く**（手引きが求めている形）。

参考: https://openfontlicense.org/ofl-reserved-font-names/

## ★ 本文用の SD フォントは改名しなくてよい

利用者が**自分で**入れるので、こちらが配布するものに当たらない。
**UDEV Gothic をその名前のまま、無改変で**推奨する ── 第 3 条が掛かるのは
「Modified Version の、利用者に見える主たるフォント名」であって、
無改変のものを名前ごと勧めることは制限されていない。

| | 改名 | 理由 |
|---|---|---|
| **焼き込む UI の面（351 字 × 6 サイズ）** | ★ **必要** | 部分集合 = Modified Version を**我々が配布する** |
| **本文用の SD フォント（6 サイズ、全 CJK）** | **不要** | 無改変で、**利用者が自分で入れる** |
