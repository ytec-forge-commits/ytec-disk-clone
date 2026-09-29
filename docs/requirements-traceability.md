# Y-TEC Tsumugi Drive v2 要件トレーサビリティ

更新日: 2026-08-28

最上位仕様: `DiskClone_Development_Spec.md` v2.0

この表は「既存部品の証跡」と「v2製品経路の完成」を区別する。過去の予約ジョブ、
`.dcimg`、`.dcmig`で得たPASSは再利用部品の根拠であり、v2の直接実行がPASSした
ことを意味しない。

## 1. 状態の意味

| 状態 | 意味 |
|---|---|
| 再利用 | 既存の安全部品と試験証跡をv2へ取り込める |
| 要拡張 | 基盤はあるがv2要件の追加が必要 |
| 要置換 | 現行製品経路がv2と矛盾し、入口・状態管理を置換する |
| 新規 | v2で新たに実装する |
| リリースゲート | 実装だけでなく所定のVM／実機／配布監査が必要 |

## 2. 要件マトリクス

| ID | 要件 | 主な境界 | 基線状態 | 必須検証 |
|---|---|---|---|---|
| EXE-001 | `OperationPlan → 再識別 → OK → 実行 → 検証 → Result` | OperationCore | 新規 | 状態遷移、例外、取消、commit合成試験 |
| EXE-002 | 予約ジョブ作成・一覧・自動実行・結果取込みを廃止 | WindowsApp／WinPEApp | 要置換 | 製品UI到達不能、旧コード参照0件 |
| EXE-003 | 公開`--job-*`を削除 | WinPEApp CLI | 要置換 | `--help`、引数拒否、媒体内バイナリ確認 |
| EXE-004 | 旧ジョブファイルを検索・読込・変換・変更・削除しない | 全製品入口 | 新規ゲート | 静的検索、監視フォルダー試験、ファイルHash不変 |
| EXE-005 | 1件だけの中断再開／破棄 | OperationCheckpoint | 新規 | 同一性、差替え、Snapshot失効、所有外ファイル保護 |
| SAFE-001 | コピー元ReaderとターゲットWriterの型分離 | DiskModel／CloneCore | 再利用 | 安全境界スクリプト、コードレビュー |
| SAFE-002 | 安定識別を実行直前・状態変更後に再検証 | DiskModel | 再利用・要拡張 | 番号交換、同型番、切断再接続、空シリアル |
| SAFE-003 | 全破壊操作を対象要約＋大文字`OK`で承認 | 共通UI／OperationCore | 要拡張 | クローン、復元、USB、起動修復のUI試験 |
| SAFE-004 | 全書込みをflush・読戻し検証 | CloneCore／MigrationEngine | 再利用 | 短いI/O、故障注入、読戻し不一致 |
| SAFE-005 | 未完了対象を完成状態へ戻さない | OperationCore | 再利用・要拡張 | 取消、例外、電断相当、commit前後 |
| SAFE-006 | Windows版とPE版で元ディスク無変更の意味を区別 | UI／ログ／Source Provider | 要拡張 | 文言、ハンドル監査、VM原本Hash |
| SAFE-007 | 自動スリープ防止、電源・バッテリー警告 | DeviceHealth／UI | 新規 | AC有無、49／50%、完了時解除 |
| SAFE-008 | 温度は表示・警告のみ、ターゲット健康異常は開始禁止 | DeviceHealth | 新規 | SMART／NVMeモック、取得不能表示 |
| CLN-001 | Windows上でシステムディスク直接クローン完結 | WindowsApp／VSS／CloneCore | 要置換 | VMでSnapshot時刻、原本Hash、対象単独起動 |
| CLN-002 | PEだけで直接クローン完結 | WinPEApp／OfflineSource | 要置換 | ジョブなしGPT／MBR／縮小／MBR→GPT |
| CLN-003 | RAW以外の既存基本ディスクもコピー先候補 | UI／Target Writer | 再利用・要拡張 | RAW、GPT、MBR、NTFS、exFAT、差替え |
| CLN-004 | 通常／縮小を自動推奨し手動上書き可能 | MigrationCore／UI | 再利用・要拡張 | 容量境界、推奨、禁止組合せ |
| CLN-005 | パーティション選択、Windows必須領域の強制選択 | Planning／UI | 新規 | Windows／データ専用／混在ディスク |
| CLN-006 | NTFS／exFAT／FAT32縮小、未対応FSは元サイズRAW | MigrationCore／Engine | 要拡張 | 混在FS、最小容量、Hash一致 |
| CLN-007 | 余剰を比例配分または未割当 | Layout Planner | 新規 | 最小値、丸め、複数区画、オーバーフロー |
| CLN-008 | MBR維持／MBR→GPTをWindows・PEの両方で実行 | MigrationEngine／BootRepair | 要拡張 | BIOS、UEFI、Secure Boot、WinRE |
| CLN-009 | 完了システムクローン先をoffline保持 | Target Writer／UI | 再利用・要拡張 | 再列挙、表示「検証完了・換装待ち」 |
| CLN-010 | USBメモリ除外、USB筐体の同一接続識別 | DiskModel | 新規 | 切断、ポート変更、シリアルなし、USBメモリ |
| IMG-001 | 単一`.tsumugi` v1 | ImageFormat | 新規 | Golden file、後方互換、不正入力 |
| IMG-002 | 通常／縮小／選択／暗号／欠損Mapを統合 | ImageFormat | 新規 | 全Feature組合せ、未知必須Flag |
| IMG-003 | `.dcimg`／`.dcmig`を正式版UIから開かない | WindowsApp／WinPEApp | 要置換 | ファイル選択Filter、CLI、媒体監査 |
| IMG-004 | NTFS／exFAT上の単一ファイル、FAT32分割なし | ImageFormat／UI | 新規 | 保存先FS判定、4GiB超、容量不足 |
| IMG-005 | Argon2id＋AES-256-GCM | CryptoProvider | 新規 | 公式Vector、Tag改ざん、Nonce、鍵消去 |
| IMG-006 | `.partial`完全検証後の回復可能置換 | File Backend | 再利用・要拡張 | 既存保持、同名、電断相当、空き容量 |
| IMG-007 | 作成は完全既定／高速任意、復元は常時完全検証 | ImageFormat／UI | 要拡張 | モード差、同一ハンドル、差替え |
| IMG-008 | ディスク全体／個別パーティション復元 | Restore Planner | Windows／PE製品VM受入済み | 全ディスク復元、Windows既存区画、WinPE未割当へのGPT 1entry追加、範囲外区画保持、選択ドリフトのI/O前拒否をNIC無効VMでPASS。MBR未割当VM、4Kn／実媒体は未完了 |
| IMG-009 | 稼働中システムディスク復元を拒否しPE再選択案内 | WindowsApp | 再利用・要置換 | ジョブ生成0件、対象不変 |
| IMG-010 | 同一物理ディスク保存はWindows詳細設定・対象外パーティションだけ | Image Writer／UI | 新規 | 全体選択拒否、PE拒否、保存先変更、警告 |
| RSC-001 | 標準読取りエラーは停止 | Source Provider | 再利用 | 故障注入、未commit |
| RSC-002 | 救出モードの有限前後・小ブロック再試行 | RescueEngine | Windows／PE合成・製品VM接続 | 再試行上限、進捗、取消、所有一時領域の書込み読戻し・封印・破棄を合成PASS。Windows／PEのlossless救出作成・復元をNIC無効VMでPASS。故障媒体／実媒体は未完了 |
| RSC-003 | 欠損をゼロ埋め＋Map化し「一部欠損」とする | RescueEngine／Result | Windows／PE合成・製品VM接続 | Bad range、試行証跡、専用結果、単一`.tsumugi`確定を合成PASS。lossless救出分類保持をVMでPASS。実故障媒体は未完了 |
| RSC-004 | システム救出はPE限定、縮小・変換禁止 | Planning | Windows／PE合成・製品VM接続 | Windowsのsystem拒否・事前保護済みdata限定、Windows／PEの512バイト論理セクター限定、Source最終再読込なしを合成PASS。PE救出作成・全ディスク／個別復元をVMでPASS。4Kn／実媒体は未完了 |
| BCD-001 | ディスク選択だけでWindows／ESP／方式を自動診断 | BootDiscovery | WinPE製品非物理接続済み | GPT／MBR、全Windows優先順または1件選択、候補不明拒否を合成PASS。VM／実媒体を残す |
| BCD-002 | BCD新規再構築の退避／ロールバック | BootRepair | WinPE製品非物理接続済み | UEFI／BIOS、固定BCDBoot `/c`、退避・読戻し・exact rollbackを合成PASS。実起動を残す |
| BCD-003 | ESP／システム領域の安全な新規作成 | BootRepair Planner／VDS Adapter | WinPE製品非物理接続済み | GPT 260 MiB ESP／MBR 100 MiB Active、追加確認、完全再識別、縮小失敗分類、raw cleanup、rollbackを合成PASS。実NTFS縮小を残す |
| BCD-004 | 第三者EFIローダー保持／削除選択 | BootDiscovery／UI／EfiDeleteTransaction | WinPE製品非物理接続済み | 保持既定、専用削除確認、immutable manifest、quarantine／rollback、未知EFI拒否を合成PASS。実ESP共存を残す |
| BCD-005 | このPC向けだけNVRAM修復 | BootRepair／NvramRepair | WinPE製品非物理接続済み | 他PCは不変、このPCの明示時だけexact ESPへ条件付き更新、BootOrder保持、rollbackを合成PASS。実firmwareを残す |
| BCD-006 | WinRE特定不能時は部分修復 | BootRepair／WinReRegistration | WinPE製品非物理接続済み | 署名済み診断、候補Hash・lock、再登録・再診断・rollback、特定不能時の通常起動のみ部分結果を合成PASS。実WinRE起動を残す |
| MED-001 | Microsoft媒体を製品・Repo・Webへ同梱しない | Packaging／MediaBuilder | 再利用 | Repo／ZIP／公開物監査 |
| MED-002 | 同意後に固定Microsoft公式URLから自動取得 | AdkAcquisition | 新規 | 同意なし通信0、URL、TLS、Redirect |
| MED-003 | Authenticode／版／SHA-256を検証 | AdkAcquisition | 要拡張 | 改ざん、版違い、署名者違い |
| MED-004 | quiet導入、Servicing、検証、アンインストール | AdkAcquisition | 新規 | ADKなしクリーンVM、再実行、失敗復旧 |
| MED-005 | 公式offline layout作成／利用 | AdkAcquisition | 新規 | オフラインVM、保持、Hash検査 |
| MED-006 | 4GiB FAT32＋残りNTFS／exFAT | MediaBuilder | 要置換 | 8／16GiB、BIOS／UEFI、USB Hash |
| MED-007 | 検証済み既存媒体をデータ保持更新 | MediaBuilder | 新規 | データ前後Hash、未知媒体拒否 |
| MED-008 | 起動USB全体を他の書込み先から除外 | DiskModel／PE | 新規 | Clone／Restore／Rescue候補0件 |
| MED-009 | 現PCの署名済みx64ストレージ／USBドライバーを収集・一覧表示し、他PC用メーカーINFを任意指定 | MediaBuilder／WindowsApp | 製品モジュール接続中 | PnP／DriverStoreと任意フォルダーをread-only列挙し、通常ファイル・非reparse・amd64・INFカタログ署名・パッケージ全体SHA-256をfail-closed評価。明示選択だけのimmutable DISM計画を合成PASS。Windows UIと既存WIMマウントtransactionへの実行接続を残す |
| UI-001 | 3～4段階ウィザード、日本語UI | WindowsApp／WinPEApp | host／製品VM受入済み | Windows 6画面、WinPE 5画面、救出・復元製品VMをPASS。残る全異常文言と実機固有表示を確認する |
| UI-002 | 1280×720、1024×600、125～200% DPI | UiSupport | host受入済み | Windows／WinPEの1024×600～1280×720、100～200%をPASS。実WinPE端末固有表示は実機で確認する |
| UI-003 | Tab／Enter／Esc／見えるFocus | UiSupport | host受入済み | Windows／WinPEでTab focus、Enter navigation、Esc終了をPASS。破壊操作のキーボード完走は実媒体前に再確認する |
| UI-004 | 短い日本語エラー＋Code＋次手＋詳細Copy | 共通Error UI | 要拡張 | 長いPowerShell／HRESULT／Stack抑制 |
| DAT-001 | EXE隣`data`だけへ設定・ログ保存 | PortableData | 要置換 | AppData書込み0、書込み不能時read-only |
| DAT-002 | ログ循環、秘密値・Path最小化 | PortableData | 要拡張 | 30日／200MiB／90日、Redaction |
| NET-001 | ADK取得と手動更新確認以外の通信なし | Network Adapter | 新規ゲート | API静的検査、通信キャプチャ |
| NET-002 | 更新確認は手動、固定Y-TEC HTTPS、表示のみ | UpdateCheck | 新規 | 起動時通信0、不正JSON、巨大応答 |
| LIC-001 | Zstd／LINE Seed／Argon2を固定しSBOM・通知一致 | Licensing | 要拡張 | Hash、版、License、配布内容 |
| LIC-002 | Visual Studio／MSVC使用資格を記録 | Release | リリースゲート | 使用資格、Toolchain、静的CRT証跡 |
| REL-001 | Win10 22H2＋Win11 25H2 x64 VM回帰 | Validation | リリースゲート | NICなし、直列、合成データ。24H2は将来の未検証回帰対象、26H1 Arm64は非対応 |
| REL-002 | 代表実機受入まで正式公開しない | Validation | リリースゲート | Clone／Shrink／PE／Image／USB |
| REL-003 | 開発版ソースと非物理証跡を公開GitHubへpush | Git／Release | リリースゲート | Apache-2.0、Clean tree、commit、remote確認、正式版との区別 |
| REL-004 | Y-TEC公式Portable ZIPは未署名、Y-TECサイトだけで公開 | Packaging／Web | リリースゲート | SHA-256、再DL一致、SmartScreen説明、第三者ビルドとの区別 |
| REL-005 | 同名v1 DOCXをv2へ再生成するまで正式配布しない | Documentation | リリースゲート | DOCX/PDF/Web/アプリ内文言の整合・表示確認 |

## 3. 現行基線から再利用できる証跡

2026-08-03時点で次の部品はVMまたは合成試験済みであり、v2へ移植後に再回帰する。

- GPT／MBR解析・生成、安定識別、対象Writer、全書込み読戻し、最終commit
- Windows SDK VSS Workflow、Writer監査、Snapshot Reader／Bitmap、cleanup
- 通常Zstandardチャンクと縮小WIMの作成・復元部品
- GPT／MBRクローン、VSS復元、MBR→GPTの対象単独起動証跡
- Microsoft署名済みBCDBootとBCD新規再構築トランザクション
- ADK／WinPE署名・版・更新診断、2011／2023 CA媒体生成
- LINE Seed JP埋込み、WinPE日本語GUI、USB安定識別と自動初期化部品

以下はv2で新たに合格させるまで未完了とする。

- Windows上のシステムディスク直接クローン完結
- ジョブなしのWinPE直接クローン／イメージ作成／復元
- `.tsumugi` v1のGolden／Fuzzは2026-08-28にPASS。正式OS VM／実媒体検証は継続
- 1件の中断再開、救出モード、自動BootDiscovery
- ADK公式取得／quiet導入／offline layout／アンインストール
- EXE隣`data`への完全移行と手動更新確認

## 4. リリース停止条件への対応

次を全て満たすまで`1.0.0`を正式公開しない。

- `EXE-002`～`EXE-004`が合格し、旧ジョブファイルが不変である。
- `CLN-001`と`CLN-002`が新しい直接製品経路で合格する。
- `IMG-001`～`IMG-009`の形式・暗号・復元安全試験が合格する。
- `MED-001`～`MED-008`がADK未導入クリーンVMと媒体試験で合格する。
- 全安全境界、静的解析、ASan、Fuzz、ライセンス、SBOM、Codex Security差分監査は
  2026-08-28に合格。以後の差分では再実行する。
- 正式OS VMマトリクス、UIマトリクス、代表実機受入が合格する。
- Portable ZIPと公開ページのSHA-256が一致する。
