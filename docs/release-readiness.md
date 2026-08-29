# Y-TEC Tsumugi Drive 1.0.0 リリースゲート

更新日: 2026-08-28

> **現在は正式リリース不可。** この台帳はv2再設計後の製品経路だけを対象とする。
> 過去のPhaseや旧形式で合格した項目は、再利用部品の履歴証跡であり、このチェックを
> 自動的に完了させない。

## 1. 仕様・安全・旧経路

- [x] 最上位仕様をv2.0へ改訂
- [x] v2安全モデル、アーキテクチャ、要件トレーサビリティを作成
- [ ] 製品UI、CLI、媒体から予約ジョブ入口が0件
- [ ] 旧ジョブファイルを検索・読込・変換・変更・削除しない回帰
- [ ] 正式版UI／CLIから旧イメージ形式を開けない
- [ ] 全破壊操作が対象要約＋大文字`OK`を要求
- [ ] コピー元Writer不在、安定識別、全読戻し、最終commitを全経路で監査
- [ ] 中断情報が1件限定で、予約・一覧・自動実行へ拡張されていない

## 2. 直接クローン

2026-08-11のSATA SSD→USBケース内SATA SSD実機試験では、複製と換装後起動は
成功したが、クローン後のWindows領域一意選択がerror 1168で失敗表示となった。
修正版の同構成再試験までは、以下のWindows直接クローン項目を合格にしない。

- [ ] Windows上でVSS開始時点のシステムディスク直接クローンを完結
- [ ] PEだけでコピー元・コピー先を選び直接クローンを完結
- [ ] Windows／PEの通常モード
- [ ] Windows／PEの縮小移行モード
- [ ] Windows／PEのMBR維持
- [ ] Windows／PEのMBR→GPT
- [ ] Windows／データ専用、GPT／MBR、パーティション選択
- [ ] RAW／既存GPT／MBR／NTFS等の対象を確認後に安全に再構成
- [ ] NTFS／exFAT／FAT32縮小と、未対応FSの元サイズRAW
- [ ] 余剰容量の比例配分／未割当
- [ ] USBメモリ・起動レスキューUSB除外、USB筐体の同一接続識別
- [ ] システムクローン先をoffline保持し「検証完了・換装待ち」と表示

## 3. `.tsumugi` v1

- [ ] v1 Golden fileと後方互換Corpusを封印
- [ ] 通常／縮小／選択／暗号／欠損Mapの全Feature組合せ
- [ ] Argon2id公式Vector、AES-256-GCM、Nonce、鍵消去
- [ ] NTFS／exFAT保存、FAT32分割なし
- [ ] 完全検証（既定）／高速検証
- [ ] `.partial`から既存完成ファイルを保護する回復可能入替え
- [ ] VSS cleanup後だけ完成名へ確定
- [ ] 復元前に同一ファイルハンドルで常時完全検証
- [ ] ディスク全体／個別パーティション復元
- [ ] 稼働中Windows自身への復元拒否とPE案内
- [ ] Fuzz、不正形式、巨大入力、範囲・オーバーフロー、故障注入

## 4. 中断・救出・起動修復

- [ ] 一時停止、再開、安全な取消、取消不能区間
- [ ] 同じSnapshot／Source stateを証明できない再開の拒否
- [ ] 通常モードの読取りエラー停止
- [ ] 救出の前方／逆方向／小ブロック有限再試行
- [ ] 欠損ゼロ埋め、Map、`一部欠損`表示
- [ ] システム救出PE限定、縮小・変換禁止
- [ ] 対象ディスクだけからのBootDiscovery
- [ ] BCD新規再構築、退避、読戻し、ロールバック
- [ ] 必要時のESP／システム領域作成と追加確認
- [ ] 複数Windows、第三者EFIローダー、WinRE
- [ ] このPC向けだけのNVRAM修復

## 5. ADK／レスキューメディア

配布済み候補は試験PCで媒体作成に失敗した。開発PCでは同一ZIP payloadとローカルADKの
読取り専用preflightが合格したが、試験PCのログがなく原因未確定である。現候補は
ADK／WinPE Add-on／必須更新を自動導入しないため、ログ付きの再試験を必須とする。

修正版の現行静的CRT payloadでは、開発PC上で2011 CA／2023 CAのWIM commitとISO生成を
実行し、両manifest、payload Hash、mount解除までPASSした。ISO SHA-256は2011 CA
`54BB8E369A266ADF2E32D577CC3106927D7E96C519E5979CB1820671B7A194C3`、2023 CA
`7B09BEB494B70716978107139D0052738AFFCD03D95D7A2E51FB308280BC3C4F`である。
実USB／PE起動を行っていないため、媒体の実機項目は未合格のままとする。

- [x] Microsoft媒体をRepo／製品ZIPへ同梱しない方針
- [ ] 検証済みADK版とMicrosoft公式URLをリリースへ固定
- [ ] EULA、取得内容、同意前通信0件
- [ ] Authenticode、版、SHA-256、必須更新検証
- [ ] quiet導入、再実行、失敗復旧、offline layout、削除
- [ ] 8GiB以上、4GiB FAT32＋NTFS／exFATデータ領域
- [ ] 検証済み媒体のデータ保持更新
- [ ] 署名済みx64ストレージ／USBドライバー選択
- [ ] PE初期画面の直接操作メニュー
- [ ] BIOS／UEFI、Secure Boot 2011 CA／2023 CA

## 6. UI・保存・診断

- [x] 3～4段階の日本語ウィザード
- [x] 1280×720、1024×600、125～200% DPI
- [x] Tab／Enter／Esc／Focus／読み上げ名
- [ ] 長いモデル名・日本語・エラーでも切れ／重なり／操作不能なし
- [ ] EXE隣`data`だけへ設定・ログを保存
- [ ] 書込み不能時は読取り専用診断だけを許可
- [ ] ログ30日／200MiB、失敗ログ最長90日、Redaction
- [ ] SMART／NVMe、温度、AC／バッテリー
- [ ] 自動スリープ防止と完了後動作
- [ ] 手動更新確認時だけ固定Y-TEC HTTPSへ接続
- [ ] 三本糸アイコンをEXE、PE、Web、PDFへ統一

## 7. 自動試験・VM

2026-08-28の作業ツリーでは、通常／静的MSVC／ASanの各構成で127/127 CTest、
固定seedの有界ImageFuzz 2,000,176入力、coverage-guided ImageFuzz 3,353,448実行、
安全境界、3依存license、SBOM、WinPE／Portable媒体境界をすべてPASSした。
1.0.0-internal-betaの非物理Portable候補も新規生成し、22ファイル、内部Hash、ZIP Hash、
Microsoft payload非同梱を監査した。Golden corpusはPASS済みだが、正式OS VM、
ADK未導入クリーンVM、実媒体は代替しない。2026-08-28には、保存済みの
Windows 10 22H2／Windows 11 25H2基底スナップショットから、NIC・USB・共有
クリップボード・drag and dropを無効にしたリンククローンを各OSで1台ずつ作成した。
両クローンでADKが未導入であること、未導入検出が安全に停止することを確認した。
現行静的CRTテスト実行ファイルをGuest Controlから直接起動した場合は`0xc0000374`を
返したが、`cmd.exe`を親プロセスにした同一のGuest Control経路では、Windows 10／11の
両方でADK取得・管理・同意・検出・取得基盤・媒体・`.tsumugi`サービスの7テスト実行ファイルが
PASSした。従って直接EXE起動はVirtualBox Guest Control固有の互換性制約として回避し、
現在のVM回帰結果は有効とした。基底VM・Kaspersky VM・物理媒体は変更していない。

error 1168修正版の再試験候補は
`Y-TEC-Tsumugi-Drive-0.2.0-dev-candidate-20260811-215115-r1.zip`、
14,015,408 bytes、SHA-256
`34CF54EDDA5C52DD1F67676CF6C93A42E2EF50CB3E60894E59C9642DBD06E218`である。
リポジトリ外の生成元と指定配置先の一致を確認したが、実機再試験前なので正式版ではない。

- [x] 通常／静的MSVC x64のクリーンビルド
- [x] 全CTest
- [x] MSVC静的解析
- [x] AddressSanitizer
- [x] 固定seedの有界ImageFuzz、不正イメージ、合成故障注入
- [x] v1 Golden corpusと長時間coverage-guided Fuzz
- [x] 安全境界、ライセンス、依存Hash、SBOM
- [x] Windows 10 22H2 x64 VM（クリーンリンククローン、ADK未導入、安全停止、対象7テストPASS）
- [N/A] Windows 11 24H2 x64 VM（今回の公開前候補では未検証として互換性を主張せず、将来の回帰対象）
- [x] Windows 11 25H2 x64 VM（クリーンリンククローン、ADK未導入、安全停止、対象7テストPASS）
- [x] ADK未導入クリーンVM（両OSで未導入検出と対象7テストPASS。EULA同意、導入、offline layout、削除は安全ゲート停止中で実行しないことを確認）
- [ ] コピー元・ISOを外した対象単独起動
- [x] VMはNIC無効・1台ずつ・新規合成データのみ（Win10 22H2／Win11 25H2の検証済み範囲）

## 8. 公開GitHubソース

- [x] 実機以外の実装・検証・配布候補監査が完了
- [ ] 変更範囲と既存差分を監査
- [ ] 意図した変更だけをcommit
- [ ] Apache-2.0、NOTICE、第三者通知、SBOM、公開文書の整合を確認
- [ ] 公開GitHub `ytec-forge-commits/ytec-disk-clone`の正しいremote／branchへpush
- [ ] push先URL、commit、実行済み確認を記録

この節は代表実機試験より前のゲートである。GitHub Releasesへ製品ZIPを置かない。

## 9. 代表実機

2026-08-11初回結果: 通常クローンのデータ複製と換装後起動はPASS、製品完了判定は
error 1168でFAIL。縮小移行とPEは未実施、レスキューメディア作成はFAIL。
詳細は`docs/real-hardware-acceptance-checklist.md`を参照する。

- [ ] Windows直接システムクローン
- [ ] 小容量SSDへの縮小移行
- [ ] PE直接クローン
- [ ] データ専用ディスクのイメージ作成・復元
- [ ] USB作成・USB起動
- [ ] 換装後チェック

詳細は`docs/real-hardware-acceptance-checklist.md`を使用する。未試験構成を
過大に正式対応とは表示しない。

## 10. 正式配布

- [x] 0.2.0-dev非物理候補のPortable ZIP、必須ファイル、内部Hash、ZIP Hash
- [x] Portable ZIP、PDF、利用規約、プライバシー
- [x] THIRD-PARTY-NOTICES、SPDX SBOM、依存Hash
- [x] 未署名、Unknown Publisher、SmartScreenの説明（SignPath審査待ち）
- [ ] Visual Studio Community／MSVC利用資格の記録
- [ ] 全Hash一覧を生成後、候補を変更していない
- [ ] 公開ZIPを再ダウンロードし、サイズ・SHA-256一致
- [ ] 名称調査結果と専門家確認ではない限界
- [ ] Y-TECサイト`/ytb/tsumugi-drive/`だけで公開
- [ ] Y-TEC公式バイナリはGitHub Releasesへ置かず、第三者ビルドと公式版を区別
- [ ] Apache-2.0に基づく第三者の改変・再配布権を追加制限しない

全項目が合格し、残る未確認を正式対応範囲から除外するまで`1.0.0`を公開しない。
