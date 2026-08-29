# Y-TEC Tsumugi Drive

[English](README.en.md) | 日本語

Windows上とWinPE上の両方で、ディスクのクローン、イメージ作成・復元、
起動修復を安全側に行うWindows x64ネイティブアプリです。

> **現在は`1.0.0-internal-beta`という開発識別子の公開前検証候補です。正式版1.0.0ではありません。**
> 代表実機受入と正式配布監査は未完了です。
> 重要データ、業務端末、唯一のバックアップには使用しないでください。

本プロジェクトは会社・勤務先の社内ソフトではなく、Y-TEC名義で個人開発・公開する
オープンソースソフトウェアです。`internal-beta`は現時点の開発識別子であり、
企業内利用や社内プロジェクトであることを意味しません。

最上位仕様は[開発仕様書 v2.0](DiskClone_Development_Spec.md)です。過去のPhase文書や
旧形式の試験結果は、安全部品を再利用するための履歴証跡であり、1.0.0の製品入口が
完成したことを意味しません。
同名の`DiskClone_Development_Spec.docx`は現行v2.0の閲覧用文書です。旧v1.0の履歴
snapshotは`docs/archive/DiskClone_Development_Spec-v1.0-20260729.docx`に保存しています。

## 1.0.0の完成像

- Windows版で、VSS Snapshotを使った稼働中Windowsの直接クローン
- WinPE版だけで完結する直接クローン、イメージ作成・復元、起動修復
- 通常モード、縮小移行モード、MBR維持、MBR→GPT
- Windowsディスクとデータ専用ディスクの両方を対象にした単一`.tsumugi` v1
- 対象ディスクだけを選ぶ通常操作からのWindows／ESP／BIOS・UEFI自動診断
- Microsoft公式ADK／WinPEの同意付き取得、検証、導入、USB／ISO作成
- 1件限定の中断再開と、欠損範囲を明示する救出モード

Windows版とWinPE版はいずれも、選択から実行・検証までを同じセッション内で
完結させます。予約ジョブ、固定名ジョブ、自動実行、結果取込み、公開`--job-*`
は正式版で使用しません。旧ジョブファイルは検索・読込・変換・変更・削除せず、
利用者の既存ファイルとしてそのまま残します。

正式イメージ形式は`.tsumugi` v1です。旧イメージ形式は正式版UI／CLIから開かず、
公開変換機能も提供しません。

## 現在の実装状況

2026-08-20時点では、次の基盤を実装・合成試験しています。

- `OperationPlan → 再識別 → OK → 実行 → 検証 → OperationResult`の共通契約
- 1件限定の`OperationCheckpoint`と再開時の同一性検査
- GPT／MBR解析・生成、安定識別、対象Writer、flush・読戻し、最終commit
- Windows VSS Workflow、Writer監査、Snapshot Reader／Bitmap、cleanup
- Windows／WinPEの直接クローン入口の基礎と、通常モードの一部接続
- `.tsumugi` v1の有界形式、Argon2id、AES-256-GCM、マニフェスト、作成・検証・
  復元計画、同一ハンドル読戻し復元トランザクション
- Microsoft署名済みBCDBootによるBCD新規再構築トランザクション
- WinPEで対象ディスクだけを選ぶGPT／UEFI・MBR／BIOS起動修復。GPT／UEFIは
  Windowsと1つの既存ESPを一意に特定でき、EFI内容がMicrosoft所有と確認できる
  安全な構成に限り直接実行し、それ以外は開始しない経路
- ADK／WinPEの署名・版・更新診断と、同意付き取得コントローラーの初期基盤
- 救出コピーの有限再試行・欠損範囲記録、所有一時領域、Windows非systemデータ／WinPE救出イメージUIの合成接続

次は未完了であり、一般製品機能としての完了を表明しません。

- Windows／WinPEの通常・縮小・MBR維持・MBR→GPTを通した全製品経路
- `.tsumugi`のWindows／WinPE実ディスクAdapterと作成・復元UIの完全接続
- 個別パーティション復元、ファイル単位縮小、512系↔4Knの条件付き経路
- 複数Windows／曖昧なESPの選択、ESP新設、第三者EFIローダー保持／削除選択、
  限定NVRAM修復、WinRE修復
- ADKの自動取得からquiet導入、offline layout、アンインストールまでの一連のUI
- Windows／PE救出イメージの実画面・VM／実媒体、4Kn条件付き経路、スリープ防止、電源、ログ循環、手動更新確認
- 正式OS VMマトリクス、全DPI／キーボード試験、代表実機受入、配布候補監査

詳細は[実装状況](docs/implementation-status.md)と
[要件トレーサビリティ](docs/requirements-traceability.md)を参照してください。

## 安全モデル

破壊的操作は次の順序に統一します。

```text
OperationPlan
  -> 読取り専用プリフライト
  -> コピー元／対象セッションを開く
  -> 安定識別を再検証
  -> 対象要約と大文字 OK
  -> 対象を未完了状態にする
  -> 実行
  -> flush・読戻し検証
  -> 完成メタデータを最後に確定
  -> OperationResult
```

- コピー元Readerとコピー先Writerを型で分離します。
- ディスク番号やドライブ文字だけで対象を決めません。
- モデル、容量、接続方式、シリアル末尾、パーティション図、消去内容を表示し、
  大文字`OK`の完全一致だけで破壊操作を承認します。
- Windows版はアプリ自身がコピー元へ書き込みませんが、稼働中WindowsとVSSの
  通常書込みは発生します。物理的な完全無変更が必要な場合はWinPEを使います。
- WinPE版はコピー元をOSレベルでも読取り専用に固定します。
- 失敗、中止、欠損、未検証を成功や起動確認済みとして表示しません。

## イメージ形式

`.tsumugi` v1は通常、縮小、パーティション選択、任意暗号化、救出時の欠損Mapを
1つのファイルに格納します。正式保存先はNTFSまたはexFATです。FAT32向け4GiB
分割は行いません。

- 暗号化: Argon2id（64MiB、3反復、並列度1）＋Windows CNG AES-256-GCM
- 圧縮: Zstandard 1.5.7。小さくならないチャンクは非圧縮へ戻す
- 作成: 隣接`.partial`へ書込み、検証後に回復可能な入替え
- 復元: 書込み／削除共有なしの同一ファイルハンドルで完全検証後に対象を変更
- 未知の必須機能、範囲外、重複、認証・Hash不一致は書込み前に拒否

形式の現在地は[イメージ形式](docs/image-format.md)を参照してください。

## レスキューメディア

製品ZIPへADK、WinPE、WIM、ISO、CAB、Microsoft製EXE／DLLを同梱しません。
正式版では、検証済みの固定Microsoft公式URL、取得内容、EULAを表示し、利用者が
同意した場合だけ取得と導入を行います。署名、版、SHA-256、必要更新の検証に
失敗した場合は媒体を作成しません。

レスキューUSBは8GiB以上、16GiB推奨です。既定構成は4GiB FAT32起動領域と
残容量のNTFSデータ領域で、詳細設定からexFATを選べます。初回作成や未知USBの
初期化は対象全体を消去するため、対象要約と大文字`OK`を要求します。

## 必要環境

- Windows 10 22H2 x64、またはWindows 11 x64の開発・検証環境
- CMake 3.25以上
- MSVC x64 C++ Build Tools
- Ninja
- PowerShell 7またはWindows PowerShell 5.1

ビルド前に同じPowerShellでMSVC環境を初期化します。

```powershell
. .\scripts\Enter-MsvcEnvironment.ps1
cmake --preset msvc-x64
cmake --build --preset msvc-x64 -- -j 2
ctest --preset msvc-x64 --output-on-failure
```

全ゲートは次で実行します。

```powershell
./scripts/ci.ps1
```

CIにはビルド、CTest、ライセンス／安全境界／SBOM検査、MSVC静的解析、
AddressSanitizerを含みます。成功した過去のCIは、後続変更の合格を意味しません。

## 読取り専用診断CLI

```powershell
./out/build/msvc-x64/src/CliTools/ytec-disk-inventory.exe --text
./out/build/msvc-x64/src/CliTools/ytec-disk-inventory.exe --json

./out/build/msvc-x64/src/MediaBuilder/ytec-winpe-environment.exe --text
./out/build/msvc-x64/src/MediaBuilder/ytec-winpe-environment.exe --json
```

正式版CLIは読取り専用診断と開発試験に限定し、予約ジョブの入口を公開しません。

## 検証と公開

- 破壊的検証はNIC無効の専用VMと新規合成ディスクだけで行います。
- VMは1台ずつ使用し、ホスト物理ディスクや実USBを対象にしません。
- ソースコードは公開GitHubリポジトリで管理し、Apache License 2.0で提供します。
- ソース公開、CIの成功、過去の試験結果は、正式バイナリの安全性や実機対応を
  保証するものではありません。
- 代表実機受入と公開監査が完了するまで`1.0.0`を一般公開しません。
- Y-TECが提供する公式ビルドは、監査済みPortable ZIPだけをY-TEC公式ページから
  配布する方針です。現時点ではGitHub Releasesへ公式バイナリを掲載しません。
- Apache-2.0と第三者ライセンスの条件を守る限り、ソースと派生物の利用、変更、
  再配布が可能です。ただし、第三者ビルドはY-TEC公式版ではなく、Y-TECによる
  動作保証やサポートの対象ではありません。

## オープンソースと参加

Y-TECが著作権を持つソースコードは[Apache License 2.0](LICENSE)で公開します。
同梱するフォントとライブラリには個別のライセンスが適用されるため、
[第三者通知](THIRD-PARTY-NOTICES.txt)、[ライセンス一覧](licenses/README.md)、
[SPDX SBOM](SBOM.spdx.json)も確認してください。Microsoft製WinPE／ADK／WIM／
ISO／EXE／DLLは、このリポジトリや製品ZIPへ同梱しません。

- 変更を提案する場合は[CONTRIBUTING.md](CONTRIBUTING.md)を確認してください。
- 脆弱性の可能性は[SECURITY.md](SECURITY.md)に従って報告してください。
- 利用相談とサポート範囲は[SUPPORT.md](SUPPORT.md)を確認してください。
- 参加時は[行動規範](CODE_OF_CONDUCT.md)を守り、
  [maintainer方針](MAINTAINERS.md)と[商標・公式版の区別](TRADEMARKS.md)も
  確認してください。
- 公開Issue／Pull Requestへ、実ディスクイメージ、完全なシリアル、回復キー、
  パスワード、認証情報、個人情報を投稿しないでください。
- 自動試験と通常のPull Requestでは、合成入力と非物理テストだけを使用します。

## 現行文書

- [開発仕様書 v2.0](DiskClone_Development_Spec.md)
- [安全モデル](docs/safety-model.md)
- [アーキテクチャ](docs/architecture.md)
- [類似オープンソース調査と採用判断](docs/open-source-landscape.md)
- [要件トレーサビリティ](docs/requirements-traceability.md)
- [実装状況](docs/implementation-status.md)
- [テスト計画](docs/test-plan.md)
- [イメージ形式](docs/image-format.md)
- [縮小移行モード](docs/shrink-migration-mode.md)
- [WinPE環境とADK取得](docs/winpe-environment.md)
- [実機受入チェックリスト](docs/real-hardware-acceptance-checklist.md)
- [リリースゲート](docs/release-readiness.md)

過去のPhase、旧形式、引継ぎ、検証サマリーは履歴資料です。最上位仕様と矛盾する
記述は現行製品の案内として使用しません。
