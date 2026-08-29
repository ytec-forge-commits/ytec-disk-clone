param(
    [Parameter(Mandatory)]
    [string]$OutputRoot,

    [switch]$BuildPackage
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$OutputEncoding = [Text.UTF8Encoding]::new($false)

function Assert-NewExternalPath {
    param(
        [Parameter(Mandatory)]
        [string]$RepositoryRoot,
        [Parameter(Mandatory)]
        [string]$CandidatePath
    )

    $repository = [IO.Path]::GetFullPath($RepositoryRoot)
    $candidate = [IO.Path]::GetFullPath($CandidatePath)
    $repositoryPrefix = $repository.TrimEnd('\') + '\'
    if ($candidate.Equals(
            $repository,
            [StringComparison]::OrdinalIgnoreCase) -or
        $candidate.StartsWith(
            $repositoryPrefix,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw 'ポータブル配布物はリポジトリ外だけに作成できます。'
    }
    $root = [IO.Path]::GetPathRoot($candidate)
    if ([string]::IsNullOrWhiteSpace($root) -or
        $candidate.TrimEnd('\').Equals(
            $root.TrimEnd('\'),
            [StringComparison]::OrdinalIgnoreCase)) {
        throw 'ドライブ直下は出力先にできません。'
    }
    if (Test-Path -LiteralPath $candidate) {
        throw "既存の出力先は上書きしません: $candidate"
    }
    $parent = Split-Path -Parent $candidate
    if (-not (Test-Path -LiteralPath $parent -PathType Container)) {
        throw "出力先の親フォルダーがありません: $parent"
    }
    $current = $parent
    while (-not [string]::IsNullOrWhiteSpace($current)) {
        $item = Get-Item -LiteralPath $current -Force
        if (($item.Attributes -band
                [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "出力先の祖先にreparse pointがあります: $current"
        }
        $next = Split-Path -Parent $current
        if ([string]::IsNullOrWhiteSpace($next) -or
            $next -eq $current) {
            break
        }
        $current = $next
    }
    return $candidate
}

function Assert-RegularFile {
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
    if ($item.Length -le 0 -or $item.Length -gt 128MB) {
        throw "$Description のサイズが許可範囲外です: $($item.Length)"
    }
}

function Assert-Amd64Pe {
    param(
        [Parameter(Mandatory)]
        [string]$Path,
        [Parameter(Mandatory)]
        [string]$Description
    )

    Assert-RegularFile -Path $Path -Description $Description
    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 512 -or
        $bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) {
        throw "$Description にDOS MZ署名がありません。"
    }
    $peOffset = [BitConverter]::ToInt32($bytes, 0x3C)
    if ($peOffset -lt 0x40 -or $peOffset -gt ($bytes.Length - 26) -or
        $bytes[$peOffset] -ne 0x50 -or
        $bytes[$peOffset + 1] -ne 0x45 -or
        $bytes[$peOffset + 2] -ne 0 -or
        $bytes[$peOffset + 3] -ne 0) {
        throw "$Description のPE署名が不正です。"
    }
    $machine = [BitConverter]::ToUInt16($bytes, $peOffset + 4)
    $magic = [BitConverter]::ToUInt16($bytes, $peOffset + 24)
    if ($machine -ne 0x8664 -or $magic -ne 0x020B) {
        throw "$Description はAMD64 PE32+ではありません。"
    }
}

function Assert-NoMicrosoftPayload {
    param(
        [Parameter(Mandatory)]
        [string]$Root
    )

    $forbiddenExtensions = @(
        '.wim', '.iso', '.cab', '.msi', '.msix', '.vhd', '.vhdx')
    $forbiddenNames = @(
        'dism.exe', 'oscdimg.exe', 'mbr2gpt.exe', 'bcdboot.exe',
        'diskpart.exe', 'copype.cmd', 'makewinpemedia.cmd')
    foreach ($file in Get-ChildItem -LiteralPath $Root -Recurse -File) {
        if ($forbiddenExtensions -contains $file.Extension.ToLowerInvariant() -or
            $forbiddenNames -contains $file.Name.ToLowerInvariant()) {
            throw "配布禁止のMicrosoft/媒体ファイルを検出しました: $($file.FullName)"
        }
        if (($file.Attributes -band
                [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "配布物にreparse pointがあります: $($file.FullName)"
        }
    }
}

function Write-NewUtf8File {
    param(
        [Parameter(Mandatory)]
        [string]$Path,
        [Parameter(Mandatory)]
        [string]$Text
    )

    $bytes = [Text.UTF8Encoding]::new($false).GetBytes($Text)
    $stream = [IO.File]::Open(
        $Path,
        [IO.FileMode]::CreateNew,
        [IO.FileAccess]::Write,
        [IO.FileShare]::None)
    try {
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush($true)
    } finally {
        $stream.Dispose()
    }
}

$repoRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
. (Join-Path $PSScriptRoot 'ProductVersion.ps1')
$productVersion = Read-YtecProductVersion `
    -Path (Join-Path $repoRoot 'version.json')
$outputFullPath = Assert-NewExternalPath `
    -RepositoryRoot $repoRoot `
    -CandidatePath $OutputRoot
$zipPath = Assert-NewExternalPath `
    -RepositoryRoot $repoRoot `
    -CandidatePath ($outputFullPath + '.zip')

$zipSha256Path = Assert-NewExternalPath `
    -RepositoryRoot $repoRoot `
    -CandidatePath ($zipPath + '.sha256')

$staticRoot = Join-Path $repoRoot 'out\build\msvc-x64-vm'
$sources = [ordered]@{
    windowsApp = Join-Path $staticRoot `
        'src\WindowsApp\ytec-tsumugi-drive.exe'
    environment = Join-Path $staticRoot `
        'src\MediaBuilder\ytec-winpe-environment.exe'
    winpeCli = Join-Path $staticRoot `
        'src\WinPEApp\ytec-winpe-app.exe'
    winpeGui = Join-Path $staticRoot `
        'src\WinPEApp\ytec-winpe-gui.exe'
    builderScript = Join-Path $repoRoot `
        'scripts\New-WinPEAppValidationMedia.ps1'
    readme = Join-Path $repoRoot 'packaging\portable-readme.txt'
    operationGuide = Join-Path $repoRoot 'packaging\operation-guide.txt'
    safetyAndLimitations = Join-Path $repoRoot `
        'packaging\safety-and-limitations.txt'
    privacyAndNetwork = Join-Path $repoRoot `
        'packaging\privacy-and-network.txt'
    securityReporting = Join-Path $repoRoot `
        'packaging\security-reporting.txt'
    termsOfUse = Join-Path $repoRoot 'packaging\terms-of-use.txt'
    dataReadme = Join-Path $repoRoot 'packaging\data-readme.txt'
    projectLicense = Join-Path $repoRoot 'LICENSE'
    projectNotice = Join-Path $repoRoot 'NOTICE'
    projectTrademarks = Join-Path $repoRoot 'TRADEMARKS.md'
    notices = Join-Path $repoRoot 'THIRD-PARTY-NOTICES.txt'
    sbom = Join-Path $repoRoot 'SBOM.spdx.json'
    licenseReadme = Join-Path $repoRoot 'licenses\README.md'
    lineSeedLicense = Join-Path $repoRoot `
        'licenses\LINE-Seed-JP-OFL-1.1.txt'
    zstandardLicense = Join-Path $repoRoot `
        'licenses\Zstandard-BSD-3-Clause.txt'
    argon2License = Join-Path $repoRoot `
        'licenses\Argon2-Apache-2.0.txt'
}

foreach ($name in @('windowsApp', 'environment', 'winpeCli', 'winpeGui')) {
    Assert-Amd64Pe -Path $sources[$name] -Description $name
}
$expectedExecutableMetadata = [ordered]@{
    windowsApp = [ordered]@{
        originalFilename = 'ytec-tsumugi-drive.exe'
        fileDescription = 'Y-TEC Tsumugi Drive'
    }
    environment = [ordered]@{
        originalFilename = 'ytec-winpe-environment.exe'
        fileDescription = 'Y-TEC Tsumugi Drive WinPE Environment'
    }
    winpeCli = [ordered]@{
        originalFilename = 'ytec-winpe-app.exe'
        fileDescription = 'Y-TEC Tsumugi Drive WinPE CLI'
    }
    winpeGui = [ordered]@{
        originalFilename = 'ytec-winpe-gui.exe'
        fileDescription = 'Y-TEC Tsumugi Drive WinPE GUI'
    }
}
$executableVersionInfo = [ordered]@{}
foreach ($name in $expectedExecutableMetadata.Keys) {
    $metadata = $expectedExecutableMetadata[$name]
    $executableVersionInfo[$name] = Assert-YtecExecutableVersionInfo `
        -Path $sources[$name] `
        -Version $productVersion `
        -OriginalFilename $metadata.originalFilename `
        -FileDescription $metadata.fileDescription
}
foreach ($name in @(
        'builderScript', 'readme', 'operationGuide',
        'safetyAndLimitations', 'privacyAndNetwork', 'securityReporting',
        'termsOfUse', 'dataReadme',
        'projectLicense', 'projectNotice', 'projectTrademarks',
        'notices', 'sbom', 'licenseReadme', 'lineSeedLicense',
        'zstandardLicense', 'argon2License')) {
    Assert-RegularFile -Path $sources[$name] -Description $name
}
$sbomVersionInfo = Assert-YtecSbomProductVersion `
    -Path $sources.sbom `
    -Version $productVersion `
    -ExpectedPackageName 'ytec-disk-clone' `
    -ExpectedNamespaceBase `
        'https://github.com/ytec-forge-commits/ytec-disk-clone/sbom'

$preflight = [ordered]@{
    schemaVersion = 1
    product = 'Y-TEC Tsumugi Drive'
    version = $productVersion.display
    fileVersion = $productVersion.file
    channel = $productVersion.channel
    outputRoot = $outputFullPath
    zipPath = $zipPath
    zipSha256Path = $zipSha256Path
    buildRequested = [bool]$BuildPackage
    repositoryContainsMicrosoftPayload = $false
    executables = $executableVersionInfo
    sbom = $sbomVersionInfo
    files = [ordered]@{}
}
foreach ($entry in $sources.GetEnumerator()) {
    $preflight.files[$entry.Key] = [ordered]@{
        path = $entry.Value
        length = (Get-Item -LiteralPath $entry.Value).Length
        sha256 = (Get-FileHash -LiteralPath $entry.Value `
            -Algorithm SHA256).Hash
    }
}

if (-not $BuildPackage) {
    Write-Output ('TSUMUGI_PORTABLE_PREFLIGHT_PASS=' +
        ($preflight | ConvertTo-Json -Depth 5 -Compress))
    return
}

New-Item -ItemType Directory -Path $outputFullPath | Out-Null
$tools = Join-Path $outputFullPath 'tools'
$winpe = Join-Path $outputFullPath 'winpe'
$licenses = Join-Path $outputFullPath 'licenses'
$data = Join-Path $outputFullPath 'data'
foreach ($directory in @($tools, $winpe, $licenses, $data)) {
    New-Item -ItemType Directory -Path $directory | Out-Null
}

Copy-Item -LiteralPath $sources.windowsApp `
    -Destination (Join-Path $outputFullPath 'ytec-tsumugi-drive.exe')
Copy-Item -LiteralPath $sources.environment `
    -Destination (Join-Path $tools 'ytec-winpe-environment.exe')
Copy-Item -LiteralPath $sources.builderScript `
    -Destination (Join-Path $tools 'New-WinPEAppValidationMedia.ps1')
Copy-Item -LiteralPath $sources.winpeCli `
    -Destination (Join-Path $winpe 'ytec-winpe-app.exe')
Copy-Item -LiteralPath $sources.winpeGui `
    -Destination (Join-Path $winpe 'ytec-winpe-gui.exe')
Copy-Item -LiteralPath $sources.readme `
    -Destination (Join-Path $outputFullPath 'はじめに.txt')
Copy-Item -LiteralPath $sources.operationGuide `
    -Destination (Join-Path $outputFullPath '操作ガイド.txt')
Copy-Item -LiteralPath $sources.safetyAndLimitations `
    -Destination (Join-Path $outputFullPath '安全上の注意と既知の制限.txt')
Copy-Item -LiteralPath $sources.privacyAndNetwork `
    -Destination (Join-Path $outputFullPath 'プライバシーと通信.txt')
Copy-Item -LiteralPath $sources.securityReporting `
    -Destination (Join-Path $outputFullPath 'セキュリティ報告.txt')
Copy-Item -LiteralPath $sources.termsOfUse `
    -Destination (Join-Path $outputFullPath '利用規約.txt')
Copy-Item -LiteralPath $sources.dataReadme `
    -Destination (Join-Path $data 'README.txt')
Copy-Item -LiteralPath $sources.projectLicense `
    -Destination (Join-Path $outputFullPath 'LICENSE')
Copy-Item -LiteralPath $sources.projectNotice `
    -Destination (Join-Path $outputFullPath 'NOTICE')
Copy-Item -LiteralPath $sources.projectTrademarks `
    -Destination (Join-Path $outputFullPath 'TRADEMARKS.md')
Copy-Item -LiteralPath $sources.notices `
    -Destination (Join-Path $outputFullPath 'THIRD-PARTY-NOTICES.txt')
Copy-Item -LiteralPath $sources.sbom `
    -Destination (Join-Path $outputFullPath 'SBOM.spdx.json')
Copy-Item -LiteralPath $sources.licenseReadme `
    -Destination (Join-Path $licenses 'README.md')
Copy-Item -LiteralPath $sources.lineSeedLicense `
    -Destination (Join-Path $licenses 'LINE-Seed-JP-OFL-1.1.txt')
Copy-Item -LiteralPath $sources.zstandardLicense `
    -Destination (Join-Path $licenses 'Zstandard-BSD-3-Clause.txt')
Copy-Item -LiteralPath $sources.argon2License `
    -Destination (Join-Path $licenses 'Argon2-Apache-2.0.txt')

Assert-NoMicrosoftPayload -Root $outputFullPath

$hashLines = @(
    foreach ($file in Get-ChildItem -LiteralPath $outputFullPath `
            -Recurse -File | Sort-Object FullName) {
        $relative = $file.FullName.Substring(
            $outputFullPath.Length).TrimStart('\').Replace('\', '/')
        $hash = (Get-FileHash -LiteralPath $file.FullName `
            -Algorithm SHA256).Hash
        "$hash *$relative"
    }
)
$hashPath = Join-Path $outputFullPath 'SHA256SUMS.txt'
[IO.File]::WriteAllLines(
    $hashPath,
    $hashLines,
    [Text.UTF8Encoding]::new($false))

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::Open(
    $zipPath,
    [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($file in Get-ChildItem -LiteralPath $outputFullPath `
            -Recurse -File | Sort-Object FullName) {
        $entryName = $file.FullName.Substring(
            $outputFullPath.Length).TrimStart('\').Replace('\', '/')
        [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
            $archive,
            $file.FullName,
            $entryName,
            [IO.Compression.CompressionLevel]::Optimal)
    }
}
finally {
    $archive.Dispose()
}
Assert-RegularFile -Path $zipPath -Description 'ポータブルZIP'
$zipSha256 = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash
$zipSha256Line = "$zipSha256 *$([IO.Path]::GetFileName($zipPath))" +
    [Environment]::NewLine
Write-NewUtf8File -Path $zipSha256Path -Text $zipSha256Line
Assert-RegularFile `
    -Path $zipSha256Path `
    -Description 'ポータブルZIP外部SHA-256'

$report = [ordered]@{
    schemaVersion = 1
    product = 'Y-TEC Tsumugi Drive'
    version = $productVersion.display
    fileVersion = $productVersion.file
    channel = $productVersion.channel
    outputRoot = $outputFullPath
    zip = [ordered]@{
        path = $zipPath
        length = (Get-Item -LiteralPath $zipPath).Length
        sha256 = $zipSha256
    }
    zipSha256File = [ordered]@{
        path = $zipSha256Path
        length = (Get-Item -LiteralPath $zipSha256Path).Length
        zipSha256 = $zipSha256
        sha256 = (Get-FileHash -LiteralPath $zipSha256Path `
            -Algorithm SHA256).Hash
    }
    repositoryContainsMicrosoftPayload = $false
    executables = $executableVersionInfo
    sbom = $sbomVersionInfo
    fileCount = (Get-ChildItem -LiteralPath $outputFullPath `
        -Recurse -File).Count
}
Write-Output ('TSUMUGI_PORTABLE_PACKAGE_PASS=' +
    ($report | ConvertTo-Json -Depth 4 -Compress))
