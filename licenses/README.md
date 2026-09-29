# licenses

Y-TEC Tsumugi Drive本体は、リポジトリ／配布物ルートの`LICENSE`にある
Apache License 2.0で提供します。製品帰属と商標に関する通知はルート`NOTICE`と
`TRADEMARKS.md`を参照してください。

このフォルダーには、同梱する第三者ライセンス本文を収録します。

- `Zstandard-BSD-3-Clause.txt`
  - Zstandard `1.5.7`
  - BSD 3-Clause License
  - Copyright (c) Meta Platforms, Inc. and affiliates. All rights reserved.
- `Argon2-Apache-2.0.txt`
  - Argon2 reference implementation `20190702`
  - Apache License 2.0
  - Copyright 2015 Daniel Dinu, Dmitry Khovratovich, Jean-Philippe Aumasson, and Samuel Neves
- `LINE-Seed-JP-OFL-1.1.txt`
  - LINE Seed JP `LINESeedJP_20241105`
  - SIL Open Font License 1.1
  - Copyright 2020-2022 LY Corporation

ZstandardとArgon2はWindows版・WinPE版へ静的リンクします。フォント本体は
Windows版とWinPE版のGUI実行ファイルへ埋め込み、プロセス内限定で読み込みます。
