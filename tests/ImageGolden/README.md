# `.tsumugi` v1 Golden corpus

このディレクトリは、公開済みv1 Readerの後方互換性を固定する監査fixtureです。各`.hex`は空白を除去して16進decodeしたバイト列を正本とし、`CORPUS-MANIFEST.txt`はdecode後バイト列のSHA-256と長さを固定します。

17件のfixtureで、次のv1互換境界を封印しています。

- 最小MBR exact-imageのpartition snapshot、認証対象manifest、完成container
- 512-byte logical sectorのGPT exact-imageにおけるprotective MBR、primary／backup GPT metadata、認証対象manifest、完成container
- Zstandard圧縮、暗黙zero、非圧縮fallbackを同時に持つ通常exact container
- 2パーティション中1件だけを含むpartition selection
- versioned single-WIM payloadを持つshrink manifest／container
- 公開試験専用password、固定Salt、固定Nonceを使う暗号化container
- 欠損Mapと有限read evidenceを持つrescue container
- read-evidence拡張前のlegacy rescue container
- 欠損のないlossless rescue container

WIM payloadはMicrosoftファイルを含まない小さな合成バイト列です。暗号化fixtureのpassword、Salt、Nonceも公開試験値であり、実際の秘密情報や実データではありません。

`ytec-tsumugi-v1-golden-corpus-tests`は固定バイトを直接読み、台帳Hash、構造、意味論、manifest／snapshotのcanonical再encode、container全integrity、改ざん拒否を検証します。さらに全9 containerをそのまま一時`.tsumugi`へ置き、製品と同じtwo-pass stream Readerで受理します。固定containerのpayload末尾またはfooter Hashを改ざんした場合は、復元callbackが0回のまま拒否されることも固定しています。writerから実行時に再生成した値だけとの比較では後方互換証明にならないため、固定ファイル自体を変更する場合は形式変更として人間の承認と版・影響記録が必要です。

fixture生成機能は製品バイナリには含まれません。17件すべては`ytec-image-fuzz-tests`のin-memory seedにも使われ、通常実行はファイルI/Oを行いません。新しいfixtureを人間が監査して追加する場合だけ、既存fixtureを上書きしない次の明示コマンドを使用できます。

```powershell
out\build\msvc-x64\tests\ytec-image-fuzz-tests.exe `
  --audit-emit-golden-corpus tests\ImageGolden `
  I_UNDERSTAND_NEW_V1_FIXTURES_ARE_PERMANENT
```

コマンドは既存fixtureが同じ固定バイトなら監査結果だけを表示し、異なる場合は上書きせず停止します。表示されたSHA-256とdecode後の長さを別経路で再計算し、`CORPUS-MANIFEST.txt`とfixture testへ人間が明示的に反映します。このGolden corpusは形式組合せの固定試験を満たしますが、長時間coverage-guided Fuzzは別のリリースゲートとして残ります。
