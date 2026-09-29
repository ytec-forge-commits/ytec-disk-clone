# Phase 3 MBR/Legacy BIOS進捗（2026-07-30）

> **履歴資料:** v2再設計前のPhase報告です。予約ジョブに関する記述は廃止済みで、
> 現行製品仕様ではありません。

## 実装済みの純粋ロジック

- 512バイト論理セクターのLegacy BIOS用MBR解析
- `0x55AA`、予約領域、4個のプライマリエントリ、Active値、LBA/容量、重複範囲の厳密検査
- GPT保護MBR、複数Active、不正ブート値、範囲外区画の明示停止
- EBRチェーン未実装時の拡張パーティション型`0x05/0x0F/0x85`の安全側拒否
- Windows CNG乱数によるコピー先ディスク署名生成
- 0、コピー元、接続中ディスクの署名候補を避け、32回の有界再試行後は停止する計画
- コピー元ブートストラップ、4エントリ、Active状態を保ちながら新しい署名を持つ512バイトMBR生成
- BCDBootの既存署名/絶対パス/固定引数境界を、`/f UEFI`と`/f BIOS`の明示列挙へ拡張
- BIOSではActiveシステム区画とWindows区画が同一ドライブの場合を許可し、UEFIではESP分離を維持
- 合成MBRディスクのNTFS使用クラスタ、回復NTFS全領域、読戻し検証、MBR最終確定を行うオフラインクローン計画/実行
- 最初にコピー先MBRを無効化し、データflush後に新署名を持つMBRを最後に確定する失敗時境界
- 安定識別、二段階確認、512バイトセクター、コピー元/コピー先寸法を再確認してからだけ実行する共通境界
- 確認語不一致、NTFS先頭クラスタ欠落、読戻し不一致、BitLocker形式を合成I/Oで安全停止する統合テスト
- MBR `0x07`パーティションを物理ディスク番号と開始オフセットで読取り専用ボリュームへ対応付ける境界
- 既定OFFのVM専用実行サービスに、固定48GiB MBRコピー元から固定56GiB RAWコピー先だけを許可するLegacy BIOSプロファイル
- 通常製品から分離した固定56GiB VirtualBox用BIOS BCDBootハーネス。Windows/システムNTFS、物理ディスク、開始位置、一意Active、`winload.exe`を再検証する

## 現在の制限

- MBRクローン計画はプライマリパーティションだけを対象とし、起動計画にはActive区画を1個要求する
- 2TiB超、4Kn、拡張/論理パーティション、動的ディスク、BitLockerは未対応
- 物理MBRクローンと`/f BIOS`ハーネスは破壊的VM専用構成でビルド済み。最新製品WIMを複製して2ハーネスだけを追加する監査付きメディア生成スクリプトと境界テストも実装・実行済み
- 通常ビルドは直接MBR書込みCLIを公開せず、検証済み予約ジョブと二段階確認を通る
  製品WinPE実行サービスだけへ接続済み
- Legacy BIOSのWindows起動VM試験は完了。仕様上のPhase 3完了条件に含まれる実機試験は、全機能が揃った最後に利用者がまとめて行う
- MBR→GPTはPhase 4でMicrosoft署名済み`mbr2gpt.exe`を使う別境界とし、独自変換は実装しない

## 次の安全ゲート

1. EBR対応の必要範囲をWindows 10 MBR構成で確定し、実装する場合は循環、範囲外、重複EBRを拒否するパーサーを先に追加する。
2. BIOS用BCDBootの重複防止は2026-08-02に用途別方針を実装済み。最新ISOで
   製品クローン後の単一項目を再確認する。
3. 実機試験は全機能が揃った最後に利用者がまとめて実施する。

## 2026-07-30 Legacy BIOS VM事前確認

- 共有ラボの`YWB-Win10-22H2-x86-Clean`と検証済みスナップショットは変更せず、現在の48GiBシステム媒体をプロジェクト側へ独立した単体VDIとして合成コピーした
- 新規56GiB RAW VDIと、Legacy BIOS、全NIC無効の`YDC-Phase3-LegacyBIOS-Worker`を作成した
- 書込みサービスを持たない最新製品WinPE ISOで自動列挙し、Disk 0が48GiB/MBR/512バイト/Active `0x07` 1区画、Disk 1が56GiB/RAW/512バイト、両者のシリアル末尾が異なることを確認した
- 事前確認画面は`.validation/evidence/phase3-preflight-vm/20260730-1938-readonly/readonly-inventory.png`に保存した
- 実構成に合わせ、コピー先パーティション1を`W:`へ割当て、WindowsルートとBIOSシステムルートを同じ`W:\`としてBCDBoot境界へ渡す固定スクリプトへ修正した
- Phase 3メディアはCOM1へ開始/完了ログを出し、ホスト側が画面へのコマンド入力なしでPASS/FAILを監視できる

## 2026-07-30 Legacy BIOS VM最終結果

- リポジトリ外の監査付きISO
  `%LOCALAPPDATA%\YTEC\ytec-disk-clone\phase3-winpe-validation\20260730-200816\YDC-Phase3-LegacyBIOS-VMOnly-amd64-2023CA.iso`
  を生成した。400,449,536バイト、SHA-256は
  `05C2F20B4456E2B5FA2A39AACEB99ED0C4D6D73A027673552A555101261313C7`
  である
- 生成マニフェストは`vmOnly=true`、
  `productWriteServiceConnected=false`を記録し、基本製品WIM
  `923BA63823CE7B0652A63011FA71FB3E145A0A2D21CCAA74F70154F7AF2CD077`
  から、2個のVM専用ハーネスと固定起動スクリプトだけを追加したWIM
  `0DE1632C2ACC9F85752424E5BB71FBD7C12ADE0C01CC68AFFC945E8731813335`
  を作成した
- 最初の媒体では基本WinPEに`findstr.exe`がないことを検出し、コピー先への最初の書込みより前に
  `clone-confirmation`段階で安全停止した。確認語抽出を`cmd.exe`組込みの`for /f`へ変更し、
  メディア境界テストで`findstr`依存を禁止した
- 修正版で48GiB MBRコピー元から56GiB RAWコピー先への物理クローン、
  新しいディスク署名、Active `0x07`区画、読戻し検証、
  Microsoft署名済みBCDBoot `/f BIOS`を完走した。画面とCOM1ログで
  `YDC_VM_CLONE_PASS`、`YDC_VM_BIOS_BCDBOOT_PASS`、
  `YDC_PHASE3_AUTOMATION_PASS`を確認した
- コピー元VDIとISOを外し、コピー先VDIだけをLegacy BIOSで起動して、
  Windows 10 10.0.19045のデスクトップとNIC 0を確認した
- 確定証跡は
  `.validation/evidence/phase3-legacy-bios-vm/20260730-2010-retry/`、
  媒体生成ログは
  `.validation/evidence/phase3-winpe-media/20260730-200816/build.log`
  に保存した。VMは電源OFF、コピー元とISOは未接続である
- 起動には成功したが、Windows Boot ManagerにWindows 10項目が2件表示された。
  既存BCDへBCDBootが項目を追加した可能性があり、製品統合前に重複防止を確認する

Phase 3のVM範囲は完了した。32bit Windowsは製品対象外であり、この媒体はMBR物理I/Oと
Legacy BIOS起動経路の隔離試験だけに使用した。Phase 4のMBR→GPT試験には、
別途Windows 10 x64専用VMを使用する。

## 2026-08-02 製品予約ジョブ単独起動

- 固定48GiB Windows 10 x64 MBRコピー元から56GiB RAWへ、通常製品の予約ジョブ経路で
  3区画コピー、全読戻し、MBR確定、online復帰、署名検証済みBCDBoot `/f BIOS`を完走
- 保護したコピー元VDIのSHA-256
  `C9C5D7FDCBC793349691BF16C6CFF990A08A0CBBF70BCE28FA25FCD62831C72B`
  は前後一致
- コピー元とISOを外したコピー先だけでLegacy BIOSからWindowsを起動し、ゲスト側から
  `DiskStyle=MBR`、`IsBoot=1`、`IsSystem=1`、`PartitionCount=3`、
  `ActiveCount=1`、`Architecture64=1`を確認
- NIC無効、実ディスク/USB不使用、Worker設定復元を確認。確定証跡は
  `.validation/evidence/product-mbr-clone-boot-vm/20260802-143059/`
- 起動メニューのWindows 10項目は今回も2件表示された。保護元を起動せず作った監査用
  クローンはメニューなしで直接Windowsへ到達し、製品コピー先だけ2件を表示した。
  標準権限のBCDオブジェクト読取りはアクセス拒否となったため、厳密比較はUAC作業へ残す
- 新しい製品クローン/復元/MBR→GPTコピー先は仕様どおり既存BCDを流用せず、Microsoft
  BCDBootの固定`/c`で新規ストアを構築するよう変更した。単独起動修復のみは正当な
  マルチブート構成を消さないよう既存ストア保持を継続する。未知の方針値は実行前拒否し、
  通常/VM用静的ビルドと単体試験はPASS。最新ISOでの再確認は未実施
