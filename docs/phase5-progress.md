# Phase 5 Windows VSSイメージ作成進捗（2026-08-01）

> **履歴資料:** v2再設計前の旧イメージ形式／旧製品経路の証跡です。1.0.0の
> 現行形式は`.tsumugi` v1で、以下を製品操作として案内しません。

## 実装済みの安全な基盤

- 仕様書の標準フローに沿うVSS Requesterワークフロー境界
- 管理者権限、1～128個の正規Volume GUIDパス、NTFS限定、重複拒否の開始前検証
- 初期化、バックアップ状態、Writer metadata、Snapshot set、対象追加、Prepare、Snapshot、Writer確認、Snapshotデバイス対応、データコピー、BackupComplete、削除の固定順序
- Writer名欠落、Snapshot直後のStable/BackupComplete待ち以外、`S_OK`以外、Writer結果0件を無視せず停止する境界
- 各対象Volume GUIDと一意な`GLOBALROOT` Snapshotデバイスの対応検証
- Snapshot set作成後の全失敗経路で削除を必ず試行し、削除失敗も成功扱いしない境界
- 実VSSを使わない注入バックエンドによる成功順序、権限不足、パス不正、重複、Writer異常、対応不整合、各段階失敗、削除失敗のモックテスト
- Windows SDK標準の`IVssBackupComponents`、`IVssAsync`、`VssApi.lib`だけを使うWindows COMバックエンド
- `InitializeForBackup`、`VSS_CTX_BACKUP`、非Component選択のFull/Bootable System State、Writer metadata監査、Snapshot set、Writer状態、`BackupComplete`、厳密なSnapshot set削除の実装
- COMの致命的例外を隠さない`IGlobalOptions`設定と、ローカルVSS限定の`RPC_C_AUTHN_LEVEL_PKT_PRIVACY`／`RPC_C_IMP_LEVEL_IDENTIFY`プロセスセキュリティ
- `IVssAsync::Wait`を短い区間で反復し、`QueryStatus`のHRESULTを保持する有限timeout、明示キャンセル、timeout時`Cancel`の実装
- Writer名、Instance/Writer GUID、状態、HRESULTの監査ログと、metadata/Writer status/BSTR/Snapshot属性/COM interfaceのRAII解放
- 早期失敗時の`AbortBackup`、作成したSnapshot setだけを対象とする`DeleteSnapshots`、デストラクタのbest-effort再Cleanup
- `GetSnapshotProperties`が返したSnapshot set ID、Snapshot ID、元Volume GUID、`GLOBALROOT\Device\HarddiskVolumeShadowCopyN`形式の再検証
- コピー処理へ元Volume GUIDを渡さず、検証済みSnapshotデバイスパス配列だけを渡す`SnapshotCopyCallback`
- `Pending→完了`、明示キャンセル、timeout、状態照会失敗、VSS操作失敗、無限待機拒否、コピーCallback未設定を実VSSなしで確認するモックテスト
- VSS Snapshotパスだけを受け付け、`GENERIC_READ`で開いた後に容量と論理セクターを再確認し、セクター整列した有界読取りだけを行う`WindowsSnapshotVolumeReader`
- Snapshotデバイスが`IOCTL_STORAGE_QUERY_PROPERTY`を未サポートと明示した場合だけ、`GetDiskFreeSpaceW`でファイルシステムのbytes-per-sectorを再確認する限定fallback
- Snapshot専用Binding型を持つ`WindowsSnapshotVolumeBitmapProvider`。通常のVolume GUID用Providerと相互のパス種別を拒否し、同一パーティションへの重複Bindingも拒否
- Snapshot ReaderのライブVolume拒否、不正Geometry拒否、Backend到達回数とIdentity固定、Snapshot/ライブBitmap Provider混在拒否を実デバイスなしで確認するモックテスト
- Snapshot Bitmapの使用範囲を16/32MiB以下へ分割し、Snapshotローカル位置からコピー元ディスク論理位置へ変換する`write_vss_snapshot_dcimg_v1`
- Volume/Reader/NTFS geometry、一意なパーティションBinding、Bitmapの整列・昇順・範囲を全件検査してからだけ、Zstandard/非圧縮`.dcimg`抽象ステージングを開始する境界
- 全索引、全非ゼロチャンク、独立ハッシュ表、フッター、全体SHA-256の再読込み成功後だけcommitし、Snapshot消失や再読込み破損を含む失敗時は未完了abortする境界
- Snapshot使用範囲の変換・分割、範囲外、Geometry不一致、Bitmap失敗、Snapshot消失、重複Bindingを実VSSなしで確認する6件のモックテスト
- Windows保存先を物理ディスクへ対応付け、コピー元/指定コピー先との安定識別分離、空き容量、ローカル絶対`.dcimg`、reparse不使用、既存ファイル非上書きを開始時と確定前に検査する具体Backend
- 保護DACL付き`CREATE_NEW`の`.partial`へ全長予約・write-through・読戻し・flushし、検証成功後だけ非上書きで完成名へ確定する処理
- コピー元/コピー先の一意再識別、同一ディスク、容量不足、reparse、既存完成/未完了ファイル、確定前の識別差替え、作成/確定/破棄失敗を実ファイルなしで確認する17件のモックテスト
- 既定OFFのVM専用`ytec-phase5-vss-live-vm`検証ハーネス。固定許可語、管理者権限、VirtualBox BIOS、固定C: NTFS、固定合成Sentinelを要求し、Snapshot内Sentinel、raw boot sector、NTFS geometry、Snapshot bitmap、Writer、`BackupComplete`、Cleanupを一連で検証する
- 専用Windows 10 x64 VMでのライブVSS統合試験。Snapshot内Sentinel、raw boot sector、容量/論理セクター、2,889個の使用範囲、Writer 10件、`BackupComplete`、Snapshot削除を一連で確認
- 既定OFFのVM専用`ytec-phase5-file-staging-vm`検証ハーネス。VirtualBox、管理者、固定許可語、システムC:、128MiB RAW識別ディスク、512MiB NTFS保存先VDIを再確認し、合成ReaderからWindows実ファイルBackendへだけ接続する
- VM内で1MiBの合成データを含む1,049,741バイトの`.dcimg`を生成し、再読込み、全ハッシュ、非上書き確定、`.partial`残留なし、ホスト側コピーとのSHA-256一致を確認
- 再識別済み読取り専用物理ディスクからMBR/GPT表と全区画を再解析し、
  正規バックアップマニフェストと最小パーティション表スナップショットを
  生成する製品メタデータ境界
- GPTのEFI/回復は読取り専用raw、Windows NTFSはVSS、MSRは表から再作成し、
  MBRのWindows NTFSはVSS、FAT32/回復はrawに振り分ける混在計画
- VSSコピー中は`.partial`の読戻し検証完了まで進めても確定を遅延し、
  `BackupComplete`とSnapshot削除の成功後だけ完成名へ確定する一体処理
- 標準権限、非システムディスク、識別差替えを物理ディスク/VSS到達前に拒否し、
  再識別、メタデータ、Volume対応、実行を一つにした製品サービス
- Windows GUIの「イメージを作成」から製品サービスをバックグラウンドで
  呼ぶ確認/実行/結果接続。標準権限ではボタン無効、自動UACなし
- NTFSファイルシステム領域が包含パーティションより小さい正常構成を許可し、
  Bitmap範囲はNTFS geometry内に限定してパーティション末尾余白を読まない境界
- `DoSnapshotSet`成功後の失敗／キャンセルでは、作成したSnapshot setを厳密に
  削除してから`AbortBackup`でWriterへ中断通知する終了順序
- 製品VSSサービスを使う固定VM試験。容量不足、コピー中キャンセル、正常完了を
  同じWindows 10 x64 VMで実行し、全経路でShadow Copy残留0、`.partial`残留なしを確認
- 製品クローン／復元のキャンセル専用WinPE試験。唯一のRAW合成コピー先を
  ディスク番号に依存せず選び、読戻し後のキャンセルでもパーティション表を
  未コミットのまま保持することをUEFI/Secure BootとLegacy BIOSで確認
- 破損dcimgと改ざん復元ジョブを製品WinPE経路へ渡し、8MiB合成復元先の
  先頭／末尾4KiB、オンラインRAW状態、パーティションなしが不変であることを確認
- DISMのWIMマウントが通常ファイルへ付けるWindows SDK定義の
  `IO_REPARSE_TAG_WIM (0x80000008)`だけを許可し、抽出したUEFIブート
  マネージャーのSHA-256、AMD64 EFI形式、Microsoft署名を再検証する媒体境界
- 2026-08-01午前時点の製品差分を収録した2011/2023 CA製品ISOを
  リポジトリ外へ生成し、HDD/NICなしVMでLegacy BIOS、UEFI、
  Secure Boot有効/無効の6条件を直列起動して元構成へ戻す試験。
  その後に追加したLINE Seed JP埋込みとライセンス同梱は、次のISO再生成対象

## 2026-08-03 製品最終VM回帰

- 製品VSS経路で作成したZstandard `.dcimg`を、別の合成GPTディスクへ
  製品WinPE経路で復元した
- 復元後にコピー元とISOを外し、対象だけでUEFI64/Secure Boot起動した
- 保護イメージのSHA-256は試験前後とも
  `E1938A4C10C10149F15C707114B301267E79FD8A2E1BE7C336CE39E6B0CCD49A`
- 通常シャットダウン、Worker設定復元、NICなし、物理ディスク/USB不使用を確認した
- 確定証跡: `.validation/evidence/product-vss-restore-vm/20260803-033054/`

## 現在残る範囲

- 中断済み`.partial`やジョブを再開する機能（現状は再開せず安全に中止／破棄する）
- 管理者GUIを人が操作する実測進捗／キャンセル
- 代表実機でのVSS生成、別ディスク復元、起動、性能受入

通常製品へワークフロー、Windows COMバックエンド、読取り専用物理ディスク、
実ファイルBackendを接続し、管理者権限を使う製品一体経路を固定VMで実行済みです。
VM証跡は合成VDIだけを対象とします。VSS生成物から別ディスクへの完全復元と
起動は確認済みですが、管理者GUIの人手操作と実機は未検証です。

## 今回の検証

- MSVC x64 C++20、`/W4 /WX /permissive- /sdl /guard:cf`: PASS
- `ytec-vss-workflow-tests`: 19件 PASS
- `ytec-dcimg-stream-tests`: 8件 PASS
- `ytec-windows-file-staging-tests`: 17件 PASS
- `ytec-vss-snapshot-image-tests`: 6件 PASS
- `ytec-vss-snapshot-plan-tests`: メタデータ生成、BitLocker拒否、
  GPT/MBR混在計画を含む7件 PASS
- `ytec-online-backup-tests`: VSS完了前非確定、BackupComplete/削除失敗abortを
  含む4件 PASS
- `ytec-windows-online-backup-job-tests`: 標準権限、非システム、
  再識別差替え、製品経路成功の4件 PASS
- `ytec-phase1-synthetic-tests`: Snapshot/ライブBitmap型分離を含む11件 PASS
- MSVC `/analyze`（VSSバックエンドと同テスト）: 警告・エラーなし
- `msvc-x64-vm-destructive`でVM専用ライブVSSハーネスの静的ランタイムビルド: PASS
- VM専用ライブVSSハーネスを含むMSVC `/analyze`: 警告・エラーなし
- Windows SDK 10.0.26100の`RegOpenKeyExW` SAL注釈が出すC6553だけを、VirtualBox判定を持つVM専用ハーネスターゲットに限って`/wd6553`で抑制。通常製品、ライブラリ、他テストの警告設定は変更なし
- 全CI: 通常、静的CRT、MSVC `/analyze`、ASanの各構成でCTest 39/39 PASS。
  ライセンス、安全境界、SBOM、WinPE媒体境界、ポータブル配布境界もPASS
- 外部ライブラリ追加: なし。`VssApi`、`Ole32`、`OleAut32`はWindows SDK／Windows標準ライブラリであり、再配布ファイルを追加していない。UI資産として利用者承認済みのLINE Seed JP `LINESeedJP_20241105` Regular/Bold（OFL-1.1）をWindows版/WinPE版GUIへ埋め込んだ
- ライブVSS専用VM: PASS。EFI64、NIC全無効、C: NTFS、Harness SHA-256 `2D39573258E8BA032D6FAEDB8C4BCC0805F63DF01070821CF5ABD3AA7B6BE3E3`
- VSS Writer: 10件確認。Snapshot直後の`VSS_WS_WAITING_FOR_BACKUP_COMPLETE`を正常遷移として扱い、`BackupComplete`後はStableを確認
- Snapshot Reader/Bitmap: Sentinel、raw boot sector、容量/論理セクター、使用範囲2,889件・17,525,841,920バイトをSnapshotから読取り
- Shadow Copy: 実行前0件、実行後0件。確定証跡は`.validation/evidence/phase5-vss-vm/20260730-231804/`
- Windows実ファイルBackend専用VM: PASS。固定128MiB RAW識別ディスク、固定512MiB NTFS保存先、1,049,741バイト、保存データ1,048,576バイト、1チャンク、`.partial`残留なし
- 完成`.dcimg` SHA-256: ゲスト/ホストとも`73F25A9816C4CABE212194DC43D655E9C7FE9980D89E4A3BAE936EF7B39F333D`。確定証跡は`.validation/evidence/phase5-file-staging-vm/20260731-004413/`
- 製品VSS一体VM: 容量不足、キャンセル、正常完了の3経路すべてPASS。
  Writer 10件、全経路でShadow Copy 0件、`.partial`残留なし、既存テスト専用出力の
  厳密な再生成も確認。完成`.dcimg`は17,865,810,051バイト、SHA-256
  `7F6822BE35A0E268986C50FD945DB062C80E866A78CAD284549D1E5CBF9D843B`。
  確定証跡は`.validation/evidence/product-vss-backup-vm/20260801-015159-resume/`
- 製品クローンキャンセルVM（EFI64、Secure Boot有効）: PASS。
  `verifiedBeforeCancel=668758016`、コピー先はoffline／RAW、GPT未コミット。
  確定証跡は`.validation/evidence/product-job-cancellation-vm/20260801-094208-clone/`
- 製品復元キャンセルVM（Legacy BIOS）: PASS。
  `verifiedBeforeCancel=19780`、コピー先はoffline／RAW、MBR未コミット。
  確定証跡は`.validation/evidence/product-job-cancellation-vm/20260801-094452-restore/`
- 破損dcimg／改ざんジョブの製品復元VM: 各PASS。コピー先はonline／RAW、
  先頭／末尾4KiB不変、パーティション表未コミット。確定証跡は
  `.validation/evidence/product-restore-failure-vm/20260801-095318-restore-corrupt-image/`と
  `.validation/evidence/product-restore-failure-vm/20260801-095605-restore-tampered-job/`
- 関連回帰: 通常7/7、ASan 7/7 PASS。VM専用ハーネスの通常ビルドと
  MSVC `/analyze`もPASS
- LINE Seed JP埋込み直前の製品WinPE ISO: 2011 CA版429,942,784バイト、SHA-256
  `B0B82226323A7F55F82DC35C677184F68533A968B9F53E932BB60FD3A381F19A`、
  2023 CA版429,942,784バイト、SHA-256
  `05302CF7B2F9B7C3E398C7A95B4D8C35D4500D6F3F742C8149F3E93163BD350E`。
  manifest、現在の製品CLI/GUI、WIMマウント残留0、リポジトリ内Microsoft媒体0を照合
- 同ISOの製品WinPE起動マトリクス: Legacy BIOS/UEFI、Secure Boot有効/無効、
  2011/2023 CAの6/6で同一の日本語製品GUIを確認。HDD 0、NICなし、物理USB/
  ディスク不使用、試験後VM poweroff、元UEFI64/Secure Boot有効/元ISOへ復元。
  確定証跡は`.validation/evidence/winpe-product-boot-matrix/20260801-102638/`
- 実機: 利用者方針により全機能実装後まで未実施

## 次の安全ゲート

1. 代表実機で長時間処理の進捗、速度、残り時間、キャンセルを確認する。
2. 実USB書込みと代表機のBIOS/UEFI/Secure Boot起動を確認する。
3. 実機試験専用チェックリストに従い、重要でない合成データだけで受け入れる。
