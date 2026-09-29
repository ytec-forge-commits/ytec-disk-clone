# Phase 1開発進捗（2026-07-30）

> **履歴資料:** v2再設計前のPhase報告です。予約ジョブに関する記述は廃止済みで、
> 現行製品仕様ではありません。

## 今回実装した範囲

- 読取り専用コピー元と書込み可能コピー先を型で分離したブロックデバイス境界
- ディスク番号に依存しない安定識別と二段階確認
- CRC32、厳密なGPT解析、GUID再生成、拡大先GPT書込み計画
- EFI FAT32、MSR、基本NTFS、Windows回復NTFSに限定したクローン計画
- `FSCTL_GET_VOLUME_BITMAP`読取り専用プロバイダーとビットマップ復号
- 全チャンクの読戻し比較、主GPTヘッダーの最終確定
- BitLockerシグネチャ、不明パーティション、4Kn書込みの明示停止
- 信頼済みSystem32、埋込み署名またはWindowsカタログ署名、Microsoft署名者を検証するBCDBoot実行境界
- 合成ディスク、モックBCDBoot、失敗経路を含む自動テスト
- 実行中Windowsの所属ディスク検出、全ディスク再列挙、安定識別、二段階確認を通過した対象だけを扱う物理ディスク境界
- コピー元の読取り専用ハンドル、コピー先のwrite-throughライター、offline/online変更後の再検証
- 物理I/Oバックエンドを使わずに対象差替え、確認不一致、システムディスク、属性不明、状態変更未反映を検証するモックテスト
- 既定OFF、VirtualBox/小容量追加ディスク/管理者権限/固定許可語に限定した破壊的VMハーネス
- 小容量試験と分離した固定96GiB→110GiB起動試験プロファイルと専用許可語
- コピー先Windows/ESPの物理ディスク、開始位置、ファイルシステム、GPT種別を再確認するBCDBoot VMハーネス
- Worker内クローン、GUID/ファイルハッシュ検証、署名検証済みBCDBoot、コピー元不在Boot-Checkを直列化するVMオーケストレーター
- ローカルADK/WinPE Add-onの固定候補、amd64構成、reparse、Microsoft署名、`/bootex`を読み取り専用で確認する`MediaBuilder`
- テキスト/JSON対応の`ytec-winpe-environment.exe`診断CLI
- Windows Installer製品バージョン、適用済みパッチ列挙、署名済みDISM版を組み合わせたADK/KB5101684のフェイルクローズ作成ゲート
- リポジトリ外だけに2011 CA/2023 CA検証用ISOとSHA-256 manifestを作る`New-WinPEBootValidationMedia.ps1`
- HDD/NICなし専用VMによるLegacy BIOS、UEFI、Secure Boot 2011 CA/2023 CA起動マトリクス
- 静的MSVCランタイム、テキスト/JSON、読み取り専用列挙とクローン選択プリフライトを持つ製品`ytec-winpe-app.exe`
- システムディスク判定とコピー先物理ライターのオブジェクト分離、およびWinPEAppの依存DLL監査
- Secure Boot有効WinPE内でimmutable合成GPTディスクを列挙する製品App実行試験
- リポジトリ外のWIM複製へ自作ファイル3点だけを追加し、変更前後と生成ISOをSHA-256監査する`New-WinPEAppValidationMedia.ps1`
- WinPEApp自動起動ISOを使ったSecure Boot有効/NICなし専用VMのテキスト/JSON列挙試験
- 製品Appから独立したVM限定ハーネスによる、実WinPE上の2GiB GPT→3GiB RAWクローン試験
- 製品Appの再列挙、対象選択、確認トークン、実行無効を確認する実WinPE読み取り専用プリフライト試験
- 製品と同じCLI境界へVM専用実行サービスだけを注入する`ICloneExecutionService`境界と拒否/確認モックテスト
- 実WinPEでの製品境界2GiB GPT→3GiB RAWクローン、および96GiB→110GiB起動クローン
- WinPE内の署名検証済みBCDBootと、コピー元不在・Secure Boot有効のコピー先単独起動

## 現時点の検証結果

2026-07-30時点で、ホスト上のMSVC x64ビルドとCTest 11件は成功しました。通常構成には、Windows標準のカタログ署名付き`System32\bcdboot.exe`を許可し、署名のないテスト実行ファイルを拒否するWindows統合試験に加え、MediaBuilderのモック試験、ホストADK検出試験、WinPE環境JSON試験、WinPEAppのヘルプ/プリフライト境界試験を含みます。VM専用破壊的ハーネスを含む静的ランタイム構成もビルドでき、同構成の非破壊CTestは再構成前10件、再構成後11件を実行します。VMラボでは次の構成で、追加前の4テスト実行ファイルがすべて終了コード0でした。

| VM | ファームウェア | Secure Boot | 結果 |
|---|---|---:|---:|
| YWB-Win10-22H2-x64-Clean | UEFI x64 | 無効 | 4/4成功（13:18 JST） |
| YWB-Win11-25H2-x64-Clean | UEFI x64 | 有効 | 4/4成功（13:22 JST） |

証跡は`.validation/evidence/phase1-vm-smoke/<VM名>/<実行日時>/`に保存しています。2台連続実行時、Win11のゲスト内一時ディレクトリ作成が一度だけ失敗し、テスト開始前に中断しました。VMが保存状態、NIC無効であることを確認してWin11単独で再実行し、4/4成功しました。

これは合成ディスク/モックテストのWindows VM内実行結果です。WinPE起動、実仮想ディスクへのクローン、コピー元を外した起動成功を証明するものではありません。

物理I/O試験用に既存VMを変更せず、フルクローンしたWindowsシステムVDI、2GiBのコピー元VDI、3GiBのコピー先VDIだけを持つ`YDC-Phase1-Physical-Helper`を作成しました。NICは無効、UEFI x64、試験後は保存状態です。GuestControlの`YbcTest`はUAC制限トークンで、組み込みAdministratorも無効だったため、初回のディスク初期化は管理者権限不足で停止しました。両追加VDIがRAW、パーティション0件のままであることを再読取りし、この初回診断では物理ライターを実行していません。診断証跡は`.validation/evidence/phase1-physical-vm/20260729-141930/`などに保存しています。

その後、ユーザーがVM画面内のUACだけを手動承認し、残りを自動実行する対話試験経路を追加しました。管理者トークンでの合成コピー元初期化は成功し、コピー元は2GiB GPT、ESP/MSR/基本NTFS/回復NTFSの4区画、コピー先は3GiB RAWであることを確認しました。この過程で次のVM統合不具合を検出・修正しています。

- `Initialize-Disk`が自動作成する先頭MSRと試験用MSRの重複を、既知の合成レイアウトだけに限定して補正
- パーティション0件のRAWディスクをWindows APIがMBR列挙値で返す場合のRAW正規化と回帰テスト
- BIOSレジストリ値が欠けるVirtualBox構成向けに、`GetSystemFirmwareTable('RSMB')`の読取り専用SMBIOS判定を追加
- Windows PowerShell 5.1がネイティブ標準エラーを停止例外として扱い、成功計画を中断する試験ラッパーを修正
- 対話UAC経路のVM準備確認を、VirtualBoxの不安定なゲストプロセス起動ではなく読取り専用`stat`へ限定
- NTFS末尾の完全な割当単位にならないセクターをクラスタとして切り上げていた計算を切り捨てへ修正し、回帰テストを追加

修正後はVM内の読取り専用クローン計画が成功し、モデル、容量、512バイトセクター、GPT/RAW、非システム、書込可、非リムーバブル、シリアル末尾を含む全ゲートと二段階確認トークンまで通過しました。途中の失敗はすべて最初の`WriteFile`より前で安全停止し、原因とコピー先状態を証跡へ残しました。

2026-07-29 23:20 JST開始の最終対話試験では、ユーザーが専用ランチャーとUACだけを手動承認し、2GiB GPTコピー元から3GiB RAWコピー先への物理I/Oが完走しました。`YDC_VM_CLONE_PASS`、665,886,720バイトのデータコピー、3区画コピー、MSR 1区画再作成、全書込み読戻し、主GPTヘッダー最終確定を確認しました。検証結果はPASSで、ディスクGUIDと全パーティションGUIDが再生成され、4区画を維持し、ESP/基本NTFS/回復NTFSのマーカーと8MiBペイロードのSHA-256がコピー元と一致しています。確定証跡は`.validation/evidence/phase1-physical-vm/20260729-232003/`です。専用VMは保存状態、UEFI x64、NIC無効です。

起動試験の初回候補には、Win10 Worker、128GiB Win11コピー元、140GiBコピー先を使用しました。二段階確認まで通過後、Win11基本データパーティションのブートセクターでBitLocker形式を検出し、クローン計画作成中かつ最初のターゲット`WriteFile`より前に安全停止しました。BitLocker解除、回復キー取得、回避は行っていません。失敗証跡は`.validation/evidence/phase1-boot-vm/20260729-235926/`に保存し、候補媒体も削除していません。

BitLocker対象を書き換えて試験に流用せず、Win11をWorker OS、96GiB Win10専用VDIをコピー元、新規110GiB VDIをコピー先とする反転構成へ変更しました。`YDC-Phase1-Boot-Worker-Win11`はUEFI64、NIC無効、3媒体を固定ポートへ接続して正常起動しました。2026-07-30 00:08 JST開始の試行では手動ランチャーが実行されなかったため、コピー処理を開始せず専用Workerを電源OFFへ戻しました。準備証跡は`.validation/evidence/phase1-boot-vm/20260730-000844/`です。

最終対話試験では、96GiB Windows 10 GPTコピー元から110GiB RAWコピー先へのクローンが完走しました。13,218,922,496バイト、3区画をコピーし、MSR 1区画を再作成しました。全書込みの読戻し、主GPTヘッダー最終確定、Disk GUID/全Partition GUIDの再生成、WindowsとEFIブートファイルのSHA-256一致を確認しています。クローン証跡は`.validation/evidence/phase1-boot-vm/20260730-042404/`です。

この試験で、`Get-Disk.UniqueId`をGPT Disk GUIDと誤認していたVM検証、未割当て`DriveLetter`が`$null`ではなくNUL文字になるPowerShell処理、Windows標準BCDBootが埋込み署名ではなくシステムカタログ署名を使う場合の検証不足を修正しました。カタログのハッシュ照合、信頼チェーン、署名者組織名を確認し、不明・不信頼・API失敗時は停止する設計です。

続けて署名検証済みBCDBootが終了コード0となり、BCDストアとEFIブートファイルを確認しました。コピー元を外し、コピー先だけを接続した`YDC-Phase1-Boot-Check-Win10`をUEFI64/NICなしで起動し、Guest AdditionsからWindows 10 10.0.19045と`C:\Windows\System32\winload.efi`を確認しました。最終結果はPASS、確定証跡は`.validation/evidence/phase1-boot-vm/20260730-051853/`です。この最初のBoot-CheckはSecure Boot無効でしたが、後述の実WinPE再試験ではSecure Boot有効で再確認しました。

起動試験後にMediaBuilderの読取り専用診断を追加し、利用者承認後にMicrosoft公式ADK `10.1.26100.2454`、WinPE Add-on、KB5101684をホストへローカル導入しました。必要なOscdimg/DISM更新は終了コード`0`、再起動要求なしです。診断CLIは`baseLayoutReady=true`、`bootexLayoutReady=true`、`versionAndServicingVerified=true`、`mediaCreationPermitted=true`、終了コード`0`となりました。完全構成、パッチ欠落、製品/DISM版不一致、`/bootex`なし、WinPE Add-on欠落、reparse、署名不信頼、x86指定を実ディスクなしのモックで検証しています。

診断ゲートを通過した後、ADK基本媒体だけから標準2011 CA版と`efisys_EX.bin`を使う2023 CA版の検証用ISOをリポジトリ外へ生成しました。HDD/NICなしの`YDC-WinPE-Boot-Matrix`でLegacy BIOS、UEFI/Secure Boot無効、UEFI/Secure Boot有効2011 CA、UEFI/Secure Boot有効2023 CAがすべてWinPE `X:\Windows\System32>`まで到達しました。証跡は`.validation/evidence/winpe-boot-matrix/20260730-1045/`です。VMは電源OFF、NICなしで、ホスト実ディスクは使用していません。

次に`WinPEApp`プレースホルダーを、同じ読み取り専用列挙器を呼ぶ製品`ytec-winpe-app.exe`へ置き換えました。実行中Windowsの所属ディスク判定を`system_disk.cpp`へ分離し、コピー先ライターを持つ`physical_disk.cpp`が製品診断バイナリから要求されないリンク境界にしています。静的MSVC x64版は1,767,936バイト、SHA-256は`612B89DC45F179467D850467CB628AAC857FB2CC92C1A5BF6B8A2AA885BC0F52`で、依存DLLはWinPE標準の`SETUPAPI.dll`、`KERNEL32.dll`、`ADVAPI32.dll`だけです。

自作EXEだけを収録した2,406,400バイトの補助ISOをリポジトリ外へ作り、基本2023 CA WinPE ISOと分離して`YDC-WinPE-App-Diagnostic`へ接続しました。VMはUEFI64、Secure Boot有効、NICなしです。既存の合成2GiB GPT媒体は直接接続せず複製し、baseをimmutableにしました。WinPE内の`--text`と`--json`はどちらもディスク0、`VBOX HARDDISK`、2GiB、SATA、512/512バイトセクター、GPT、4パーティションを列挙し、JSONは`issues: []`でした。証跡は`.validation/evidence/winpe-app-read-only/20260730-1252/`です。基本WinPEへ日本語フォントを追加していないためテキストの日本語は□表示ですが、JSONと数値/GUID/ASCII項目を確認できています。VMは電源OFF、稼働中VMは0台です。

続けて、非昇格では事前診断だけを行い、明示した管理者実行時だけリポジトリ外の未作成出力先へWIM複製とISOを作る`New-WinPEAppValidationMedia.ps1`を追加しました。リポジトリ/ドライブルート/既存出力/reparseを拒否し、ADK/KB/署名ゲートと自作EXEのAMD64 PE32+・固定DLL依存を確認します。WIMへ追加するのは`ytec-winpe-app.exe`、`launch.cmd`、`winpeshl.ini`だけで、元WIMと追加後WIM、追加ファイル、ISOのサイズ/SHA-256をmanifestへ記録します。失敗時はマウントを破棄し、出力やログを成功扱いにしません。

実際のWIM統合では元WIM SHA-256 `FBCBDB1C6651AB3A69384E9D4F95F2C02321318603849453B252E21E827C8197`から、追加後WIM `478EBF794BFC24F6AFFE844113E5D664DE0A22B1B2EA8AB128A4D05CA3B386BA`への変化を確認しました。生成した2023 CA ISOは397,684,736バイト、SHA-256 `1E2BB7C2ED8AF913D376AD65D329AE7E80B224FF9F17FCA0B070A2CB9BED0549`です。専用`YDC-WinPE-App-Integrated`をUEFI64、Secure Boot有効、NICなしで起動するとWinPEAppが自動実行され、合成2GiB GPTディスクをテキスト/JSONで列挙して`issues: []`を返しました。証跡は`.validation/evidence/winpe-app-integrated/20260730-1310/`です。VMは電源OFF、稼働中VMは0台です。このISOにクローン機能はありません。

次にVM限定ハーネスを自作補助ISOへ収録し、`YDC-WinPE-Clone-Execution`のUEFI64/Secure Boot有効/NICなしWinPEで2GiB GPTから新規3GiB RAWへクローンしました。668,758,016バイト、3区画をコピーし、MSR 1区画を再作成しました。全書込み読戻し、主GPT最終確定、新しいDisk GUID、コピー先4区画、`issues: []`を確認しています。コピー元Disk GUIDは`{3C4E522B-FB72-4162-9388-E2615D300203}`、コピー先は`{1F7F8993-FF82-4FBD-82C0-229F91338209}`です。証跡は`.validation/evidence/winpe-clone-execution/20260730-1321/`です。ホスト/実機ディスクは使用せず、製品Appからこのハーネスを起動する経路もありません。

製品WinPEAppへ書込みを接続する前段として、`--clone-preflight --source N --target N`を追加しました。成功しても`executionEnabled=false`を返し、当時の実行指定は列挙前に終了コード`64`で拒否しました。この時点の静的MSVC x64版は1,816,064バイト、SHA-256は`884766166A5B44260B8DA2DF8E8A0F16D2EB365F63176E97EA75E62F82978519`で、依存DLLは`SETUPAPI.dll`、`KERNEL32.dll`、`ADVAPI32.dll`だけです。後続変更で現在の明示指定は`--clone-execute`へ統一しています。

この製品Appを自作補助ISOから`YDC-WinPE-Product-Preflight`のUEFI64/Secure Boot有効/NICなしWinPEで実行しました。2GiB GPTコピー元と新規3GiB RAWコピー先をテキスト/JSONで再識別し、コピー先固有トークン、`executionEnabled=false`を確認しました。実行後もコピー先はRAW、パーティション0件、`issues: []`です。証跡は`.validation/evidence/winpe-product-preflight/20260730-1331/`です。

次に`ICloneExecutionService`を追加し、`--clone-execute`が対象消去承認、正確な確認トークン、許可語、再列挙プリフライトを通過した場合だけ注入サービスを呼ぶ境界へ変更しました。通常製品の`main`はサービスを注入せず、実行要求を列挙前に終了コード`64`で拒否します。破壊的VMビルドだけがVirtualBox、管理者権限、固定容量、固定許可語を再検証するサービスを注入します。

実WinPEでは製品と同じCLI境界を通し、2GiB GPT→3GiB RAWを完走しました。668,758,016バイト、3区画コピー、MSR 1区画再作成、全読戻し、主GPT最終確定、コピー先online復帰、新しいDisk GUID、4区画、`issues: []`を確認しています。証跡は`.validation/evidence/winpe-product-execution/20260730-1352/`です。

さらに実WinPEで96GiB Windows 10 GPT→110GiB RAWをクローンし、12,950,999,040バイト、3区画コピー、MSR 1区画再作成、全読戻し、主GPT最終確定、コピー先online復帰を確認しました。Microsoft署名検証を必須とするBCDBootハーネスは終了コード`0`でBCDストアとEFIフォールバックファイルを生成しました。コピー元とDVDを外し、コピー先だけを接続した`YDC-WinPE-Boot-Check-Secure`はEFI64、Secure Boot有効、NICなしでWindows 10 10.0.19045まで起動し、Guest Additions応答とログイン画面を確認しました。確定証跡は`.validation/evidence/winpe-boot-secure/20260730-1409/`です。

変更後の通常ビルド、CTest 11/11、静的ランタイムビルドとCTest 11/11、依存ライセンス0件、安全境界、WinPEメディア境界、SBOM、MSVC`/analyze`、AddressSanitizerビルドとCTest 11/11はすべてPASSです。

## 2026-07-30時点の未実装・未検証

- 製品UI/CLIからのクローン開始
- クローン機能を有効にした製品向けISO/USB生成
- 通常製品WinPEAppへの実行サービス接続（VM専用注入経路ではoffline/online、物理I/O、BCDBootまで検証済み）
- 基本WinPEでの日本語UI/ログ表示方式と必要コンポーネントの再配布条件確認
- 最新WinPEAppをWIMへ再統合したISOの生成と再起動確認（管理者操作待ち）
- Windows回復環境の実起動と復旧操作
- BitLocker状態API、動的ディスク、Storage Spacesの統合事前診断
- 4Kn、MBR/Legacy BIOSの実WinPE起動、VSS、物理ディスク/ファイルを使うイメージ保存・復元

## 次の実装ゲート

Phase 1のVM範囲は製品CLI境界、物理I/O、BCDBoot、コピー元不在Secure Boot起動まで到達しました。次は外部依存を増やさず、Phase 2の厳密な`.dcimg` v1パーサー/インデックス/SHA-256検証基盤と、Phase 3のMBR/Legacy BIOS基盤を合成ディスクとVM専用経路で進めます。最新AppのWIM再統合だけは管理者操作が可能な時点で行います。Windows回復環境の起動、容量不足・対象差替え・破損GPTなどの失敗経路も追加確認します。利用者方針に従い、一通りの機能と実機向けチェックリストをレビューするまで、ホスト実ディスク、実機、実機USB用ISOコピーは使用しません。

## 2026-07-30 後続Phase基盤の追記

上記の「次の実装ゲート」後、Phase 2はMBR/GPTパーティション表スナップショットと、全体検証後にデータを読戻しながら復元しパーティション表を最後に確定する合成境界まで進みました。Phase 3は固定48GiB→56GiBの物理MBRクローンとBIOS BCDBootのVM専用ハーネス、Phase 4はMicrosoft署名済みMBR2GPTのvalidate→再識別→convert境界と固定VMハーネスまでビルド済みです。いずれも通常製品入口へは接続しておらず、Legacy BIOS/MBR2GPTの実WinPE試験は管理者操作を伴う最新媒体生成後に行います。

## 2026-08-02 製品予約ジョブ経路の追記

後続Phaseで通常製品の検証済み予約ジョブ経路へGPT/MBRクローンと復元を接続しました。
固定96GiB Windows 10 GPTから110GiB RAWへの製品クローンをWinPEで完走し、
区画/データ読戻し、主副GPT、コピー先online復帰、署名検証済みBCDBootを確認しました。
保護したコピー元VDIのSHA-256前後一致後にコピー元とISOを外し、コピー先だけで
Secure Boot有効UEFIからWindowsを起動しています。確定証跡は
`.validation/evidence/product-gpt-clone-boot-vm/20260802-135917/`です。

現在残るのは修正済み自動検証ランナーを含む最新ISO再生成、同じ起動回帰、
Zstandard版VSS復元、4Kn/実機です。上の2026-07-30時点一覧は履歴として残します。
