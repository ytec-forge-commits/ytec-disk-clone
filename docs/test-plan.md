# Y-TEC Tsumugi Drive 1.0.0 テスト計画

更新日: 2026-08-04

基準: `DiskClone_Development_Spec.md` v2.0

過去のPhase、旧形式、旧製品経路の試験は、再利用する安全部品の履歴証跡として
保持する。この計画の合格に数えるのは、v2の直接製品経路へ接続した後に再実行した
試験だけである。

## 1. 共通完了条件

- コピー元Readerと対象Writerが型・所有権・ハンドルで分離されている。
- 破壊操作が`OperationPlan → 再識別 → OK → 実行 → 検証 → Result`を通る。
- 対象差替え、接続し直し、容量・セクター・レイアウト変更を実行前に拒否する。
- 全書込みをflushし、同じ対象ハンドルから読戻してから最終commitする。
- 失敗、中止、欠損、未検証を成功として表示しない。
- 製品UI／CLIが旧ジョブファイルを検索・読込・変換・変更・削除しない。
- 正式版UI／CLIが旧イメージ形式を開かない。
- 実行していない試験をPASSとして記録しない。

## 2. ホスト上の自動試験

### OperationCore

- 状態遷移、二重実行、single-use plan、取消可能／不能区間
- 大文字`OK`だけの承認、計画Hash・対象指紋の不一致
- ディスク番号交換、同型番、空シリアル、USB切断
- Checkpoint 1件制限、Snapshot失効、Source state不一致、破棄
- 例外、短いI/O、flush失敗、読戻し不一致、commit失敗

### クローン・レイアウト

- GPT／MBRの正常・破損・境界・重複・オーバーフロー
- 通常／縮小、同容量、1バイト不足、大容量余剰
- GPT→GPT、MBR→MBR、MBR→GPT、GPT→MBR拒否
- パーティション選択とWindows必須領域の解除拒否
- NTFS／exFAT／FAT32縮小、未対応FSの元サイズRAW
- 比例配分、未割当、複数区画、丸め
- RAW／既存GPT／MBR／NTFS等の対象候補と、対象外レイアウト
- USBメモリ、起動中レスキュー媒体、Dynamic Disk、Storage Spaces、RAID拒否

### VSS・Windows直接操作

- 複数Volumeを同じSnapshot setへ固定
- Snapshot device、元Volume、Geometry、役割の一意Binding
- 静的領域とSnapshot領域の重複・欠落拒否
- VSS Writer異常の既定停止とクラッシュ整合性の明示選択
- timeout、取消、`BackupComplete`、Snapshot cleanup
- cleanup前にコピー先／イメージを完成扱いしない
- Source state混在、Snapshot差替え、VSS差分領域不足

### `.tsumugi` v1

- Golden fileとv1後方互換
- ヘッダー、版、必須Flag、個数、位置、長さ、加算・乗算
- 未知暗号・圧縮、非正規順序、重複、穴、末尾追加、切詰め
- Zstandard／非圧縮／ゼロ／ファイルペイロード
- Argon2id公式Vector、AES-GCM Tag、Nonce一意性、鍵消去
- 暗号化／非暗号化、誤パスワード、弱いパスワード警告
- Source model／serial／state HashとGeometryの不一致
- NTFS／exFAT保存、FAT32／UNC／reparse拒否
- `.partial`、完全／高速検証、既存完成ファイルを保護した入替え
- 復元前の同一ハンドル完全検証、検証後差替え拒否
- 全体／個別復元、既存区画／未割当、running system拒否
- Target session再識別、範囲外、読戻し不一致、commit順序
- Fuzz、不正イメージ、メモリ／時間上限

現行の再現可能な最小基盤は`ytec-image-fuzz-tests`とする。固定seed
`0x5954454346555A5A`、合成seed 8件、変異4,096件、最大64 KiB、timeout 120秒で、
`.tsumugi`、認証対象manifest、partition snapshotをメモリ上だけで検査する。
manifestとsnapshotを受理した場合はcanonical再エンコード一致も必須とする。
通常、MSVC `/analyze /WX`、AddressSanitizerの3構成で実行する。長時間の
coverage-guided Fuzzと封印Golden corpusは、リリース候補で別途実施する。

### 起動修復

- GPT／MBR、UEFI／BIOS、Windows 10／11、複数Windows
- ESP／Active／Windows／WinREの自動検出と曖昧時停止
- BCD退避、BCDBoot `/c`、読戻し、ロールバック
- ESP／システム領域の作成可能／不可、追加確認
- 第三者EFIローダー保持／削除
- このPC／他PC判定とNVRAM変更の限定
- BCD欠損、BCDBoot失敗、WinRE欠損・複数・不明

### 救出モード

- 標準モードの最初の読取りエラー停止
- 前方、逆方向、小ブロックの有限再試行と上限
- ゼロ埋め、欠損Map、`partial_loss`結果
- 縮小・形式変換の拒否、システム救出のWindows版拒否
- 一時停止、再開、取消、欠損Map差替え

### ADK・媒体・PortableData

- 同意なし通信0件、固定Microsoft公式URL、Redirect制限
- Authenticode、版、SHA-256、必要更新、巨大／破損応答
- quiet導入、再実行、途中失敗、offline layout、削除
- 8／16GiB、4GiB FAT32＋NTFS／exFAT、既存データ保持更新
- 起動USB全体の対象候補除外とデータ領域の保存先許可
- 署名済みx64ドライバー収集と任意INFフォルダー
- EXE隣`data`、書込み不能時read-only、AppData書込み0
- ログ循環、Redaction、サポートZIP内容確認
- 手動更新確認だけの通信、起動時通信0

## 3. ビルド・解析ゲート

```powershell
. .\scripts\Enter-MsvcEnvironment.ps1
cmake --preset msvc-x64
cmake --build --preset msvc-x64 -- -j 2
ctest --preset msvc-x64 --output-on-failure
./scripts/ci.ps1
```

リリース候補ごとに次を新しく実行する。

- 通常／静的MSVC x64ビルド
- 全CTest
- MSVC `/analyze`
- AddressSanitizer
- Fuzzと不正入力Corpus
- 安全境界、ライセンス、依存Hash、SBOM、第三者通知
- 製品バイナリの依存DLL、公開引数、旧経路不在
- 配布ZIP内のMicrosoft媒体不在

## 4. VMラボ

- VMは1台ずつ使用する。
- NICを無効にし、実データを持ち込まない。
- 書込み対象は新規合成ディスクだけにする。
- ホスト物理ディスク、実USB、既存VM正本を対象にしない。
- デスクトップ到達だけでPASSにせず、ゲスト内証跡と対象単独起動を要求する。
- 複製BCD項目、空の証跡、Source／ISO依存起動はFAILとする。

### OSマトリクス

- Windows 10 22H2 x64
- Windows 11 25H2 x64

Windows 11 24H2 x64は、今回の公開前候補では未検証として互換性を主張しない。将来のVM回帰対象とする。

Windows 11 26H1はx64向け公式ADK状況により正式対応外とし、誤表示を検査する。

### 機能マトリクス

- GPT／MBR、通常／縮小、MBR→GPT
- Windows／データ専用、複数パーティション、選択
- Windows直接クローン、PE直接クローン
- `.tsumugi`作成、暗号化、全体／個別復元
- 中断再開、取消、破損、故障注入、救出欠損
- 起動修復、WinRE、コピー元とISOを外した対象単独起動
- Legacy BIOS、UEFI、Secure Boot 2011 CA／2023 CA

## 5. UIマトリクス

- 1280×720、1024×600
- 100%、125%、150%、175%、200% DPI
- 日本語長文、長いモデル名、取得不可、エラー詳細
- Tab／Shift+Tab、Enter、Space、Esc、Focus、読み上げ名
- ボタン切れ、重なり、横はみ出し、クリック不能、見えない必須操作
- 破壊確認の対象要約と大文字`OK`
- 一時停止、再開、安全な取消、取消不能区間

## 6. ADK未導入クリーンVM

1. 起動時に通信しない。
2. 媒体作成時に不足を検出する。
3. 公式URL、EULA、取得内容を表示し、同意前に通信しない。
4. 署名、版、SHA-256を検証する。
5. Deployment Tools、WinPE Add-on、必要更新をquiet導入する。
6. ISO／USBを作成・検証する。
7. 検証済み媒体更新でデータ領域を保持する。
8. offline layoutを作成・利用する。
9. 設定画面から安全に削除する。

## 7. 実機受入

非実機ゲートが全て合格した候補だけを`docs/real-hardware-acceptance-checklist.md`
で試験する。代表ケースは次を含む。

- Windows直接システムクローン
- 1TB級HDDから小容量SSDへの縮小移行
- PE直接クローン
- データ専用ディスクのイメージ作成・復元
- USB作成・起動
- 換装後チェック

未試験ハードウェアを過大に正式対応と表示しない。

## 8. 配布候補監査

- Portable ZIP、PDF、Web説明、利用規約、プライバシー、第三者通知
- 完全Hash一覧、SBOM、依存の版・License・Hash
- 未署名／Unknown Publisher／SmartScreenの説明
- 公開ZIPの再ダウンロードとローカル候補のサイズ・SHA-256一致
- Y-TEC公式ページ以外に製品ZIPを配布していないこと
- 名称調査結果と、専門家確認ではない限界

## 9. 判定

- 1件でも必須失敗、未確認、証跡不足があればリリースしない。
- 失敗後は同じ対象へ安易に再実行せず、未完了状態とログを読取り専用で保全する。
- コピー元変更が疑われる場合は関連経路をリリース停止する。
- 非物理ゲートの証跡と開発版ソースを公開GitHubへpushし、代表実機受入後に
  Y-TEC公式バイナリ候補を別途封印する。
