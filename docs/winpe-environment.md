# WinPE環境検出と導入ゲート

更新日: 2026-08-04

> **v2移行中:** 1.0.0では、ADK／WinPE不足時にMicrosoft公式URL、EULA、
> 取得内容を表示し、利用者の明示同意後だけ固定パッケージを取得する。
> Authenticode署名、版、SHA-256、必須更新を検証してからquiet導入し、
> offline layoutの作成・利用と設定画面からの安全な削除にも対応する。
> 現在は既存の読取り専用診断と取得コントローラー初期基盤までで、取得から
> 導入・削除までの製品UIとクリーンVM回帰は未完了である。

Microsoft製ADK、WinPE、WIM、ISO、CAB、EXE／DLLを製品ZIP、リポジトリ、
Y-TECサイトへ同梱・再配布しない方針は維持する。

## 2026-07-30時点の診断基盤（履歴）

`MediaBuilder`には、利用者のPCへローカル導入されたWindows ADKとWindows PE Add-onを読み取り専用で検出する基盤を追加しています。この節はv2取得フロー実装前の履歴であり、診断CLI自体はダウンロード、インストール、WIMマウント、ISO/USB作成を行いません。別の監査付きスクリプトだけが、診断ゲート通過後にリポジトリ外の複製へ検証ISOを生成できます。Microsoftファイルをリポジトリへコピーしません。

検出対象はamd64に限定し、次を確認します。

- `ProgramFiles(x86)`、`ProgramFiles`、Windows Kits登録情報から作った固定候補
- ADK Deployment ToolsとWindows Preinstallation Environmentのディレクトリ
- `copype.cmd`、`MakeWinPEMedia.cmd`、amd64 `winpe.wim`
- amd64 `dism.exe`、`oscdimg.exe`が通常ファイルかつ非reparseであること
- DISMとOscdimgのMicrosoft署名
- `MakeWinPEMedia.cmd`を1MiB上限で読み取り、`/bootex`スイッチが存在すること
- Windows Installerが報告するDeployment Toolsが`10.1.26100.2454`であること
- Microsoft署名済みDISMがKB5101684適用後の`10.0.26100.8972`であること
- Windows InstallerがOscdimgとDISMのKB5101684パッチを「適用済み」と列挙すること

2026-07-30に固定した検証ポリシーは、ADK `10.1.26100.2454`とMicrosoftが同日時点で最新として案内するKB5101684です。ファイル配置だけで更新済みとは判定せず、Windows Installerの製品バージョン、適用済みパッチ列挙、署名済みDISMの版を組み合わせます。いずれかが不明・不一致・読取り失敗なら`mediaCreationPermitted=false`で安全側に停止します。Microsoftが新しいADKまたは更新を公開した場合は、公式情報を再確認してこの許可リストを更新するまで作成を許可しません。

## 診断CLI

```powershell
./out/build/msvc-x64/src/MediaBuilder/ytec-winpe-environment.exe --text
./out/build/msvc-x64/src/MediaBuilder/ytec-winpe-environment.exe --json
```

終了コードは、すべての作成ゲートを通過した場合が`0`、未導入・不足・更新未確認が`2`、引数誤りが`64`です。このCLIは管理者権限を要求せず、ファイル作成や外部通信を行いません。

2026-07-30の承認後、Microsoft公式のADK `10.1.26100.2454`、対応WinPE Add-on、KB5101684をローカル導入しました。必要なOscdimg/DISMパッチは終了コード`0`、適用外の未導入機能は`1642`として区別し、再起動要求はありませんでした。導入後のホスト診断は`baseLayoutReady=true`、`bootexLayoutReady=true`、`versionAndServicingVerified=true`、`mediaCreationPermitted=true`、終了コード`0`です。ADK、WIM、ISOなどのMicrosoftファイルはリポジトリへコピーしていません。

## 当時の外部依存承認記録（履歴）

実際のWinPE試験には、Microsoft Windows ADKと対応するWindows PE Add-onが新しい外部依存として必要です。

| 項目 | 判断 |
|---|---|
| 理由 | Phase 1のWinPEオフラインクローン、BCDBoot、BIOS/UEFI/Secure Boot起動を正規のMicrosoft環境で検証するため |
| ライセンス | Microsoftインストーラー上の利用条件を利用者が確認・同意し、ローカルPCへ導入する。製品やリポジトリへ再配布しない |
| 採用候補 | 2026-07-30確認時点では、x64 Windows 10/11を対象にするADK 10.1.26100.2454と対応WinPE Add-on、Microsoftが案内する最新ADKパッチ |
| 代替案1 | ADKなしで合成/Windows VM試験を継続できるが、WinPE起動とWinPE内実行は証明できない |
| 代替案2 | 独自PEや別OSは仕様外で、Microsoft標準ブート部品/Secure Boot方針を満たさないため採用しない |
| 禁止案 | 完成済みWinPE、WIM、ISO、ADKツールをリポジトリや配布物へコピーする |

Microsoft公式情報:

- [Windows ADKとWinPE Add-onのダウンロード/対応範囲](https://learn.microsoft.com/en-us/windows-hardware/get-started/adk-install)
- [Windows PE起動メディア作成手順](https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/winpe-create-usb-bootable-drive?view=windows-11)
- [Windows ADK Servicing Updates](https://learn.microsoft.com/en-us/windows-hardware/get-started/adk-servicing)

ADK/WinPE Add-onの導入は、この理由・ライセンス・代替案を提示し、利用者の承認後に実施しました。依存はローカル開発環境だけに置き、リポジトリや製品へ同梱しません。

## 当時の導入・検証順序（履歴）

1. Microsoft公式インストーラーでADK Deployment Toolsと対応WinPE Add-onだけをローカル導入する。（完了）
2. 診断CLIで固定パス、必要ファイル、署名、`/bootex`を再確認する。（完了）
3. バージョンと適用済みADKパッチを検証するゲートを実装する。（完了）
4. リポジトリ外の一時作業ディレクトリだけでamd64 WinPE検証用ISOを生成する。（完了）
5. ISOがADK基本媒体だけから成り、資格情報・実データ・自作ペイロードを含まないことを確認する。（完了）
6. HDD/NICなしの専用VMでLegacy BIOS、UEFI、2011 CA Secure Boot、2023 CA Secure Bootを別々に起動検証する。（完了）
7. 静的ランタイムの自作WinPEAppを基本WinPEと分離した補助ISOから起動し、読み取り専用列挙を確認する。（完了）
8. WinPEAppをWIM複製へ監査可能な手順で追加し、Secure Boot有効VMで自動起動と読み取り専用列挙を確認する。（完了）
9. WinPE内で安定識別と小容量オフラインクローンをVM限定ハーネスで再検証する。（完了）
10. 製品WinPEAppでコピー元/コピー先の読み取り専用プリフライトと通常製品の実行拒否を確認する。（補助ISOで完了、最新WIM統合は未実施）
11. 製品と同じCLI境界へVM専用サービスを注入して起動可能WindowsクローンをWinPE内で実行し、BCDBoot、コピー元不在、Secure Boot有効起動を再検証する。（完了）
12. 最新AppをWIM複製へ再統合し、Secure Boot有効VMで起動する。（管理者操作待ち）
