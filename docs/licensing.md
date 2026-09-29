# Y-TEC Tsumugi Drive v2 ライセンス管理

更新日: 2026-08-20

対象: Y-TEC Tsumugi Drive 1.0.0

本書は法的助言ではなく、実装・配布時の監査条件を定める。正式公開前に、
実際に使用したVisual Studio／ADK／WinPEの条項、組織の使用資格、配布地域、
製品利用規約を人間が再確認する。

## 1. 製品ライセンス方針

- Y-TECが著作権を持つソース、文書、ビルド成果物はApache License 2.0で
  提供する。個人・法人、商用・非商用を問わず、同ライセンス条件に従う利用、
  改変、再配布を許可する。
- リポジトリの`LICENSE`を製品ライセンスの正本とし、`NOTICE`、
  `THIRD-PARTY-NOTICES.txt`、`licenses/`の帰属と第三者条件を維持する。
- Apache-2.0はY-TECおよびTsumugi Driveの商標使用権や、第三者ビルドを
  Y-TEC公式版と表示する権利を付与しない。詳細は`TRADEMARKS.md`に定める。
- Y-TECが品質保証する公式バイナリの公開先はY-TEC公式ページに限定し、当面
  GitHub Releasesへ公式バイナリを置かない。これはApache-2.0に基づく第三者の
  再配布権を制限せず、非公式ビルドはY-TECのサポート・保証対象外とする。
- Portable ZIPだけを提供し、インストーラーは提供しない。
- 1.0.0はコード署名なしで公開するため、Unknown Publisher／SmartScreen、公式SHA-256確認方法を明記する。

利用規約、免責、サポート条件、公式版の識別方法は正式バイナリ公開前の
リリース停止項目である。利用規約や商標方針によってApache-2.0の権利を
追加制限しない。

## 2. 承認済み第三者依存

| 名称 | 固定版 | 公式配布元 | ライセンス | 製品内用途・形態 | 承認 |
|---|---|---|---|---|---|
| Zstandard | 1.5.7 | <https://github.com/facebook/zstd/releases/tag/v1.5.7> | BSD-3-Clause | `.tsumugi`チャンク圧縮、必要ソースだけを静的リンク | 承認済み |
| LINE Seed JP | LINESeedJP_20241105 | <https://seed.line.me/index_jp.html> | OFL-1.1 | 未改変App TTF Regular／BoldをGUIへ埋込み | 2026-08-01承認 |
| Argon2 reference implementation | 20190702 | <https://github.com/P-H-C/phc-winner-argon2/tree/20190702> | Apache-2.0 | Argon2idに必要なportable reference sourceだけを静的リンク | 2026-08-04承認 |

正確な配布元、ソースアーカイブSHA-256、ライセンスファイルは
`third_party/dependencies.json`を正本とする。`THIRD-PARTY-NOTICES.txt`、
`licenses/`、`SBOM.spdx.json`、実バイナリの依存内容を同時に一致させる。

### 2.1 Zstandardの収録範囲

- 圧縮／展開ライブラリだけを静的リンクする。
- CLI、DLL、辞書Builder、legacy decoder、tests、contribを配布しない。
- 辞書、legacy、連結frameを許可しない製品profileへ固定する。

### 2.2 LINE Seed JPの収録範囲

- 未改変のApp TTF Regular／BoldだけをWindows／WinPE GUIへ埋め込む。
- `AddFontMemResourceEx`でプロセス内だけに読み、OSへインストールしない。
- 読込み失敗時は両Weightを解除し、`Yu Gothic UI`へフォールバックする。
- OFL本文と著作権表示をPortable ZIPとWinPEへ含める。

### 2.3 Argon2の収録範囲

- 公式tag`20190702`のportable reference sourceからArgon2idに必要な部分だけを使用する。
- CLI、benchmark、test program、architecture固有最適化、threaded backendを配布しない。
- `.tsumugi` v1はparallelism 1を使用する。
- Apache-2.0本文、著作権表示、改変の有無を通知へ記録する。
- Argon2はパスワード回復・解析には使用せず、利用者が指定したイメージ暗号化のKDFだけに使用する。

## 3. 依存追加ゲート

新規依存は実装前に次を提示し、人間の明示承認を得る。

- 名称、固定版、公式配布元、取得物SHA-256
- SPDXライセンス識別子、本文、NOTICE／著作権要件
- 用途、静的／動的リンク、配布物への含有
- 必要性、代替案、更新・撤回方法
- 既存形式・セキュリティ・WinPEサイズへの影響

承認後に次を同じ変更で更新する。

- `third_party/dependencies.json`
- `licenses/`
- `THIRD-PARTY-NOTICES.txt`
- `SBOM.spdx.json`
- 依存取得／Hash検査とライセンスCI

製品へGPL、AGPL、SSPL、LGPL、MPL、EPL、Commons Clause、Business Source
License、独自非商用、ライセンス不明の依存を組み込まない。

## 4. Microsoft ADK／WinPE

### 4.1 非同梱

次をGitリポジトリ、Portable ZIP、Y-TECサーバー、GitHub Releasesへ含めない。

- Windows ADK／WinPE Add-onインストーラー
- WinPE ISO、`boot.wim`、`winpe.wim`、`winre.wim`
- ADK由来のEXE、DLL、CAB、CMD、フォント、USBイメージ
- `copype.cmd`、`MakeWinPEMedia.cmd`、`oscdimg.exe`のコピー
- 本製品が生成した完成WinPE ISO／USBイメージ

利用者端末で正規に導入されたADK／WinPEを、その端末内で署名・版確認して使用する。
生成したUSB／ISOは利用者自身のローカル成果物である。

### 4.2 承認済みの公式取得例外

従来の「アプリから自動取得しない」方針はv2で変更し、次を満たす場合だけ
Microsoft公式ADK取得を許可する。

1. 製品リリースごとに、試験済みADK、WinPE Add-on、Servicing UpdateのMicrosoft公式HTTPS URL、版、SHA-256、署名者を固定する。
2. 取得前に必要性、取得内容、容量、Microsoft利用条件へのリンクを表示する。
3. 利用者が明示的に同意した場合だけ取得を開始する。
4. Authenticode、版、SHA-256の全一致後だけMicrosoft対応quiet setupを起動する。
5. アプリがEULA同意を代行、偽装、暗黙承認しない。
6. 成功後に一時取得物を削除し、製品キャッシュとして再配布しない。
7. インストール済みADKは保持し、設定画面から通常のアンインストールを呼べるようにする。
8. Microsoft公式`/layout`によるoffline setは利用者指定先へ作成し、利用者資産として保持する。

未検証の最新版をWebスクレイピングで選択すること、Y-TEC側でMicrosoft取得物を
ミラーすること、生成ISOを自動アップロードすることは禁止する。

## 5. Windows標準ツール

`bcdboot.exe`、`mbr2gpt.exe`、`reagentc.exe`、`dism.exe`、`diskpart.exe`、
PowerShell等は製品へコピーしない。対象Windows／WinPEの正規System32または
検証済みADKインストール先にあるファイルを、絶対パス、非reparse、Microsoft署名、
許可版、固定引数を確認してその場で呼ぶ。

Secure Boot、Windowsライセンス認証、BitLockerの回避を行わない。

## 6. Visual Studio／MSVC

- 開発・正式ビルドに使用したVisual Studio／Build Toolsの製品名、版、Edition、使用資格を記録する。
- Visual Studio Communityの適用条件内であることを人間が正式公開前に確認する。
- Community条件に該当しない場合は、適切なProfessional／Enterprise等のライセンスへ切り替えるまで公開しない。
- 静的MSVCランタイム構成を維持し、デバッグランタイムを配布しない。
- Microsoft REDIST一覧と使用中ライセンス条件をリリースごとに確認する。
- SDK、Build Tools、Import Libraryはビルド時だけ使用し、リポジトリへコピーしない。

## 7. 通信・プライバシー

製品ネットワーク通信は次の2つだけに限定する。

1. 利用者が開始したMicrosoft公式ADK／WinPE取得
2. 利用者が「更新を確認」を押した時のY-TEC公式HTTPSページ照会

起動時通信、自動更新、テレメトリ、クラッシュ自動送信、広告、クラウド同期、
ネットワーク共有へのイメージ保存を実装しない。

プライバシーポリシーには、通信が発生する操作、送信先、送信される最小情報、
自動送信がないことを明記する。サポートZIPは利用者が内容一覧を確認して
ローカル保存し、自動送信しない。

## 8. Windowsライセンス注意文

製品内、操作マニュアル、Web、利用規約へ次の趣旨を明記する。

> 本ソフトウェアは、Windowsその他のソフトウェアに関するライセンスを付与または
> 移転するものではありません。バックアップ、復元および別デバイスへの移行は、
> 対象ソフトウェアの使用許諾条件に従い、適切にライセンスされた環境で実施して
> ください。本ソフトウェアはライセンス認証の回避機能を提供しません。

## 9. リリース監査

正式公開前に次を全件確認する。

- 実配布ZIPにMicrosoft媒体・ツール・デバッグランタイムがない。
- `dependencies.json`、ライセンス本文、通知、SBOM、実バイナリが一致する。
- 製品ルート`LICENSE`と`NOTICE`がPortable ZIPおよびWinPE媒体へ含まれ、
  リポジトリ正本とSHA-256一致する。
- Zstandard／LINE Seed JP／Argon2の取得物Hashが固定値と一致する。
- 禁止ライセンス、未知バイナリ、由来不明コード、秘密情報がない。
- Visual Studio／MSVC使用資格の記録がある。
- 製品利用規約、免責、プライバシー、Windows注意文が公開物に含まれる。
- Microsoft、Windows、Windows PE、競合製品のロゴや公認誤認表現がない。
- WinPE／ADK非同梱と、公式取得時の利用者同意を説明している。
- Y-TEC公式ZIP、Web掲載Hash、再ダウンロードしたZIPのSHA-256が一致する。
