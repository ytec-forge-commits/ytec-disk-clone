# third_party

承認済み依存を `dependencies.json` で管理します。

`zstd/` は公式 Zstandard v1.5.7 Release アーカイブから、静的ライブラリに
必要な `lib/` と CMake 定義、LICENSE、README、CHANGELOG だけを展開した
ソースです。CLI、DLL、テスト、辞書生成、legacy decoderはビルドしません。
本製品はBSD-3-Clause条件を選択します。
