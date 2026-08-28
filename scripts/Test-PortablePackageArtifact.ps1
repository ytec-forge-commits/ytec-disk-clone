param(
    [Parameter(Mandatory)]
    [string]$PackageRoot,

    [Parameter(Mandatory)]
    [string]$ZipPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Assert-RegularNonReparseFile {
    param(
        [Parameter(Mandatory)]
        [string]$Path,
        [Parameter(Mandatory)]
        [string]$Description
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Description が見つかりません: $Path"
    }
    $item = Get-Item -LiteralPath $Path -Force
    if (($item.Attributes -band
            [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "$Description のreparse pointは使用しません: $Path"
    }
    if ($item.Length -le 0) {
        throw "$Description が空です: $Path"
    }
}

$root = [IO.Path]::GetFullPath($PackageRoot)
$zip = [IO.Path]::GetFullPath($ZipPath)
if (-not (Test-Path -LiteralPath $root -PathType Container)) {
    throw "ポータブル配布フォルダーが見つかりません: $root"
}
$rootItem = Get-Item -LiteralPath $root -Force
if (($rootItem.Attributes -band
        [IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw "配布フォルダーのreparse pointは使用しません: $root"
}
Assert-RegularNonReparseFile -Path $zip -Description 'ポータブルZIP'

$expectedFiles = @(
    'ytec-tsumugi-drive.exe',
    'tools\New-WinPEAppValidationMedia.ps1',
    'tools\ytec-winpe-environment.exe',
    'winpe\ytec-winpe-app.exe',
    'winpe\ytec-winpe-gui.exe',
    'はじめに.txt',
    '操作ガイド.txt',
    '安全上の注意と既知の制限.txt',
    'プライバシーと通信.txt',
    'セキュリティ報告.txt',
    'data\README.txt',
    'LICENSE',
    'NOTICE',
    'THIRD-PARTY-NOTICES.txt',
    'SBOM.spdx.json',
    'licenses\README.md',
    'licenses\LINE-Seed-JP-OFL-1.1.txt',
    'licenses\Zstandard-BSD-3-Clause.txt',
    'licenses\Argon2-Apache-2.0.txt',
    'SHA256SUMS.txt')
$actualFiles = @(
    Get-ChildItem -LiteralPath $root -Recurse -File |
        ForEach-Object {
            $_.FullName.Substring($root.Length).TrimStart('\')
        } |
        Sort-Object)
if ($actualFiles.Count -ne $expectedFiles.Count) {
    throw "配布フォルダーのファイル数が不正です: $($actualFiles.Count)"
}
foreach ($relative in $expectedFiles) {
    if ($actualFiles -notcontains $relative) {
        throw "配布フォルダーの必須ファイルがありません: $relative"
    }
    Assert-RegularNonReparseFile `
        -Path (Join-Path $root $relative) `
        -Description $relative
}

function Get-StreamSha256 {
    param(
        [Parameter(Mandatory)]
        [IO.Stream]$Stream
    )

    $sha256 = [Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString(
                $sha256.ComputeHash($Stream))).Replace('-', '')
    } finally {
        $sha256.Dispose()
    }
}

$repoRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$dependencyManifestPath = Join-Path $repoRoot `
    'third_party\dependencies.json'
Assert-RegularNonReparseFile `
    -Path $dependencyManifestPath `
    -Description '依存台帳'
$dependencies = @((Get-Content -LiteralPath $dependencyManifestPath -Raw |
        ConvertFrom-Json).dependencies)
$expectedDependencies = [ordered]@{
    'LINE Seed JP' = [ordered]@{
        version = 'LINESeedJP_20241105'
        source = 'https://seed.line.me/src/images/fonts/LINE_Seed_JP.zip'
        sourceArchiveSha256 = '75C0144CB1076EA1FE5C9BF081396333D40B6D4B66C5F812604EAC2C087F4F48'
        license = 'OFL-1.1'
        licenseFile = 'licenses/LINE-Seed-JP-OFL-1.1.txt'
        copyright = 'Copyright 2020-2022 LY Corporation'
        primaryPackagePurpose = 'FONT'
    }
    'Zstandard' = [ordered]@{
        version = '1.5.7'
        source = 'https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz'
        sourceArchiveSha256 = 'EB33E51F49A15E023950CD7825CA74A4A2B43DB8354825AC24FC1B7EE09E6FA3'
        license = 'BSD-3-Clause'
        licenseFile = 'licenses/Zstandard-BSD-3-Clause.txt'
        copyright = 'Copyright (c) Meta Platforms, Inc. and affiliates. All rights reserved.'
        primaryPackagePurpose = 'LIBRARY'
    }
    'Argon2' = [ordered]@{
        version = '20190702'
        source = 'https://github.com/P-H-C/phc-winner-argon2/archive/refs/tags/20190702.tar.gz'
        sourceArchiveSha256 = 'DAF972A89577F8772602BF2EB38B6A3DD3D922BF5724D45E7F9589B5E830442C'
        license = 'Apache-2.0'
        licenseFile = 'licenses/Argon2-Apache-2.0.txt'
        copyright = 'Copyright 2015 Daniel Dinu, Dmitry Khovratovich, Jean-Philippe Aumasson, and Samuel Neves'
        primaryPackagePurpose = 'LIBRARY'
    }
}
if ($dependencies.Count -ne $expectedDependencies.Count) {
    throw "依存台帳の外部依存数が配布基線と一致しません: $($dependencies.Count)"
}

$noticesText = Get-Content `
    -LiteralPath (Join-Path $root 'THIRD-PARTY-NOTICES.txt') `
    -Raw
$licenseReadmeText = Get-Content `
    -LiteralPath (Join-Path $root 'licenses\README.md') `
    -Raw
$sbom = Get-Content -LiteralPath (Join-Path $root 'SBOM.spdx.json') `
    -Raw | ConvertFrom-Json
$packagedProjectLicense = Join-Path $root 'LICENSE'
$packagedProjectNotice = Join-Path $root 'NOTICE'
$sourceProjectLicense = Join-Path $repoRoot 'LICENSE'
$sourceProjectNotice = Join-Path $repoRoot 'NOTICE'
foreach ($projectDocument in @(
        [ordered]@{
            packaged = $packagedProjectLicense
            source = $sourceProjectLicense
            description = '製品LICENSE'
        },
        [ordered]@{
            packaged = $packagedProjectNotice
            source = $sourceProjectNotice
            description = '製品NOTICE'
        })) {
    Assert-RegularNonReparseFile `
        -Path $projectDocument.source `
        -Description "$($projectDocument.description)正本"
    if ((Get-FileHash -LiteralPath $projectDocument.packaged `
                -Algorithm SHA256).Hash -cne
        (Get-FileHash -LiteralPath $projectDocument.source `
                -Algorithm SHA256).Hash) {
        throw "$($projectDocument.description)がリポジトリ正本と一致しません。"
    }
}
$rootSbomPackages = @($sbom.packages | Where-Object {
        $_.SPDXID -ceq 'SPDXRef-Package-ytec-disk-clone'
    })
if ($rootSbomPackages.Count -ne 1 -or
    $rootSbomPackages[0].licenseDeclared -cne 'Apache-2.0' -or
    $rootSbomPackages[0].licenseConcluded -cne 'Apache-2.0' -or
    $rootSbomPackages[0].downloadLocation -cne
        'https://github.com/ytec-forge-commits/ytec-disk-clone' -or
    $rootSbomPackages[0].copyrightText -cne 'Copyright 2026 Y-TEC') {
    throw 'SBOMの製品packageがApache-2.0公開台帳と一致しません。'
}
foreach ($dependencyName in $expectedDependencies.Keys) {
    $dependency = @($dependencies | Where-Object {
            $_.name -ceq $dependencyName
        })
    if ($dependency.Count -ne 1 -or $dependency[0].approved -ne $true) {
        throw "承認済み依存を依存台帳で一意に特定できません: $dependencyName"
    }
    $entry = $dependency[0]
    $expected = $expectedDependencies[$dependencyName]
    foreach ($field in @(
            'version',
            'source',
            'sourceArchiveSha256',
            'license',
            'licenseFile',
            'copyright',
            'primaryPackagePurpose')) {
        if ([string]$entry.$field -cne [string]$expected[$field]) {
            throw "依存台帳の固定値が配布基線と一致しません: $dependencyName / $field"
        }
    }
    $licenseRelative = ([string]$entry.licenseFile).Replace('/', '\')
    if ($expectedFiles -notcontains $licenseRelative) {
        throw "依存台帳のライセンス本文が配布集合にありません: $dependencyName"
    }
    $packagedLicense = Join-Path $root $licenseRelative
    $sourceLicense = Join-Path $repoRoot $licenseRelative
    Assert-RegularNonReparseFile `
        -Path $sourceLicense `
        -Description "$dependencyName の正本ライセンス"
    if ((Get-FileHash -LiteralPath $packagedLicense -Algorithm SHA256).Hash -ne
        (Get-FileHash -LiteralPath $sourceLicense -Algorithm SHA256).Hash) {
        throw "配布ライセンス本文が正本と一致しません: $dependencyName"
    }
    $licenseFileName = [IO.Path]::GetFileName($licenseRelative)
    if (-not $licenseReadmeText.Contains($licenseFileName)) {
        throw "licenses/README.mdにライセンス本文の案内がありません: $dependencyName"
    }
    if (-not $licenseReadmeText.Contains([string]$entry.version)) {
        throw "licenses/README.mdに固定版の案内がありません: $dependencyName"
    }
    $noticeLicensePath = ([string]$entry.licenseFile).Replace('\', '/')
    if (-not $noticesText.Contains("Version: $($entry.version)") -or
        -not $noticesText.Contains("Source: $($entry.source)") -or
        -not $noticesText.Contains("License text: $noticeLicensePath") -or
        -not $noticesText.Contains([string]$entry.sourceArchiveSha256) -or
        -not $noticesText.Contains([string]$entry.copyright)) {
        throw "第三者通知が依存台帳と一致しません: $dependencyName"
    }
    $sbomPackage = @($sbom.packages | Where-Object {
            $_.name -ceq $dependencyName
        })
    if ($sbomPackage.Count -ne 1 -or
        $sbomPackage[0].versionInfo -cne $entry.version -or
        $sbomPackage[0].downloadLocation -cne $entry.source -or
        $sbomPackage[0].licenseDeclared -cne $entry.license -or
        $sbomPackage[0].licenseConcluded -cne $entry.license -or
        $sbomPackage[0].copyrightText -cne $entry.copyright -or
        $sbomPackage[0].primaryPackagePurpose -cne
            $entry.primaryPackagePurpose) {
        throw "SBOMが依存台帳と一致しません: $dependencyName"
    }
    $relationship = @($sbom.relationships | Where-Object {
            $_.spdxElementId -ceq 'SPDXRef-Package-ytec-disk-clone' -and
            $_.relationshipType -ceq 'DEPENDS_ON' -and
            $_.relatedSpdxElement -ceq $sbomPackage[0].SPDXID
        })
    if ($relationship.Count -ne 1) {
        throw "SBOMのDEPENDS_ON関係が一意ではありません: $dependencyName"
    }
}

$forbiddenExtensions = @(
    '.wim', '.iso', '.cab', '.msi', '.msix', '.vhd', '.vhdx')
$forbiddenNames = @(
    'dism.exe', 'oscdimg.exe', 'mbr2gpt.exe', 'bcdboot.exe',
    'diskpart.exe', 'copype.cmd', 'makewinpemedia.cmd')
foreach ($file in Get-ChildItem -LiteralPath $root -Recurse -File) {
    if ($forbiddenExtensions -contains $file.Extension.ToLowerInvariant() -or
        $forbiddenNames -contains $file.Name.ToLowerInvariant()) {
        throw "配布禁止ファイルを検出しました: $($file.FullName)"
    }
    if (($file.Attributes -band
            [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "配布物にreparse pointがあります: $($file.FullName)"
    }
}

$hashPath = Join-Path $root 'SHA256SUMS.txt'
$hashBytes = [IO.File]::ReadAllBytes($hashPath)
$utf8 = [Text.UTF8Encoding]::new($false, $true)
$hashText = $utf8.GetString($hashBytes)
if ($hashText -notmatch [regex]::Escape('*はじめに.txt')) {
    throw 'SHA256SUMS.txtが日本語ファイル名をUTF-8で保持していません。'
}
$hashLines = @($hashText -split '\r?\n' |
    Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
if ($hashLines.Count -ne ($expectedFiles.Count - 1)) {
    throw "SHA256SUMS.txtの行数が不正です: $($hashLines.Count)"
}
foreach ($line in $hashLines) {
    if ($line -notmatch '^([0-9A-F]{64}) \*(.+)$') {
        throw "SHA256SUMS.txtの行形式が不正です: $line"
    }
    $relative = $matches[2].Replace('/', '\')
    $filePath = Join-Path $root $relative
    Assert-RegularNonReparseFile -Path $filePath -Description $relative
    $actual = (Get-FileHash -LiteralPath $filePath -Algorithm SHA256).Hash
    if ($actual -ne $matches[1]) {
        throw "配布ファイルのSHA-256が一致しません: $relative"
    }
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::OpenRead($zip)
try {
    $entries = @($archive.Entries |
        Where-Object { -not [string]::IsNullOrEmpty($_.Name) })
    if ($entries.Count -ne $expectedFiles.Count) {
        throw "ZIPのファイル数が不正です: $($entries.Count)"
    }
    foreach ($relative in $expectedFiles) {
        $entryName = $relative.Replace('\', '/')
        $entry = @($entries | Where-Object {
            $_.FullName.Equals(
                $entryName,
                [StringComparison]::Ordinal)
        })
        if ($entry.Count -ne 1 -or $entry[0].Length -le 0) {
            throw "ZIPの必須ファイルがありません: $entryName"
        }
        $stream = $entry[0].Open()
        try {
            $zipEntryHash = Get-StreamSha256 -Stream $stream
        } finally {
            $stream.Dispose()
        }
        $folderHash = (Get-FileHash `
            -LiteralPath (Join-Path $root $relative) `
            -Algorithm SHA256).Hash
        if ($zipEntryHash -cne $folderHash) {
            throw "ZIP内容が監査済み配布フォルダーと一致しません: $entryName"
        }
    }
    foreach ($entry in $entries) {
        $extension = [IO.Path]::GetExtension(
            $entry.Name).ToLowerInvariant()
        if ($forbiddenExtensions -contains $extension -or
            $forbiddenNames -contains $entry.Name.ToLowerInvariant()) {
            throw "ZIPに配布禁止ファイルがあります: $($entry.FullName)"
        }
    }
} finally {
    $archive.Dispose()
}

$report = [ordered]@{
    schemaVersion = 1
    packageRoot = $root
    zipPath = $zip
    fileCount = $expectedFiles.Count
    hashedFileCount = $hashLines.Count
    zipSha256 = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash
    repositoryContainsMicrosoftPayload = $false
}
Write-Output ('TSUMUGI_PORTABLE_ARTIFACT_PASS=' +
    ($report | ConvertTo-Json -Depth 3 -Compress))
