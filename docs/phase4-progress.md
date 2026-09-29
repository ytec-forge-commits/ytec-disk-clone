# Phase 4 MBR→GPT進捗（2026-07-31）

> **履歴資料:** v2再設計前のPhase報告です。再利用部品の証跡であり、Windows／PEの
> v2直接製品経路が完成したことを意味しません。

## 実装済みの安全なプロセス境界

- 独自のMBR→GPT変換ロジックは実装せず、現在のWinPEの`System32\mbr2gpt.exe`だけを対象にする
- `PATH`検索、任意EXE、reparse、署名不明を許可せず、埋込み署名またはWindowsシステムカタログと`Microsoft Corporation`署名者を検証する
- 引数は`/validate /disk:N`と`/convert /disk:N`の固定2個だけを生成する
- `/allowFullOS`を生成せず、フルWindows上の変換を許可しない
- `/map`を生成せず、未知パーティション型を推測変換しない
- 安定識別、実行中システムディスク除外、二段階確認を`/validate`前に検証する
- `/validate`終了コード0の後、同じディスク番号を読み取り専用で再列挙し、安定識別と確認を再検証してからだけ`/convert`を起動する
- validate/convertの標準出力、標準エラー、終了コード、Microsoft署名検証、再識別完了を成功レポートに保持する
- validateまたはconvertの非ゼロ終了、対象差替え、ディスク番号変更、署名失敗を成功扱いにしない
- 変換前に対象Windowsの`System32\ntoskrnl.exe`を読取り専用かつreparse無効で開き、DOS/COFF/オプショナルヘッダーを有界解析する
- Windows 10/11 x64のAMD64 PE32+だけを許可し、x86、ARM64、機械種別とオプショナルヘッダーの不一致、切詰めを変換前に拒否する

## 自動テスト

モックで固定引数、未知動作拒否、正常なvalidate→再識別→convert、確認不一致、署名失敗、validate失敗、対象差替え、ディスク番号変更、convert失敗、System32パス不正を確認します。PEヘッダー単体テストではAMD64、x86、ARM64、機械種別不一致、切詰め、範囲外オフセットを確認します。Windows統合試験ではホストの正規`System32\mbr2gpt.exe`のMicrosoftカタログ署名を確認しますが、ホスト上でプロセスは実行しません。

## WinRE読み取り専用診断

別ターゲット再構築プランへ渡すWinRE情報を推測で作らないため、
`inspect_winre_source`を追加しました。現在のWindowsまたはWinPEの
信頼済み`System32\reagentc.exe`についてMicrosoft署名を検証し、文書化された
`/info /target <オフラインWindows>`だけを固定引数で実行します。任意コマンド、
`PATH`検索、オンライン構成変更、`/enable`、`/disable`、`/setreimage`は
実行しません。

REAgentC出力から`GLOBALROOT\device\harddiskN\partitionN`を有界解析し、
再識別済み対象ディスク番号との一致を確認してから、登録先の
`Recovery\WindowsRE\Winre.wim`を`GENERIC_READ`だけで開きます。登録がない場合は
固定の`<オフラインWindows>\System32\Recovery\Winre.wim`だけを確認します。
通常ファイル、非reparse、1バイト以上8 GiB以下を必須にし、異なる複数登録先、
対象ディスク不一致、古い登録だけが残る状態、REAgentC非ゼロ終了は安全側に
不明として扱います。

確認済みの登録区画、Windows内フォールバック、欠損の事実だけを
`Mbr2GptRebuildRequest`へ移す接続も実装しました。不明状態や署名・読取り専用
境界が確認できないレポートは再構築プランを有効化しません。ASCII/UTF-16LE
出力、固定引数、パス不正、登録先不一致、欠損/古い登録、署名失敗、
非ゼロ終了、プラン入力を実ディスクなしのモックで確認し、通常/VM CI、
MSVC`/analyze`、ASanを通しています。

製品WinPE CLIには`--winre-diagnostic --disk N --windows-root W:\`として
接続しました。物理ディスクの再列挙と安定識別、固定ルート、不明時の終了コード1、
テキスト/JSONの`readOnly=true`と`executionEnabled=false`をモックで確認します。
WinPE GUI表示と実VM内のオフラインWindowsに対する実行はまだ行っていません。

## VM専用ハーネス

`YTEC_BUILD_DESTRUCTIVE_VM_TESTS=ON`でだけ`ytec-phase4-mbr2gpt-vm.exe`と`ytec-phase4-bcdboot-vm.exe`をビルドします。固定56GiB、VirtualBox、512バイト、online、非システム、書込可、非リムーバブル、MBR、1～3個の既知基本区画、一意Active、対象NTFS Windows、AMD64 PE32+カーネル、`winload.exe`、BitLockerシグネチャ不在、管理者権限、固定許可語、二段階確認を必須にします。変換後のBCDBoot側は同じ56GiB媒体、GPT、ESP FAT32、Windows NTFS、物理ディスク対応を再検証します。

通常製品、通常CTest、製品WinPEAppからこのハーネスを起動する経路はありません。

## 2026-07-30 VM実行結果

- Windows 10 Pro x64 `10.0.19045`をLegacy BIOS/MBR、固定56GiB、NICなしの新規`YDC-Phase4-MBR2GPT-x64`へ導入した
- ISO SHA-256
  `A25504F176B9886BA52E43A8509C4932BAAF24CF72B7BD1C3A4D005445752040`
  の初回VM専用WinPEで`mbr2gpt.exe /validate`と`/convert`が終了コード0になり、Microsoft署名と変換直前の対象再識別を確認した
- 変換後の読み取り専用再列挙で、同じ56GiB媒体がGPT、Windows基本データ区画、100MiB EFIシステム区画になったことを確認した
- Legacy BIOS起動のWinPEでは`mountvol S: /S`が「パラメーターが間違っています」で停止した。変換成功とBCDBoot成功を混同せず、`YDC_PHASE4_AUTOMATION_FAIL stage=esp-mount`として証跡を保存した
- 修正後は、使用中の`S:`を拒否してからVM固定のdisk 0 / partition 2だけをDiskPartで割り当てる。続くBCDBootハーネスが、同じ56GiB VirtualBox GPT媒体、ESPの物理offset/FAT32、Windows区画の物理対応/NTFSを再検証してからだけ書き込む
- BCDBoot後はDiskPartで`S:`を解除し、解除失敗も成功扱いにしない。`clean`、`format`、任意ディスク/区画の指定は媒体スクリプトと境界テストで禁止する
- 再開用ISO SHA-256
  `F89F13EFD6164A23B483C462F2EAF8216D2E7B6E7A38E34BB6388B11D21DEDE3`
  で、`Boot files successfully created.`、署名検証済みBCDBoot、ESPドライブ文字解除、`YDC_PHASE4_BCDBOOT_RESUME_PASS`を確認した
- ISO/補助媒体を外し、対象VDI 1台、UEFI64、Secure Boot無効、NICなしでWindows 10 Pro x64 `10.0.19045`のデスクトップとGuest Additions 7.1.4を確認した
- Windows区画へ保存したPhase 4ログをGuest Additionsのファイルコピーだけで回収し、`microsoftSignatureVerified=true`、BCDBoot終了コード0、ESP区画2、最終PASSを再確認した。ゲスト内コマンドは実行していない
- VirtualBox NVRAMへMicrosoft署名群とPlatform Keyを登録し、`PK`、`KEK`、`db`、`SecureBootEnable`が存在するSecure Boot有効構成で、同じ対象VDI 1台からWindows 10 `10.0.19045`のログオン済みデスクトップまで起動した
- 主要証跡は`.validation/evidence/phase4-mbr2gpt-vm/20260730-211544/`、`.validation/evidence/phase4-bcdboot-resume-vm/20260730-212737/`、`.validation/evidence/phase4-uefi-boot/20260730-213451/`、`.validation/evidence/phase4-secureboot/20260730-214614/`に保存した
- VirtualBox 7.1.4はゲスト停止後にホスト側状態が`stopping`で残るため、稼働中VMが0、対象VDIが5分以上未更新、対象UUIDの`VBoxHeadless`が正確に3プロセスだけであることを確認して残留プロセスを終了した。これは製品/ゲストの起動失敗ではないが、VMラボ基盤の既知事項として残す
- WinPE標準`diskpart.exe`は単体の埋込み署名済みとは主張せず、SHA-256固定済みMicrosoft WinPE基礎WIMを信頼起点とする。使用したEXEのSHA-256
  `EF268BC1295F9F4323508764B71151D0597C6B9467863E846432FB54B58D76B2`
  も媒体manifestへ記録した
- 通常製品の書込みサービスはどちらのWIMにも接続していない

## 2026-08-03 製品最終VM回帰

- BCD新規再構築を、既存BCDの非上書き退避、`BCDBoot /c`、新規BCD確認、
  成功時の退避削除、失敗時の旧BCD復元を行うトランザクションへ強化した
- 最新製品経路でMBR→GPT変換を先頭から実行し、GPT/ESP/MSR、Windows x64、
  BCDを再検証した後、ISOを外した対象だけでUEFI64/Secure Boot起動した
- 判定はターゲットへ事前配置した一回限りの自己削除型VMプローブとUARTで取得し、
  GuestControl、ゲスト資格情報、キーボード入力へ依存しない
- 確定証跡: `.validation/evidence/product-mbr2gpt-vm/20260803-062217/`

## 現在残る範囲

- 初回MBR2GPT媒体が保存したMicrosoft診断ログ4ファイルの追加回収と機密情報除去方針
- WinRE診断のWinPE GUI表示、ライブVM確認、変換後の登録状態再検証
- 実ディスクから区画順、型、容量、ESP作成余地を集約する製品向け読み取り専用診断
- 検証済み実行契約を対象限定ライター、内容移行、Windows領域拡張、WinRE配置/登録へ接続する処理
- 実機試験。全機能がVMで揃った最後に利用者が実施する

## パーティション配置の方針

現在のin-place変換はMicrosoft標準MBR2GPTが`/validate`で受理する構成だけを
対象にし、既存パーティションの任意移動、サイズ変更、独自の型変換、
欠損WinREの新規構築を行いません。MBR2GPTがESPを作成または既存システム
領域を再利用できる場合は標準動作へ任せ、変換後にGPT、ESP、Windows領域、
BCDを別々に再検証します。拡張/論理区画、未知パーティション型、空き領域不足、
Active不明など標準要件を満たさない構成は安全側に停止します。

回復パーティションがWindows領域の直後かつディスク末尾にある構成は、
Windowsの一般的な推奨配置に合うため、それだけを理由に移動しません。
回復領域が欠損、狭すぎる、Windows領域より前にあり将来の拡張を妨げる、
またはWinRE登録が破損している場合は、次の二段階で対応します。

1. まず読み取り専用診断で現在の区画順、サイズ、型、WinRE登録、必要空き容量を
   表示し、MBR2GPTだけで変換可能かを分類する。WinRE登録先とイメージの
   診断APIは実装済みで、区画全体の製品表示への統合は未実装。
2. 理想配置への変更が必要な場合は、既存ディスクをin-placeで自動移動せず、
   原則として別のコピー先へESP、MSR、Windows、十分な回復領域を新規作成し、
   データ復元、BCDBoot、WinRE登録、再読込み、VM起動確認後に切り替える。

元ディスクを直接並べ替える高度なin-place再配置は、電源断や途中失敗時の
復旧リスクが大きいため通常機能には含めません。必要性が確認された場合だけ、
専用VM試験行列、ロールバック設計、利用者の明示承認を別途設計して判断します。

## 2026-07-31 別ターゲット再配置プラン

コピー元を変更しない純粋ロジック
`plan_mbr2gpt_rebuild_to_new_target`を追加しました。Microsoftの現行文書に
合わせ、別の空ターゲットを次の順で計画します。

1. EFIシステム区画: 512/512eは200 MiB、4Knは300 MiB、FAT32
2. Microsoft予約区画: 16 MiB
3. Windows区画: コピー元容量以上とし、残容量をここへ割り当て
4. 回復ツール区画: 990 MiB以上、かつ確認済み`Winre.wim`容量＋
   250 MiB以上
5. 任意の確認済みNTFSデータ区画

根拠はMicrosoft Learnの
[UEFI/GPT-based hard drive partitions](https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/configure-uefigpt-based-hard-drive-partitions?view=windows-11)、
[MBR2GPT](https://learn.microsoft.com/en-us/windows/deployment/mbr-to-gpt)、
[Deploy Windows RE](https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/deploy-windows-re?view=windows-11)、
[Windows RE partition requirements](https://learn.microsoft.com/en-us/troubleshoot/windows-client/windows-security/disk-partition-requirement-use-windows-re-tool)
です。Microsoft文書どおり、in-place予備判定は`mbr2gpt /validate`の代用に
しません。

回復領域がWindowsより前、またはデータ区画を挟んだ位置にある場合は、
別ターゲット上でWindows直後へ再配置します。回復領域が欠損または狭すぎる
場合は、確認済みWinREイメージがある時だけ新規作成/配置を計画します。
WinRE不明/欠損、登録区画不一致、未知/OEM区画、拡張/論理区画、非NTFS、
BitLocker未復号、UEFI/x64不明、容量不足は安全側に拒否します。

`make_mbr2gpt_gpt_metadata_plan`は、この配置を128エントリのGPTとして
メモリ上に具体化します。ESP、MSR、Windows/データ、Windows REへMicrosoftの
型GUIDを割り当て、Windows REには`0x8000000000000001`（必須＋既定の
ドライブ文字なし）を設定します。保護MBR、主副の区画配列、バックアップ
ヘッダーを先に並べ、プライマリヘッダーを最後のcommit候補にします。

合成テストでは、推奨済み配置、回復領域がWindowsより前、回復領域欠損、
WinRE欠損/不明、狭小回復領域、任意データ、4Kn、容量不足、
拡張/論理区画、BitLocker未復号に加え、型GUID、回復属性、LBA一致、
メタデータ確定順、改ざんされた区画順/重複範囲の拒否を確認しています。
`prepare_mbr2gpt_target_build_execution`は配置と生成済みGPTを相互検証し、
ディスク/区画GUID、型、LBA、回復属性、メタデータ書込み順の改ざんを拒否します。
Microsoft `/validate`、コピー元再識別、空ターゲット/二段階確認、署名済み
Microsoftツール確認を最初のターゲット変更より前に固定し、その後のESP作成、
区画内容移行、Windowsファイルシステム拡張、WinRE新規配置または移行、
BCDBoot、WinRE登録、主副GPT/内容/起動ファイル/登録の再検証、コピー先単独
コールドUEFI起動を順序付き契約として生成します。すべての工程でコピー元
読取り専用を固定し、`execution_adapter_connected=false`、
`physical_write_started=false`のため、この段階ではデバイス適用へ進みません。

## 次の安全ゲート

1. 製品対象外のPhase 3 x86媒体を流用せず、Windows 10 Pro x64を固定56GiBへLegacy BIOS/MBRで導入した専用VMを隔離する。
2. VM限定WinPE媒体を管理者操作でリポジトリ外へ生成し、変換前のAMD64、MBR、Active、NTFS、BitLocker不在を確認する。
3. 自動WinPE試験でvalidate/convert、GPT再列挙、ESPマウント、署名済みBCDBoot `/f UEFI`を直列確認する。
4. ISOを外してUEFI64へ切替え、Secure Boot無効で初回起動後、Secure Boot有効でもコピー先単独起動することを確認する。
5. 変換失敗媒体は削除せず、成功媒体と明確に分離して証跡を残す。

## 2026-07-30 x64専用VM準備

- Phase 3の起動媒体はWindows 10 x86であり、仕様のWindows 10/11 x64対象外なのでPhase 4へ流用しない
- 既存ISOをサイズ6,023,215,104バイト、SHA-256
  `4949E32D1B96C66C79BB8F2227624E74B5DBDA485CA0D4F8A32852F51E67AEA8`
  で照合し、VirtualBoxがWindows 10 Pro x64のImage Index 3として認識することを確認した
- 新規`YDC-Phase4-MBR2GPT-x64`をLegacy BIOS、固定56GiB、NICなしで作成し、既存VM/スナップショットとは分離した
- 資格情報はリポジトリ外の`.validation/vm-secrets`へACLを限定して生成し、値をログ/manifestへ出力しない
- `scripts/New-Phase4WinPEValidationMedia.ps1`は検証済み製品WIMの複製へ2個のVM専用ハーネスだけを追加し、MBR2GPT→GPT再列挙→ESPマウント→BCDBootを自動化する。通常製品の書込みサービスは接続しない
