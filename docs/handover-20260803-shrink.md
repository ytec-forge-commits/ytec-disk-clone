# Y-TEC Tsumugi Drive 縮小移行 引継ぎ（2026-08-03）

> **履歴資料:** 1.0.0 v2再設計より前の引継ぎです。旧イメージ形式と予約ジョブに
> 関する記述は現行製品仕様ではありません。現在地は`implementation-status.md`を
> 参照してください。

## 対象と完成基準

- 正本: `D:\Y-TEC-Workspace\business-apps\ytec-disk-clone`
- 追加機能: `縮小移行モード`、Windows/データ専用ディスクの通常/縮小
  クローン・イメージ作成・復元
- 守るもの: コピー元へ一切書き込まない、物理ディスク/実USBを使わない、
  VMラボを直列利用しNICを有効にしない、既存VM/スナップショットを保持する
- 完成基準: 現行差分の全CI、データ専用縮小VM、最新ポータブルZIP監査が合格し、
  実機だけの項目を分離してcommit/pushする

## 実装済み

- UIの`通常モード（完全複製）` / `縮小移行モード（小容量へ）`
- ジョブschema v4の`transferMode=exact/shrink`とv2/v3読込み互換
- 使用量＋安全余白から小容量コピー先を計算する`MigrationCore`
- GPT/MBR、Windows/データ専用の読取り専用解析
- VSS SnapshotからMicrosoft DISMでWIMを作る`.dcmig`束
- `.dcmig`の非上書き確定、全WIM長/SHA-256、reparse/宣言外項目拒否
- コピー先GPT/MBRメタデータ再作成、FORMAT、WIM適用、読戻し
- WindowsディスクだけのBCDBoot新規BCD、データ専用では起動処理なし
- 直接縮小クローンの第三物理ディスク作業束
- オンライン縮小作成の保存先/作業先を原本と別の同一単一物理ディスクへ限定し、
  開始時、Snapshot保存直前、完成名確定前に再確認
- 復元用`.dcmig`のUNC/複数物理ディスク/reparse祖先拒否と、固定領域役割・
  宣言パーティション総量・ラベルの厳格検証
- WinPE DISM作業フォルダーを実行中システムドライブの通常フォルダーへ限定
- 通常`.dcimg`でもデータ専用ディスクの作成/復元

詳細は[`shrink-migration-mode.md`](shrink-migration-mode.md)を参照する。

## ローカル検証状態

- PowerShell VMランナー構文: PASS
- `ytec-boot-repair-tests`: PASS
- `ytec-migration-engine-tests`: PASS
- `ytec-product-data-shrink-vm`ビルド: PASS
  - SHA-256 `349E34F6E783F75AF1E6EF85CE194EDEDF615750DE8B66FA859B6A2F7DC3C7BC`
- 通常/静的CRT/ASan各43/43、静的解析、ライセンス/SBOM/安全/媒体/配布境界:
  PASS（`scripts\ci.ps1`、VM最終PASS後の再実行581.6秒）

## VMで判明した事項（失敗履歴と最終対策）

固定VM `YDC-Phase5-VSS-x64`へ、新規4 GiB RAW原本と2 GiB RAWコピー先を
SATA 4/5だけに追加した。SATA 0～3、NIC、既存スナップショットは変更していない。

1. VSS `.dcmig`作成とSnapshot削除は成功した。
2. `SetVolumeMountPointW`は新規Volume GUIDにエラー87を返したため、Volume GUIDを
   `QueryDosDeviceW`でNTデバイスへ再確認し、通知なしの一時DOSデバイスを割り当てる
   実装へ変更した。
3. FORMATの全引数引用は`/FS:NTFS`を無効扱いにしたため、空白を含まない
   `CreateProcessW`引数は引用しない回帰修正を入れた。
4. その後、フルWindows Explorerが新規未フォーマット領域を検出して確認画面を出し、
   ハーネスが待機した。WinPEにはない試験環境干渉であり、製品結果は未確定。
5. 対策後の2026-08-03 17:07試行はUAC承認後、レジストリに`NoAutoMount`値が
   存在しないWindows既定状態をStrictModeでプロパティ参照して停止した。
   `disks-before.json`では4 GiB原本と2 GiBコピー先がともにRAWで、製品ハーネス、
   VSS、コピー先書込みには到達していない。

VMランナーは原本S: fixtureを作成した後だけ`ShellHWDetection`と`mountvol /N`を
適用し、成功/失敗の両方で`mountvol /E`と元のサービス状態を復元するよう修正済み。
値が存在しない場合を自動マウント有効の既定値`0`として扱うヘルパーへ修正し、
停止適用直後と復元直後にもレジストリ値を再確認する。通常のACPI終了を5分待って
固定VMだけが`stopping`へ固着した場合は、固定VM名、UUID、実行ファイル、全
`VirtualBoxVM.exe`プロセスを厳密照合できたときだけ対象プロセスを終了し、結果を
`host-shutdown-recovery.json`へ残す。別VMまたは曖昧なプロセスがあれば何もしない。

Volume GUIDや一時DOSデバイスをFORMATへ直接渡すと「無効なドライブ指定」と
なることを実測したため、MicrosoftのMount Managerへドライブ文字を登録する前後で
Volume GUIDを再照合し、FORMAT後に所有したマウントポイントだけを解除する実装へ
変更した。2026-08-03 22:47試行は4 GiBデータ専用原本→2 GiB RAWのVSS作成・
縮小復元を完走し、原本不変、ファイル一致、Snapshot残留0件、起動処理なし、
試験環境復元をすべてPASSした。

最終証跡:
`.validation\evidence\product-data-shrink-vm\20260803-224747`

直近失敗証跡:
`.validation\evidence\product-data-shrink-vm\20260803-170747`

Explorer干渉の直近画面証跡:
`.validation\evidence\product-data-shrink-vm\20260803-125409`

VMは稼働しておらず、現在の`VMState`は`aborted`。追加VDIはSATA 4/5から
取り外して証跡として保持し、SATA 0～3、NICなしも読取り専用で再確認済み。
`aborted`を削除や異常確定せず、次の再試験は専用ランナーの検証済み復帰経路を使う。

## 最終引継ぎ状態

1. 最終VM、全CI、最新ポータブルZIP再展開監査はPASS済み。
2. 最新ZIPは
   `%LOCALAPPDATA%\YTEC\ytec-disk-clone\portable-audit\Y-TEC-Tsumugi-Drive-0.2.0-dev-shrink-final-20260803-231412.zip`。
3. ZIP SHA-256は
   `E9A4A97A7E6DE68C335ECA11673BA26FDF9C25BCE76487612D1A5C8FA77D1969`。
4. 残る受入は物理ディスク/実USBを使う代表実機項目と公開条件。

## 禁止事項

- UAC無効化、自動承認、昇格回避
- 物理ディスク、実USB、ユーザー実データでの試験
- 既存VM/スナップショット/VDIの削除、登録解除、改名
- 失敗/空証跡をPASSへ書き換えること
- 最終VMとZIP監査前のGitHub push
