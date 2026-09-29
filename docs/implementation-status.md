# Y-TEC Tsumugi Drive v2 実装状況

更新日: 2026-08-28

基準: `DiskClone_Development_Spec.md` v2.0

この文書は1.0.0再設計後の製品経路だけを追跡する。過去のPhaseで合格した
予約ジョブ経路や旧イメージ形式の試験は、安全部品の再利用根拠として保持するが、
現行製品入口の完了には数えない。

## 状態の意味

| 状態 | 意味 |
|---|---|
| 基盤完了 | 共通契約と合成試験があり、製品Adapterから再利用できる |
| 接続中 | 製品入口の一部まで接続済みだが、必須モードまたは異常系が残る |
| 未完了 | 1.0.0の必須要件を満たす製品経路がまだない |
| リリースゲート | 実装に加え、VM／実機／配布監査が必要 |

## 現在のスナップショット

| 領域 | 実装・確認済みの基盤 | 残件 | 状態 |
|---|---|---|---|
| OperationCore | 不変`OperationPlan`、対象再識別、大文字`OK`、実行状態機械、`OperationResult`、1件限定Checkpoint、検証済み安全境界だけで待機する同一プロセス内手動一時停止 | 全製品フローの統一、永続Checkpoint再開、スリープ・電源・ログとの接続 | 基盤完了 |
| 旧経路廃止 | 製品ビルドと公開CLIから予約ジョブ入口を除去。旧ファイルを触らない安全境界と、Windows／PE製品targetが旧形式targetを再リンクしない自動監査を追加 | 隔離した旧形式回帰targetの退役判断、媒体・UI・文書を含む最終監査 | 接続中 |
| GPT／MBRクローン基盤 | 解析・生成、安定識別、対象無効化、bounded I/O、flush・読戻し、最終commit、取消 | v2直接経路での全モード回帰 | 基盤完了 |
| Windows直接クローン | VSS Workflow、複数Snapshot、静的領域Reader、通常モードの直接実行基礎、クローン後USBボリューム公開待ちを`ERROR_NOT_FOUND`限定・最大30秒・再識別／状態／配置再検証付きで行う起動確定 | 修正版の実機再試験、2台だけの直接縮小、MBR維持／MBR→GPT、パーティション選択、余剰配分、全異常系 | 接続中 |
| WinPE直接クローン | PE内でコピー元・コピー先を選ぶ入口、コピー元read-only化、通常モード基礎 | 縮小、変換、選択、媒体候補除外を含む製品VMマトリクス | 接続中 |
| `.tsumugi` v1 | 512バイト有界ヘッダー、マニフェスト、チャンク、Argon2id、AES-GCM、救出欠損Map、通常／縮小／救出の作成・完全検証・全体／個別復元、永続再開境界、Golden corpus、決定論的2,000,176件Fuzz、300秒coverage-guided Fuzz | 正式OS VM、4Kn／実媒体 | リリースゲート |
| 起動修復 | 対象ディスクだけからWindows／GPT・MBR／ESP・Active／WinRE／第三者EFIを読取り専用診断し、複数Windowsの全件優先順または1件選択、Microsoft署名済みBCDBoot `/c`、BCD退避・読戻し・exact rollback、追加確認付きESP／BIOS system領域作成、第三者EFIの保持または専用immutable-manifest削除、明示したこのPCだけのNVRAM条件付き修復、WinRE再登録または通常起動のみ部分結果までWinPE製品UIへ接続。normal／`/analyze /WX`の合成・source-route試験あり | 実VMでのGPT／MBR起動、複数Windows表示順、第三者EFI共存、NVRAM／WinRE、電断・失敗注入、代表実媒体での起動受入 | リリースゲート |
| 救出モード | 有限再試行、前方／逆方向／小ブロック、ゼロ埋め、欠損Map、Windows非systemデータ救出、PEシステム／データ救出、安定再識別、保護媒体除外、縮小・変換禁止、`partial_loss`専用表示、所有一時領域Adapter、Windows非systemデータ／PE救出画像UI、合成failure injection、NIC無効VMでの製品作成・完全検証・復元 | PE救出画像の4Kn条件付き経路、実媒体受入 | 接続中 |
| ADK／WinPE取得 | 導入済み環境の署名・版・更新診断、固定取得物／Hash／署名／版検証、quiet導入、offline layout、削除計画、EULA抽出・同意コントローラーの合成試験 | 完全EULA同意UI、primary pins／無予期再起動ゲート、クリーンVM、製品経路の公式取得・導入・削除 | 接続中 |
| レスキュー媒体 | ローカルADKからのISO／USB作成、安全なUSB再識別、2011／2023 CA基盤、Portable候補の同梱スクリプト／EXEによるローカルADK preflight | ADK自動取得／導入の製品接続、ADK未導入クリーン環境、実機で失敗した媒体作成のログ付き再試験、4GiB FAT32＋データ領域、検証済みUSB更新、ドライバー選択、最新PE直接メニュー | 接続中 |
| UI／PortableData | Win32日本語GUI、LINE Seed JP、左ナビ、共通進捗、対象要約、Windows／PEの主要画面、1024×600～1280×720・125～200% DPI、Tab／Enter／Esc／Focusのhost受入、安全境界連動の一時停止／再開ボタン、直接縮小の準備中／非書込み表示、狭幅用の媒体プロファイル表示 | EXE隣`data`の全経路、ログ循環、手動更新、再起動を越える永続再開、実機固有表示 | 接続中 |
| DeviceHealth | SMART故障予測、NVMe Health Log、温度・閾値の読取り専用取得、異常Target開始禁止、異常Source救出推奨、温度警告のみのUI／再検査、Windows版のバッテリー50%未満確認とAC推奨 | PE版の電源確認、応答parserの追加境界試験、実機対応幅 | 接続中 |
| リリース | 通常／全target静的解析／ASanの全127 CTest、Golden、2種Fuzz、安全境界、3依存license、SBOM、媒体preflight、host UI、Codex Security差分監査、1.0.0-internal-beta Portable候補22ファイルと内外Hash監査 | 正式OS VM、クリーンADK VM、代表実機、Web／法務／署名・公開監査 | リリースゲート |

## 2026-08-28 公開前検証候補監査

- 通常MSVC、`/analyze /WX`、AddressSanitizerの全target buildと各CTest 127/127: PASS
- `.tsumugi` v1 Golden corpus、決定論的ImageFuzz 2,000,176件、coverage-guided Fuzz
  3,353,448 runs／301秒: PASS（crash／ASan artifactなし）
- 安全境界、3依存license、SBOM、製品版、Portable／WinPE媒体境界: PASS
- Codex Security working-tree差分監査: 108 review items、complete coverage、報告対象0件
- UI acceptance buildで「移行後の確認」5項目、操作中排他、完了後再有効化、画面遷移後の
  primary button復元を確認。合成表示であり、実ディスク／実起動の証拠には数えない
- `1.0.0-internal-beta` Portable候補: 22ファイル、ZIP 16,774,966 bytes、SHA-256
  `80A49B5BA7EE2443E1044CFC7FD3341B7BC9B14948699BF0B621E4BFF362066B`、Microsoft payload非同梱、未署名、未公開
- 代表実機、実USB、4Kn、換装後起動、SignPath署名、公開Web監査は未完了

## 2026-08-10 非実機検証スナップショット

- 通常MSVC、MSVC `/analyze /WX`、MSVC AddressSanitizerの全target build: PASS
- 上記3構成の全CTest: 各90/90 PASS
- `ytec-image-fuzz-tests`: 固定seed、最大64 KiB、4,104入力を3構成でPASS。`.tsumugi`、manifest、partition snapshotだけをメモリ上で検査し、受理manifest／snapshotのcanonical再エンコードも確認
- 安全境界、3依存license、SBOM整合性、WinPE／Portable／Phase 3／Phase 4媒体preflight境界: PASS
- 物理ディスク、実USB、VM、UAC、実画面、外部通信はこの検証で使用していない。したがって実I/O、起動、表示品質、実媒体互換の証明には数えない

## 2026-08-11 非物理リリース候補監査

- 通常MSVC、MSVC `/analyze /WX`、MSVC AddressSanitizerの全targetをクリーンbuildし、各CTest 90/90と決定的ImageFuzzをPASS
- 安全境界、3依存license、SBOM、WinPEApp／WinPE製品boot matrix／Portable／Phase 3／Phase 4媒体境界をPASS
- 0.2.0-dev Portable候補をリポジトリ外へ新規生成し、必須15ファイル、内部Hash 14件、ZIP SHA-256、Microsoft payload非同梱をPASS
- Windows／WinPEのUI受入targetを1024×600相当で再表示し、全主要画面、Tab／Enter／Esc／Focus、日本語切れ・重なり・操作不能なしを確認。配布候補の実製品EXEも初期画面まで起動し、処理開始は行っていない
- 監査中に、PowerShellスクリプト1件のUTF-8 BOM欠落、MediaBuilder監査値の陳腐化、通常Windowsの非管理者volume照会が`GENERIC_READ`で拒否される回帰を検出し、最小差分で修正した
- Golden corpus／長時間coverage-guided Fuzz、正式OS VM、ADK未導入クリーンVM、4Kn／実媒体、正式版PDF／Web／法務／公開は未実施で、1.0.0は引き続き正式リリース不可

## 2026-08-11 代表実機フィードバックの反映

- SATA SSDからUSBケース内SATA SSDへの通常クローンは、複製と換装後Windows起動に
  成功した一方、クローン後Windows領域の一意選択がWindows error 1168で失敗表示となった。
  製品としてはFAILであり、修正版の同構成再試験を残す。
- 原因を、コピー先をonlineへ戻した直後のVolume GUID公開待ち競合と判断した。
  `ERROR_NOT_FOUND`だけを最大30秒再試行し、毎回対象の安定再識別、状態、区画配置不変を
  検証する最小差分を追加した。遅延後成功、上限timeout、配置drift、待機失敗、曖昧選択を
  unit testで固定した。実機での原因確定は再試験まで保留する。
- 2台だけの縮小直接クローンは、安全な第三ディスク上のscratch／checkpoint／logを
  証明できないため接続せず、クローン画面を準備中・非書込み表示へ変更した。
  縮小`.tsumugi`の作成／復元とは別経路である。
- レスキューメディア失敗は、開発PCのローカルADKと配布済みZIPの同一payloadでは
  再現しなかった。現候補にADK自動取得／導入はなく、試験PCの正確な失敗ログもないため、
  ADK／WinPE Add-on／必須更新を事前導入する条件を配布文書へ明記し、ログ付き再試験を残す。
- 修正版の現行静的CRT CLI／GUIでも、開発PCの同じADKから2011 CA／2023 CA ISOを
  UAC承認後に新規生成した。両方ともWIM mount／日本語font追加／payload検証／commit／
  ISO生成／manifest照合／mount解除をPASSした。ISO SHA-256は2011 CA
  `54BB8E369A266ADF2E32D577CC3106927D7E96C519E5979CB1820671B7A194C3`、2023 CA
  `7B09BEB494B70716978107139D0052738AFFCD03D95D7A2E51FB308280BC3C4F`である。
  実USB作成／PE起動は行っておらず、試験PC側の失敗原因は引き続き未確定である。
- 1024×600相当・カスタム200%のディスプレイ3でWindows 6画面とWinPE 5画面を再確認し、
  新規文言2件の文字切れを短縮した。Tab／Shift+Tab／矢印／Enter／Esc／Focusを再確認した。
- error 1168修正版の0.2.0-dev候補
  `Y-TEC-Tsumugi-Drive-0.2.0-dev-candidate-20260811-215115-r1.zip`を新規生成した。
  必須15ファイル、内部Hash 14件、Microsoft payload非同梱を専用artifact検査でPASSし、
  14,015,408 bytes、SHA-256
  `34CF54EDDA5C52DD1F67676CF6C93A42E2EF50CB3E60894E59C9642DBD06E218`を生成元と
  `<approved-release-storage>`への配置先で一致確認した。

## `.tsumugi`の現在地

共通形式層では次を実装している。

- 512バイトの有界ヘッダーと版・必須機能検査
- コピー元モデル／シリアル／状態Hash、Geometry、パーティションの相互照合
- 暗号化時のArgon2idとチャンク単位AES-256-GCM
- 救出欠損チャンクごとの前向き／逆向き／セクター試行、native error、ゼロ埋め読戻し証跡。拡張前の証跡なし救出画像は後方互換で読取りのみ許可
- 故障Sourceを再読込せず、読戻し済み救出一時領域を封印し、完全検証済み画像partialの作成後に一時領域を破棄してから完成名を公開する共通オーケストレーター
- 完成パスからだけ導出する隣接一時領域、`CREATE_NEW`、制限DACL、単一link／非reparse／File ID、保存先とSourceの物理分離、一時領域＋最大画像の合計空き容量、read-only再open、正確な所有handleによる破棄、破棄後の画像partial再識別を拘束するWin32 Adapter
- 欠損が選択payload外または区画境界を横断する場合のfail-closedと、欠損ゼロでも救出分類を保持するlossless rescue
- 通常、縮小、個別パーティションの復元計画
- 同じSource session以外のReader混在拒否
- 復元対象の安定識別、範囲・オーバーフロー検査、USBメモリ等の対象外判定
- レイアウトを最後にcommitする復元トランザクション

通常（exact）のディスク全体復元と、既存パーティション／未割当領域への個別復元は、
共通物理サービスからWindows／PE Adapterと製品UIまで接続済みである。既存区画では
選択範囲だけを書いて区画表を変更しない。未割当では同じGPT／MBR形式の1MiB整列候補へ
元区画と同じ容量を書き、全payload読戻し後に新規1entryだけを確定する。GPTはbackup側を
先に公開し、MBRを含むmetadata失敗時は同じロック済みハンドルから取得した元bytesへ
rollbackして読戻す。Windowsを含む区画では、復元後に別確認を必要とする起動修復を案内する。

保持型個別復元はGPT／MBRの合成成功・故障rollbackに加え、Windows既存区画とWinPE未割当領域の
NIC無効VM製品受入まで確認した。救出イメージは欠損・
試行証跡schema、Win32所有ローカル一時領域Adapter、Windows非systemデータ／PEの選択・確認・
進捗・結果表示まで接続し、通常作成との重点回帰とNIC無効VMでの作成・完全検証・全ディスク復元を確認した。
Windows／PE救出画像は実媒体検証が済むまで512バイト論理セクターに限定する。縮小移行、正式OS VM、
実媒体／実機検証は揃っていないため、
現段階を正式版のイメージ作成・復元完成とは表現しない。

## 旧資産の扱い

- 予約ジョブの作成、検索、実行、自動実行、結果取込みを製品仕様に戻さない。
- 旧ジョブファイルは検索、読込、変換、移動、上書き、削除しない。
- 旧イメージ形式は正式版UI／CLIから開かず、公開変換機能を作らない。
- 過去のPhase文書、旧形式文書、VM証跡は「履歴資料」と表示して保持する。
- 過去のPASSは、同じ安全部品をv2経路へ接続した後の回帰試験で再確認する。

## 直近の非実機ゲート

1. 通常／縮小、GPT／MBR、MBR→GPT、Windows／データ専用を正式OS VMの直接経路で再確認する。
2. ADK取得、PortableData、PE電源確認を製品UIへ接続する。
3. ADK未導入クリーンVMで取得・同意・導入・媒体・更新・削除を確認する。
4. 正式OS VMとADK未導入クリーンVMの残る製品マトリクスを確認する。
5. 4Kn、物理ディスク、実USB、実機起動と復元後bootを代表実機で受け入れる。
6. Web／利用規約／プライバシー／法務／署名／版番号を公開直前に監査する。
7. ユーザー確認後、意図した差分だけをcommitし、公開GitHub
   `ytec-forge-commits/ytec-disk-clone`へpushする。開発版ソース公開と正式バイナリ公開を区別する。

## 公開停止条件

次が揃うまで1.0.0を一般公開しない。

- v2直接製品経路の全必須要件
- 全安全境界、静的解析、ASan、Fuzz、ライセンス、SBOM
- 正式OS VM、UIマトリクス、ADKクリーンVM
- 代表実機受入
- Portable ZIP、PDF、Web、利用規約、プライバシー、第三者通知、SHA-256監査

要件単位の状態と合格条件は`docs/requirements-traceability.md`を正本とする。
