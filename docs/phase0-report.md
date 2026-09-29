# Phase 0 実装・検証報告

> **履歴資料:** v2再設計前のPhase報告です。再利用部品の証跡であり、1.0.0の
> 現行製品経路が完成したことを意味しません。
> 本文のprivate remote検討は当時の記録で、現在はApache-2.0の公開GitHubへ
> 移行済みです。

実施日: 2026年7月29日

## 結果

Phase 0 の読み取り専用ディスク診断CLIまで実装した。クローン、バックアップ、復元、VSS、ブート修復、WinPE / ADK操作、書き込み用抽象化は実装していない。

## 実施内容

- C++20 / CMake / MSVC x64 の構成と Ninja プリセット
- `WindowsApp`、`WinPEApp`、`CloneCore`、`DiskModel`、`ImageFormat`、`VssRequester`、`BootRepair`、`MediaBuilder`、`CliTools` の CMake ターゲット
- エラー型、`Result<T>`、標準エラーログ、`HANDLE` RAII
- SetupAPI と照会専用 IOCTL による物理ディスク / パーティション列挙
- JSON schema version 1 と人間向け日本語テキスト出力
- 自作単体テストと注入式モックテスト（外部テスト依存なし）
- THIRD-PARTY-NOTICES、空の依存台帳、SPDX 2.3 SBOM生成 / 一致確認
- ライセンス、安全境界、MSVC x64 ビルド、CTest、MSVC `/analyze` をまとめる `scripts/ci.ps1`
- MSVC AddressSanitizer 用プリセット

## 設計判断

- ディスクハンドルはアクセス権 `0` で開く。`FILE_SHARE_WRITE` は他プロセスの共有を妨げない指定であり、このCLIへ書き込み権限を与えない。
- `IOCTL_DISK_GET_LENGTH_INFO` が非管理者環境で拒否された場合だけ、同じ照会専用ハンドルで `IOCTL_DISK_GET_DRIVE_GEOMETRY_EX` を使用する。
- `WriteFile`、`GENERIC_WRITE`、破壊的 / 状態変更 IOCTL はソースに置かない。
- 完全なシリアルはモデルへ保持せず、制御文字除去後の末尾8文字以内だけを保持・出力する。
- 可変長のデバイス記述子とドライブレイアウトは最大1 MiB、API返却長、オフセット、件数を検証する。
- Phase 0 の空コンポーネントは INTERFACE ライブラリとし、先行機能が実行バイナリへ混入しないようにした。
- 外部 C++ / テスト依存は追加しなかった。ホスト型CIワークフローも、外部checkout actionの承認前には追加しない。
- MSVCランタイムは既定の動的リンクを維持した。配布方式とライセンス確認前に固定・同梱しない。

## 実行した検証

| 検証 | 結果 |
|---|---|
| CMake 3.29.2 / MSVC 19.50.35725.0 x64 configure | 合格 |
| RelWithDebInfo `/W4 /WX /permissive- /sdl /guard:cf` build | 合格 |
| CTest 単体・モック | 2 / 2 合格 |
| Windows PowerShell 5.1 `ci.ps1 -SkipAnalysis` | 合格 |
| MSVC `/analyze` build | 警告・エラーなし |
| MSVC AddressSanitizer build / CTest | 2 / 2 合格 |
| ライセンス台帳検査 | 合格、外部依存0件 |
| Phase 0 書き込みAPI / 禁止成果物検査 | 合格 |
| SBOM再生成一致検査 | 合格 |
| `dumpbin /headers` | x64 を確認 |
| 非管理者で実機JSON診断 | 3台、終了コード0、issues 0、必須項目欠落0、容量未取得0、シリアル8文字超過0 |
| 非管理者で実機テキスト診断 | 終了コード0、読み取り専用表示を確認 |

## 未検証・未実施

- 管理者権限での実機CLI確認
- 別の Windows 10 / 11 実機、SATA HDD / SSD、USBケース、4Kn、MBR / RAW ディスクでの照合
- 実機の表示値とディスク管理・ベンダー情報との全項目比較
- Windows PE でのビルド / 起動 / CRT 依存確認
- Windows 互換性VMラボ（Phase 0 は実ディスク不要テストを優先し、VMを起動していない）
- Git初期化、commit、GitHub private repository作成、push、ホスト型CI
- 配布ZIP、インストーラー、署名、Windows 10 / 11互換性試験

## Phase 1 前の確認事項

1. Phase 1 の破壊的I/O範囲と、合成VMディスクだけを対象にする初期実装・試験計画を人間が承認する。
2. 安定ディスク識別、コピー元読み取り型とコピー先書き込み型の分離、二段階確認、再識別失敗時停止を先に設計レビューする。
3. VMラボが他タスクに利用されていないことを確認し、検証済みスナップショットと合成データだけを使用する。
4. Visual Studio / Build Tools の組織内使用資格、MSVCランタイムの静的 / 動的リンク、再配布条件を確認する。
5. WinPE / ADK は利用者のローカル導入物だけを使い、リポジトリ・配布物へコピーしない手順を確認する。
6. Git / GitHub の初期化・private remote・commit / pushを行うか、ホスト型CIで使うcheckout方式と依存承認を決める。
7. GPT解析・作成とNTFSクラスタ読取を実装する前に、境界値テスト、失敗注入、コピー元書き込み経路なしの検査方法を確定する。
