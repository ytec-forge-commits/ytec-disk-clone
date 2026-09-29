# Y-TEC Tsumugi Drive v2 アーキテクチャ

更新日: 2026-08-04

対象: Y-TEC Tsumugi Drive 1.0.0

本書はv2の目標アーキテクチャを定義する。現行の予約ジョブ経路、`.dcimg`、
`.dcmig`は移行元であり、ここに記載する製品入口には含めない。過去のPhase文書と
試験証跡は、再利用するエンジン部品の根拠として保持する。

## 1. 設計原則

- Windows版とWinPE版は同じ`OperationPlan`と実行エンジンを使い、表示とSource Providerだけを変える。
- 選択、確認、実行、検証を同じセッション内で完結させる。永続予約ジョブを作らない。
- コピー元Readerとコピー先Writerを型で分離する。
- ディスク番号・ドライブ文字ではなく安定識別から実行直前の対象を解決する。
- レイアウト計算、形式解析、状態遷移は純粋または合成Backendで試験可能にする。
- Windows API、VSS、物理I/O、外部Microsoftツール、ネットワークを外縁Adapterへ隔離する。
- 未完了処理は1件の`OperationCheckpoint`として扱い、ジョブ一覧・予約・自動実行へ拡張しない。

## 2. コンポーネント

### 2.1 再利用・拡張する既存部品

- `DiskModel`: 読取り専用列挙、システムディスク判定、安定識別、物理Reader／Writer
- `CloneCore`: GPT／MBR解析・再生成、I/O進捗、取消、読戻し検証、最終commit
- `MigrationCore`: 通常／縮小のレイアウト計算、Windows／データ専用の役割判定
- `MigrationEngine`: ファイル単位移行、FORMAT／適用、Windowsだけの起動最終化
- `VssRequester`: VSS Workflow、Writer監査、Snapshot Reader／Bitmap、cleanup
- `BootRepair`: Microsoft署名検証、BCDBoot新規BCDトランザクション、MBR2GPT／REAgentC境界
- `MediaBuilder`: ADK／WinPE検出、署名・版・更新ゲート、ISO／USB作成
- `UiSupport`: LINE Seed JP、DPI、共通UI、進捗表示

### 2.2 v2で置換・追加する部品

- `OperationCore`: `OperationPlan`、`TargetAuthorization`、状態機械、`OperationResult`
- `OperationCheckpoint`: 1件限定の中断保存、同一性再検証、再開／破棄
- `ImageFormat`: `.tsumugi` v1 Reader／Writer、暗号、圧縮、完全検証
- `CryptoProvider`: Argon2id KDF、Windows CNG AES-256-GCM、鍵消去
- `RescueEngine`: 有限再試行、逆方向／小ブロック、欠損マップ
- `BootDiscovery`: ディスク選択からWindows／ESP／Active／WinRE／EFIローダーを自動検出
- `AdkAcquisition`: 固定公式URL取得、署名／版／Hash検証、quiet導入、offline layout
- `DeviceHealth`: SMART／NVMe状態、温度、装置閾値、AC／バッテリー
- `PortableData`: EXE隣`data`の設定、ログ循環、サポートZIP、手動更新確認

### 2.3 製品入口

- `WindowsApp`: 常時elevated、Windows直接クローン、オンラインイメージ作成、データディスク復元、起動修復、媒体作成
- `WinPEApp`: 直接クローン、イメージ作成／復元、救出、起動修復、診断
- `CliTools`: 読取り専用診断と開発試験だけ。正式版に`--job-*`を持たない

`WindowsApp`と`WinPEApp`は予約ジョブの作成、検索、読込、実行、結果取込みを
行わない。旧ジョブファイルはファイル列挙対象にも含めない。

## 3. 依存方向

```text
WindowsApp / WinPEApp
          |
          v
     OperationCore  <---- OperationCheckpoint
      /    |    \
     v     v     v
CloneCore ImageFormat BootRepair/BootDiscovery
    |       |          |
    v       v          v
DiskModel CryptoProvider MicrosoftToolAdapter
    ^
    |
VssSourceProvider / OfflineSourceProvider / RescueSourceProvider

MediaBuilder -> AdkAcquisition -> verified Microsoft local installation
PortableData -> manual UpdateCheckAdapter -> fixed Y-TEC HTTPS endpoint
```

- 内側のレイヤーはGUI、ディスク番号、ドライブ文字、WinHTTP、PowerShellを知らない。
- 外側Adapterは検証済み不変値だけを内側へ渡す。
- `ImageFormat`はネットワーク、GUI、物理ディスクを知らない。
- `CryptoProvider`はパスワード文字列を永続オブジェクトへ保持しない。

## 4. 公開内部契約

名称は実装時に既存の命名規約へ合わせられるが、責務は次のとおり固定する。

### 4.1 `OperationPlan`

```text
OperationPlan
- operation_id             # 実行セッション内ID。予約IDではない
- kind                     # clone / image_create / image_restore / rescue / boot_repair / media
- mode                     # exact / shrink
- source_identity
- target_identity[]
- source_snapshot_policy   # offline / VSS strict / VSS crash-consistent
- partition_selection[]
- layout_plan
- boot_conversion          # preserve / mbr_to_gpt
- filesystem_policy
- verification_policy
- completion_action
- risk_flags[]
- plan_hash
```

- UI入力を正規化した後は不変とする。
- `plan_hash`は確認画面、対象再識別、中断再開の結合に使う。
- シリアル完全値、パスワード、BitLocker回復キーを含めない。

### 4.2 `TargetAuthorization`

```text
TargetAuthorization
- plan_hash
- displayed_target_fingerprint
- acknowledged_at_utc
- confirmation == "OK"
```

同一実行プロセス内だけで有効とし、ディスク再接続、計画変更、アプリ再起動で
無効にする。中断再開時も再度対象要約と`OK`を要求する。

### 4.3 `OperationCheckpoint`

```text
OperationCheckpoint v1
- operation_kind
- plan_hash
- source_fingerprint
- target_fingerprint
- snapshot_identity_or_offline_epoch
- output_file_identity
- verified_extents_or_chunks
- checkpoint_sequence
- checksum
```

- 認識するのは最大1件である。
- `data`、RAM、対象、レスキューUSBのうちコピー元ではない安全な場所だけに置く。
- パスワード、派生鍵、BitLocker資格情報、平文マニフェストを保存しない。
- 再開時は同一性と記録済み範囲を再検証する。

### 4.4 `OperationResult`

```text
OperationResult
- status                   # verified / partial_loss / cancelled / failed
- copied_bytes
- written_bytes
- verified_bytes
- snapshot_created_at_utc
- layout_result
- boot_result
- bad_ranges[]
- warnings[]
- next_action
- diagnostic_code
```

Windowsシステムクローンは`verified`でも表示名を「検証完了・換装待ち」とし、
実際の起動成功を意味しない。救出欠損時は必ず`partial_loss`とする。

## 5. 実行状態機械

```text
Idle
 -> Planning
 -> Preflight
 -> AwaitingOK
 -> OpeningSource
 -> Revalidating
 -> InvalidatingTarget
 -> Transferring <-> Paused
 -> Verifying
 -> Finalizing
 -> Verified

Any pre-commit state -> Cancelling -> Cancelled
Any state            -> Failing    -> Failed
Rescue with bad map  -> PartialLoss
```

- `AwaitingOK`より前に対象を変更しない。
- `InvalidatingTarget`後は完成状態へ戻す唯一の経路を`Finalizing`に限定する。
- `Paused`中もWindows VSSの安全余裕を監視する。
- `Finalizing`では取消を無効にし、UIへ理由を表示する。
- 例外を成功状態へ変換しない。

## 6. Source Provider

### 6.1 `VssSourceProvider`

- Windowsシステム／オンラインボリュームを同一Snapshot setへ追加する。
- Snapshotデバイス、元Volume、Geometry、パーティション役割を固定Bindingにする。
- Bitmapとデータを同じSnapshotから取得する。
- Snapshot対象外のESP／回復等は、読取り専用物理Readerと一意に対応できる場合だけ混在させる。
- Writer異常は既定で失敗し、クラッシュ整合性は明示したPlanだけを許可する。
- `BackupComplete`とSnapshot削除を完了条件に含める。

### 6.2 `OfflineSourceProvider`

- WinPEでコピー元物理ディスクをread-onlyへ設定し、再列挙後に同じ対象であることを確認する。
- ボリューム自動マウントやドライブ文字割当を必要最小限にし、コピー元へメタデータを書かない。
- Windows／データ専用の両方を扱う。

### 6.3 `RescueSourceProvider`

- 通常Readerの失敗を隠さず、救出モード専用に差し替える。
- 前方、逆方向、小ブロックの有限戦略を明示する。
- 読めない範囲をゼロデータと欠損マップの組で返す。
- システムディスクではWinPEからだけ生成可能にする。

## 7. クローンパイプライン

### 7.1 共通

1. 読取り専用列挙とパーティション役割判定
2. 通常／縮小の推奨とパーティション選択
3. 必須システム領域の強制選択
4. コピー先レイアウトと余剰配分
5. 変換・起動計画
6. 対象要約と`OK`
7. Source Provider開始と全対象再識別
8. コピー先offline・既存識別無効化
9. データ移行と各書込み読戻し
10. パーティション表最終確定
11. BCD／WinRE／MBR→GPT最終化
12. 全体検証と`OperationResult`

### 7.2 通常モード

- 同じ論理セクターサイズを要求する。
- NTFSは使用クラスタ、FAT／OEM／未対応FSは方針に応じて全領域、MSRは定義だけを扱う。
- コピー先GUID／Partition GUID／MBR署名を再生成する。
- 同容量未満を必要とする場合は縮小モードへ切り替える。

### 7.3 縮小移行モード

- NTFS、exFAT、FAT32の使用量と安全余白から最小容量を計算する。
- 対応外FSは元サイズのRAW領域を確保する。
- 元パーティションを縮小・変更しない。
- ファイル単位ペイロードを一時`.dcmig`束ではなく`.tsumugi`内部へ格納する。
- exFAT／FAT32は保持を既定とし、データ領域だけ詳細設定でNTFSへ変換できる。

### 7.4 MBR→GPT

- コピー元を変更せず、コピー先へGPT、ESP、MSR、Windows、回復、データを再構成する。
- 対応Windowsとレイアウトを事前診断する。
- Microsoft署名済み標準ツールを使う場合も、対象・版・署名・固定引数を検証する。
- Windows版とWinPE版の両方で同じ実行契約を使う。
- 変換失敗を通常クローン成功へ格上げしない。

## 8. `.tsumugi` v1

### 8.1 Reader／Writer

- `TsumugiWriter`は新規`.partial`、チャンク圧縮、任意暗号化、Hash／Tag、索引、footer、flush、検証、確定を担当する。
- `TsumugiReader`は同一ハンドル上の有界解析、認証、完全検証を担当する。
- `PreparedTsumugiRestore`は完全検証済みReaderを単回使用で封入し、別ハンドルへのすり替えを防ぐ。
- 復元先Writerへ渡す前にパーティション表と全論理範囲を検証する。

### 8.2 暗号

- `CryptoProvider`はArgon2id 20190702とWindows CNG AES-256-GCMだけを実装する。
- KDFパラメーターは形式へ保存するが、v1の安全上限・下限をReaderが検査する。
- Salt、Nonce、Tag、鍵長を固定境界で検査する。
- チャンク単位の認証後だけ平文を下流へ渡す。
- 鍵をキャッシュ、中断保存、ログ出力しない。

### 8.3 検証モード

- 「高速」は各書込み即時読戻しと最終メタデータ検証を行い、完成後の追加全走査だけを省く。
- 「完全」は完成ファイルを先頭から再走査し、全チャンクと全体を検証する。
- 復元時は常に完全検証し、利用者に省略設定を提供しない。

## 9. 起動修復

`BootDiscovery`はディスクだけを受け取り、以下を候補と根拠付きで返す。

- Windows 10／11 x64インストール
- GPT／MBRとUEFI／BIOS
- ESP／Active領域
- BCD、Windows Boot Manager、WinRE
- 第三者EFIローダー
- ESP／システム領域新設に使える縮小可能NTFS

`BootRepairPlan`は対象ファイル、区画、NVRAM、退避先、部分修復条件を列挙する。
実行は`OK`後に対象を再識別し、署名済みMicrosoftツールを固定引数で呼ぶ。

BCD新規再構築は既存実装の次のトランザクションを維持する。

1. BCDBoot署名確認
2. 既存BCDを非上書き退避
3. 署名再確認
4. `BCDBoot /c`
5. 新規BCD確認
6. 成功時退避削除、失敗時部分BCD除去と旧BCD復元

## 10. レスキューメディア

### 10.1 `AdkAcquisition`

- アプリリリース内の許可マニフェストから公式URL、版、SHA-256、署名者を読む。
- 利用者同意後だけ一時ダウンロードする。
- Authenticode、版、Hashの全一致後だけMicrosoft対応quietセットアップを起動する。
- Deployment Tools、WinPE Add-on、Servicing Updateを順序付きで確認する。
- 成功後は一時取得物を削除し、導入済み構成を設定画面から安全にアンインストールできるよう記録する。
- `/layout`作成時は利用者指定フォルダーを出力所有先とし、勝手に削除しない。

### 10.2 `MediaBuilder`

- 検証済みローカルADKだけを使用し、元WIMを変更しない。
- 自作Windows／WinPEアプリ、設定、第三者通知、日本語フォントサポート、承認済みドライバーだけを追加する。
- USB初回作成は4GiB FAT32起動＋残りNTFS／exFATデータとする。
- 検証済み既存媒体は起動／アプリ領域だけ更新し、データ領域を保持する。
- 起動中USB全体を他の書込み候補から除外する。

## 11. 保存・通信

- 設定、ログ、中断チェックポイントはEXE隣`data`を基準とする。
- `data`が書込み不能ならread-only診断へ縮退し、AppDataへ移さない。
- 実行中の保存先がコピー元物理ディスクなら、RAM、対象、イメージ保存先、レスキューUSBへ明示的に切り替える。
- Windows版の同一物理ディスク保存例外は、イメージ対象外の別パーティション、詳細設定、毎回の警告が揃う場合だけ許可する。PEとディスク全体イメージでは許可しない。
- 通信Adapterは`AdkAcquisition`と手動`UpdateCheckAdapter`だけにリンクする。
- 更新確認は固定Y-TEC HTTPS URL、固定サイズ、固定JSONスキーマを有界解析し、表示だけを行う。
- テレメトリ、起動時通信、自動更新、クラウド同期を持たない。

## 12. 移行手順

1. v2仕様、安全モデル、依存台帳、AGENTS限定通信例外を確定する。
2. `OperationCore`と1件の`OperationCheckpoint`を合成Backendで作る。
3. Windows／WinPE UIを直接実行フローへ切り替える。
4. 予約ジョブ作成・検索・実行・結果取込みと公開`--job-*`を到達不能化して削除する。
5. `.tsumugi` v1、暗号、通常／縮小ペイロードを実装する。
6. `.dcimg`／`.dcmig`公開入口を削除し、移行中試験が不要になった段階で旧形式製品コードを削除する。
7. BootDiscovery、救出、ADK取得、媒体更新を接続する。
8. v2のVM回帰を新規に実施する。旧ジョブ経路PASSを製品合格へ流用しない。

旧ジョブファイルの削除・変換・整理は、この移行手順に含めない。
