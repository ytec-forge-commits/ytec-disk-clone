# ADK固定値監査 — Tsumugi Drive 1.0.0

監査日: 2026-08-09

この文書は、正式版1.0.0で利用するMicrosoft配布物の候補を一次資料と
実取得物で照合した記録です。ここに値があるだけでは自動導入を許可しません。
製品コードの固定マニフェスト、アーカイブ展開境界、同意画面、クリーンVM試験が
すべて合格した後にだけ実行ゲートを有効化します。

## 一次資料

- ADK導入案内: <https://learn.microsoft.com/en-us/windows-hardware/get-started/adk-install>
- ADKオフライン導入案内: <https://learn.microsoft.com/en-us/windows-hardware/get-started/adk-offline-install>
- ADK更新案内: <https://learn.microsoft.com/en-us/windows-hardware/get-started/adk-servicing>
- 固定版: Windows ADK / Windows PE Add-on `10.1.26100.2454`
- 必須更新候補: `KB5101684`

Microsoft公式案内は、上記ADKへ`KB5079391`以降のセキュリティ更新を要求し、
2026-08-09時点の更新ページでは、より新しい`KB5101684`を同版向けに案内している。
同日に3取得物を固定直接URLから再取得し、下表のbytesとSHA-256を再照合した。
2つのbootstrap EXEはAuthenticode `Valid`、署名者`Microsoft Corporation`、
File/Product version `10.1.26100.2454`も再確認した。検証用取得物は照合後に削除し、
リポジトリや製品ZIPへ保存していない。

## 取得物

| 種別 | 直接取得URL | bytes | SHA-256 | File version | 署名 |
|---|---|---:|---|---|---|
| Deployment Tools bootstrap | `https://download.microsoft.com/download/2/d/9/2d9c8902-3fcd-48a6-a22a-432b08bed61e/ADK/adksetup.exe` | 2,234,632 | `7F61E29F2314BCDD7E0ABF67A8367D83A05AA4A7B9223F85C5FD2582A35CC6F4` | `10.1.26100.2454` | Microsoft Corporation / Valid |
| WinPE Add-on bootstrap | `https://download.microsoft.com/download/5/5/6/556e01ec-9d78-417d-b1e1-d83a2eff20bc/ADKWinPEAddons/adkwinpesetup.exe` | 1,945,400 | `ADF53CA21CAE36821E0A8F3C31546752B9CE066944DE1D4F1673E491831255E2` | `10.1.26100.2454` | Microsoft Corporation / Valid |
| ADK update archive | `https://download.microsoft.com/download/a087a851-4056-4f7f-9791-02a20509b706/Windows_ADK_10.1.26100.2454_Update_KB5101684.zip` | 411,048,362 | `DC19725A2FB0CCE44C32AC14059A85A25257B9534BA21C93B479F4F09FB5AF38` | archive | ZIP自体はAuthenticode対象外 |

公式短縮URLと固定リダイレクト先は次の組です。

- `https://go.microsoft.com/fwlink/?linkid=2289980` → Deployment Tools
- `https://go.microsoft.com/fwlink/?linkid=2289981` → WinPE Add-on
- `https://aka.ms/Windows_ADK_10.1.26100.2454_Update_KB5101684.zip` → ADK update archive

## ADK固有EULAの導入前取得

固定済みDeployment Tools bootstrap `adksetup.exe`を、全体SHA-256、
Authenticode署名者、File/Product versionの照合後に読み取り専用で監査した。
PE stub末尾のattached Burn CABはoffset `0xB3000`、length `0x16C2CD`で、
BurnManifestのUX mappingは`SourcePath="u6"`を`FilePath="ja\eula.rtf"`、
`FileSize="293766"`としている。CAB member `u6`を有界抽出して得た本文は次の固定値だった。

| 項目 | 固定値 |
|---|---|
| 表示名 | `ja\eula.rtf` |
| bytes | 293,766 |
| SHA-256 | `32B66AE90683DE9C91EDE927A45E8E44845CD36E43821BFA4EB2CA5C36A9CF54` |
| 文書タイトル | `WINDOWS ASSESSMENT AND DEPLOYMENT KIT (ADK)` |

導入済み環境の`Docs\Eula\ja-jp\eula.rtf`ともbyte一致した。したがって、
単独の公開EULA URLや汎用Microsoft規約を代用せず、未導入PCでも、固定Microsoft
bootstrap自体を公式取得元としてADK固有EULA本文を導入前に提示できる。
BurnManifest内のSHA-1は補助情報に限り、製品の正本固定値には上記SHA-256を使う。

実Windows adapterは、同じ所有済みbootstrapの全体identityとSHA-256を抽出前後に
再検証し、固定CAB範囲とmember `u6`をCREATE_NEWの非reparse単一link領域へ有界抽出、
完全読取り後のSHA-256・文書title一致、所有一時ファイルのhandle削除まで実装した。
固定公式bootstrapを使った読取り検証でもreceiptと本文の取得に合格した。ただし、
取得内容と全文を表示して明示同意を得る製品UIはまだ未接続なので、自動導入ゲートは開かない。

## KB5101684 アーカイブ

アーカイブは固定ルートディレクトリ1件と、次のMicrosoft署名済みMSP 9件だけを
含むことを確認した。製品実装ではファイル名、個数、非圧縮長、SHA-256、
Microsoft署名、MSP Revision GUIDをすべて固定し、未知の項目や重複を拒否する。

| MSP | bytes | SHA-256 | Revision GUID |
|---|---:|---|---|
| `Appman Sequencer on amd64-x64_en-us.msp` | 91,561,984 | `027A1FC0C20CFD2A35B9D51225419C682C8F6CF3B68BE90E4B5FBF9A8DFF3BB5` | `{F8E9F1ED-2F45-4C33-8C3D-FA9C657511C8}` |
| `Appman Sequencer on x86-x86_en-us.msp` | 84,307,968 | `309B4C907F85778D57CCCE94A7E9EE2BEA077F4894656FE1D68BAB4CF12C265F` | `{C4256C64-EEC0-4D8D-9666-3A09317B69FD}` |
| `OA3Tool-x86_en-us.msp` | 1,634,304 | `F1BF0D357C32E3767D3215D8DA3AEEADD2E40CF613BB0C1326AD2EC2A62A98D3` | `{F5FA22BB-8025-481D-AEBF-8D54AC801DA7}` |
| `OACheck-x86_en-us.msp` | 1,462,272 | `9D5D50E16D77ABD32A7348371291B6FD3A7EF3BC4472644BAB20ED82BF1A4C28` | `{9D99F1AD-52C1-48C5-B0A2-8C203EBC054C}` |
| `OATool-x86_en-us.msp` | 258,048 | `307B2923025340E789D1156E782DADF2B124273EBE675BDB83D2FAF2208C25E3` | `{EE1D618C-B0FF-4572-9B0B-F66A25559385}` |
| `Oscdimg (OnecoreUAP)-x86_en-us.msp` | 18,132,992 | `A93F24C3275967E4F0DEC5792BE2102A068A60EE481143545589B1D073327C6A` | `{83449E02-24CA-44C1-A0A3-80A9AA2E85F0}` |
| `Volume Activation Management Tool-x86_en-us.msp` | 864,256 | `8D278EB333C601A28E26FA2EA2097864C596AFAA9C20440DBAD67A034A6777D5` | `{4FEA9E08-B1CF-4C0C-8E90-31BAEE5AF0E4}` |
| `Windows Deployment Image Servicing and Management Tools (OnecoreUAP)-x86_en-us.msp` | 181,039,104 | `77242FEFF4CF0221C84249A21675215DB6A12D26E633F4E4BD622E942B368B04` | `{BF926991-C615-45A6-BF73-AFC83E790865}` |
| `Windows System Image Manager-x86_en-us.msp` | 33,853,440 | `2570FA4C28A3D939BA60CC3290823998D3506C803F152E4D95B28F310EC2B416` | `{4F944331-4B13-4554-9627-2B606D4B4EEE}` |

全MSPの署名状態は`Valid`、署名者Subjectは
`CN=Microsoft Windows, O=Microsoft Corporation, L=Redmond, S=Washington, C=US`
だった。

## 実装済みの安全ゲート

- ZIPは固定member 9件以外、重複、path traversal、非正規名、上限超過を拒否し、
  所有する新規非reparse領域へ有界展開する。
- 展開MSPは長さ、SHA-256、Microsoft署名、Revision GUIDを同一の固定表で再照合する。
- 起動直前に所有識別、通常ファイル、非reparse、単一link、長さ、Hash、署名、版を再検証し、
  shellを使わず絶対`System32\\msiexec.exe`へ`/qn /norestart /p`で渡す。
- 未導入の任意ADK機能向けMSPが返す`1642`はMSPに限って記録・継続し、
  Deployment Tools、WinPE、DISM、Oscdimg、必須更新の導入後再検査に合格しなければ失敗する。
- 既存ADKは削除せず、Tsumugi自身が固定マニフェストで導入した記録だけを
  アンインストール計画の対象にできる。
- 同意レビューは、bootstrap URL、固定CAB範囲、member、EULA bytes／SHA-256、
  3取得物の公式URLと取得内容、EULA全文表示、利用者の明示操作を同じマニフェストへ
  結び付ける。EULA digestまたは取得物順序が変われば同意を再利用しない。
- `AdkReleaseManifest::unattended_install_no_unexpected_restart_confirmed`がfalseなら、
  自動導入はプラットフォーム呼出し前に停止する。

## 未解決ゲート

- Deployment ToolsとWinPE Add-onの導入前に、取得内容と抽出したADK固有EULA全文を
  画面へ表示し、利用者自身の明示同意を得る。
- 現行Microsoft ADKオフライン導入資料はbootstrapの`/quiet /installpath /features`を
  明示する一方、`/norestart`が確実に尊重されることまでは明示していない。
  MSPを起動する`msiexec /norestart`のMicrosoft仕様とは分け、bootstrapについては
  ADK未導入の制御VMで引数受理と予期しない再起動がないことを証明する。
- ADK未導入のクリーンVMで、公式取得、EULA同意、MSP `0/1642/3010`、
  導入後再検査、オフラインレイアウト、管理対象だけの削除を通しで確認する。

これらが終わるまで`primary_source_pins_confirmed=false`を維持し、
自動ダウンロード／自動インストールは安全側に停止する。
