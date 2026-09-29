# 実機前最終検証サマリー（2026-08-03）

> **履歴資料:** v2再設計前の旧製品経路に対する検証です。再利用部品の証跡として
> 保持しますが、1.0.0の直接操作と`.tsumugi`製品経路の合格には数えません。

## 判定

引継ぎで定義した実機前の完成基準はPASSです。試験対象は固定VirtualBox VMと
合成VDIだけで、物理ディスク、実USB、ネットワークは使用していません。

## BCD新規再構築

クローン、復元、MBR→GPTのコピー先では、Microsoft署名済みBCDBootの事前確認、
固定BCDパスとreparse不使用の確認、既存BCDの非上書き退避、署名再確認、
`BCDBoot /c`、新規BCD確認、成功時の退避削除を1トランザクションにしました。
失敗時は部分BCDを除去して旧BCDを復元します。単独起動修復の既存ストア保持方針は
変更していません。単体テストは29/29 PASSです。

## 製品VM回帰

| 経路 | 結果 | 証跡 |
|---|---|---|
| GPT→GPTクローン、コピー先単独UEFI/Secure Boot起動 | PASS | `.validation/evidence/product-gpt-clone-boot-vm/20260803-011915` |
| MBR→MBRクローン、コピー先単独Legacy BIOS起動 | PASS | `.validation/evidence/product-mbr-clone-boot-vm/20260803-023613` |
| VSS/Zstandardイメージ作成物の別GPTディスク復元、単独Secure Boot起動 | PASS | `.validation/evidence/product-vss-restore-vm/20260803-033054` |
| Microsoft標準MBR2GPT、BCD新規再構築、単独UEFI/Secure Boot起動 | PASS | `.validation/evidence/product-mbr2gpt-vm/20260803-062217` |

GPT/MBR試験では保護コピー元VDIのSHA-256が試験前後で一致しました。VSS試験でも
保護イメージのSHA-256が前後一致しています。全経路で製品処理後にコピー元とISOを
切り離し、対象だけでWindowsが起動することを確認しました。BCDの重複メニューは
再現していません。MBR2GPTの最終判定は、製品処理前にターゲットWindowsへ一回限りの
自己削除型VMプローブを安全に配置し、GuestControlやキーボード入力に依存しない
UART証跡で取得しました。このプローブはVM専用で通常製品には含まれません。

## 最新製品媒体

生成ルート:
`%LOCALAPPDATA%\YTEC\ytec-disk-clone\final-media-candidate\20260803-063736`

| 媒体 | バイト | SHA-256 |
|---|---:|---|
| 2011 CA製品ISO | 435,118,080 | `EE9D2A6D4F011CEEC57C2E89F5A76A61242F78B25E23B5DE85163ABA37D638BF` |
| 2023 CA製品ISO | 435,103,744 | `DDEE9A786D5D804EE5F6A84EEFB519161BC53B073DAACD07232B00B149184910` |
| MBR2GPT 2023 CA VM専用ISO | 438,394,880 | `726B48FECA4D8C918E65F1B38B097445CF4D0C823580CCBC0C00CDA0D3CF6EA9` |

2011/2023 CA製品ISOはBIOS、UEFI/Secure Boot無効、UEFI/Secure Boot有効の
6条件すべてで日本語GUIを表示しました。目視で文字切れ、重なり、起動エラーなしを
確認しています。証跡は
`.validation/evidence/winpe-product-boot-matrix/20260803-064210`です。
manifestとISO、WIM追加ファイルのSHA-256は一致し、WIMマウント残留と
リポジトリ内Microsoft媒体はいずれも0件です。

## 自動検証

- 通常CTest: 40/40 PASS
- 静的CRT CTest: 40/40 PASS
- ASan CTest: 40/40 PASS
- MSVC `/analyze`: PASS
- PowerShell UTF-8 BOM、ライセンス、SBOM、安全境界、WinPE媒体境界、
  起動マトリクス境界、ポータブル配布境界: PASS

## 実機初回試行で判明した16KiB物理セクター対応

2026-08-03 08:29、Samsung SSD 980 1TBのオンラインイメージ作成を開始したところ、
Windowsが論理512バイト／物理16,384バイトを報告し、旧実装が物理セクターを
512/4096バイトだけに限定していたため、VSS開始前・出力作成前に安全停止しました。
ディスクの安定識別、容量、論理セクターは再列挙前後で一致し、コピー元への書込み、
完成`.dcimg`、`.partial`はいずれもありません。

dcimg本体が既に持つ安全境界へ全層を統一し、論理セクター512/4096バイト、
物理セクターは論理以上の2の累乗かつ整数倍、64KiB以下を許可しました。
16KiBをマニフェスト、VSS計画、VSSコピー、製品実行入口まで保持する4本の
回帰テストと、非2の累乗/64KiB超の拒否を追加しました。修正後の通常/静的CRT/
ASan各40/40、MSVC静的解析、全境界検査はPASSです。実機での全量バックアップ
再試行は利用者確認として残します。

## 実機2回目で判明したVSS COM初期化順序対応

2026-08-03 08:55、16KiB対応版で同じオンラインイメージ作成を開始し、セクター
条件を通過しました。その後、VSS COMセキュリティ初期化が
`RPC_E_TOO_LATE (0x80010119)`で停止しました。アプリ内のWindows Update状態確認が
先にCOMを利用した後、バックアップ開始時にプロセス全体の`CoInitializeSecurity`を
初めて呼んでいたことが原因です。この試行もVSS Snapshot開始前・出力作成前で、
完成`.dcimg`と`.partial`はありません。

UIスレッドのCOM apartmentとVSS用の認証`PKT_PRIVACY`・偽装`IDENTIFY`を
アプリ起動直後に確定し、VSS Backendは同じ結果を再利用する安全確認へ変更しました。
起動時初期化、後続のWindows Update COM確認、VSS側再確認の順序を回帰テストへ
追加しました。修正後の通常/静的CRT/ASan各40/40、MSVC静的解析、全境界検査は
PASSです。外部依存と認証強度は変更していません。

## 検証証跡の容量整理

未参照の途中・失敗世代に重複保存されていたVDI 132個、932.61GiBを削除し、
VirtualBoxの未接続媒体登録131件も解除しました。稼働中VMは0台、候補外の子媒体は
0件であることを削除前に確認しています。現用VM、文書参照済み確定PASS証跡、
ログ、JSON、スクリーンショットは保持しました。プロジェクト全体は約1,216GiBから
約284GiBになりました。さらに再生成可能なCMake/Ninjaビルド出力3,504ファイル、
9.62GiBを削除し、最終的にプロジェクト約274GiB、Dドライブ空き約1,477GiBです。

## 最終ポータブルZIP

- パス: `%LOCALAPPDATA%\YTEC\ytec-disk-clone\portable-audit\Y-TEC-Tsumugi-Drive-0.2.0-dev-com-fix-20260803-091555.zip`
- サイズ: 12,971,574バイト
- SHA-256: `AF95C042462399C129A07ABF4949B7428325FA2696F71871D37A4668FF1F637F`
- 15ファイル、`SHA256SUMS.txt`の14件一致、再展開後の全ファイル一致
- Microsoft媒体0件、外部ランタイムDLL 0件、reparse point 0件
- `%USERPROFILE%\Downloads`と指定先`<approved-release-storage>`へ
  同名で非上書き複製し、コピー後SHA-256一致を確認

## 実機へ残す項目

- 実SSD/HDD/NVMeでのGPT/MBRクローン、MBR→GPT、VSS復元、起動修復
- 実USBの作成とBIOS/UEFI/Secure Boot起動
- 接続方式と容量の代表組合せでの速度、残り時間、キャンセル応答
- 電源断相当を含む復旧手順と、正式公開前の法務・署名・版番号判断

これらが終わるまで、本成果物は`0.2.0-dev`の実機受入前候補です。
