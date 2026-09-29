# Packaging

ポータブル配布物は、静的MSVC x64でビルドしたY-TEC製EXE、媒体作成
スクリプト、説明、製品`LICENSE`／`NOTICE`、第三者ライセンス表示、SBOM、
SHA-256一覧だけを含みます。
Microsoft製EXE、DLL、CAB、WIM、ISO、ADK/WinPE本体は同梱しません。
初回からEXE隣の正規保存先を一意にできるよう、製品所有の
`data\README.txt`も同梱します。更新時は`data`全体を保持します。

1.0.0の製品WinPE媒体は、利用者PCへ検証済み構成として導入した
ADK/WinPE Add-onからリポジトリ外へ生成します。不足時のMicrosoft公式取得・
検証・導入は利用者の明示同意後だけ行います。自作の静的
`ytec-winpe-app.exe`と`ytec-winpe-gui.exe`に加え、製品`LICENSE`／`NOTICE`、
第三者通知、SPDX SBOM、
Zstandard／LINE Seed JP／Argon2のライセンス本文をWIMへ追加し、日本語表示は
同じローカルADKの`WinPE-FontSupport-JA-JP.cab`をDISMで適用します。
CAB、WIM、ISO、ADKツールを配布ZIPまたはリポジトリへ含めません。

現在は`1.0.0-internal-beta`という開発識別子の公開前検証候補であり、正式版1.0.0では
ありません。本製品は会社・勤務先の社内ソフトではなく、Y-TEC名義で個人開発・公開する
オープンソースソフトウェアです。
配布候補の生成だけで正式リリース可能とは扱いません。
全製品経路、正式OS VM、代表実機、文書・Hash監査の合格後に候補を封印します。

リポジトリ外の新規パスへ配布候補を作る例:

```powershell
./scripts/New-PortablePackage.ps1 `
  -OutputRoot C:\TsumugiRelease\Y-TEC-Tsumugi-Drive-1.0.0-internal-beta `
  -BuildPackage
```

既存フォルダー/ZIP、リポジトリ内、ドライブ直下、reparse pointを拒否します。
生成物は次の実体監査を通してから扱います。

```powershell
./scripts/Test-PortablePackageArtifact.ps1 `
  -PackageRoot C:\TsumugiRelease\Y-TEC-Tsumugi-Drive-1.0.0-internal-beta `
  -ZipPath C:\TsumugiRelease\Y-TEC-Tsumugi-Drive-1.0.0-internal-beta.zip
```

CIは一意な一時フォルダーで実ZIPを作成し、日本語ファイル名を保持する
UTF-8の`SHA256SUMS.txt`、全ハッシュ、期待する配布ファイル集合、
`data\README.txt`の実体、Microsoft媒体/ツール名の不在を検証してから、
その一時成果物だけを削除します。
