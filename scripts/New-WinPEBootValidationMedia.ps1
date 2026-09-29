param(
    [Parameter(Mandatory)]
    [string]$OutputRoot
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$OutputEncoding = [Text.UTF8Encoding]::new($false)

$repoRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$outputFullPath = [IO.Path]::GetFullPath($OutputRoot)
$repoPrefix = $repoRoot.TrimEnd('\') + '\'

if ($outputFullPath.Equals($repoRoot, [StringComparison]::OrdinalIgnoreCase) -or
    $outputFullPath.StartsWith($repoPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Microsoft WinPE/ADK生成物はリポジトリ外だけに作成できます。'
}

$pathRoot = [IO.Path]::GetPathRoot($outputFullPath)
if ([string]::IsNullOrWhiteSpace($pathRoot) -or
    $outputFullPath.TrimEnd('\').Equals(
        $pathRoot.TrimEnd('\'),
        [StringComparison]::OrdinalIgnoreCase)) {
    throw 'ドライブ直下を出力先には指定できません。'
}

if (Test-Path -LiteralPath $outputFullPath) {
    throw "既存の出力先は上書きしません: $outputFullPath"
}

$diagnostic = Join-Path $repoRoot `
    'out\build\msvc-x64\src\MediaBuilder\ytec-winpe-environment.exe'
if (-not (Test-Path -LiteralPath $diagnostic -PathType Leaf)) {
    throw "WinPE環境診断CLIを先にビルドしてください: $diagnostic"
}

$diagnosticText = (& $diagnostic --json | Out-String)
$diagnosticExit = $LASTEXITCODE
if ($diagnosticExit -ne 0) {
    throw "WinPE環境診断が終了コード $diagnosticExit で作成を拒否しました。"
}
$diagnosticReport = $diagnosticText | ConvertFrom-Json
if (-not $diagnosticReport.mediaCreationPermitted -or
    $null -eq $diagnosticReport.selectedCandidateIndex) {
    throw 'WinPE環境診断の作成許可ゲートを通過していません。'
}

$candidate = $diagnosticReport.candidates[
    [int]$diagnosticReport.selectedCandidateIndex]
$adkRoot = [IO.Path]::GetFullPath([string]$candidate.root)
$winpeRoot = Join-Path $adkRoot 'Windows Preinstallation Environment'
$deploymentRoot = Join-Path $adkRoot 'Deployment Tools'
$sourceMedia = Join-Path $winpeRoot 'amd64\Media'
$sourceWim = Join-Path $winpeRoot 'amd64\en-us\winpe.wim'
$oscdimgRoot = Join-Path $deploymentRoot 'amd64\Oscdimg'
$oscdimg = Join-Path $oscdimgRoot 'oscdimg.exe'
$etfsboot = Join-Path $oscdimgRoot 'etfsboot.com'
$efisys2011 = Join-Path $oscdimgRoot 'efisys.bin'
$efisys2023 = Join-Path $oscdimgRoot 'efisys_EX.bin'

foreach ($required in @(
        $sourceMedia,
        $sourceWim,
        $oscdimg,
        $etfsboot,
        $efisys2011,
        $efisys2023)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "必要なADK構成要素が見つかりません: $required"
    }
}

$oscdimgSignature = Get-AuthenticodeSignature -FilePath $oscdimg
if ($oscdimgSignature.Status -ne [Management.Automation.SignatureStatus]::Valid -or
    $oscdimgSignature.SignerCertificate.Subject -notmatch
        '(^|,\s*)O=Microsoft Corporation(,|$)') {
    throw 'Oscdimgの有効なMicrosoft署名を確認できません。'
}

$workingRoot = Join-Path $outputFullPath 'working'
$mediaRoot = Join-Path $workingRoot 'media'
$sourcesRoot = Join-Path $mediaRoot 'sources'
$standardIso = Join-Path $outputFullPath 'YDC-WinPE-amd64-2011CA.iso'
$bootexIso = Join-Path $outputFullPath 'YDC-WinPE-amd64-2023CA.iso'

New-Item -ItemType Directory -Path $outputFullPath | Out-Null
New-Item -ItemType Directory -Path $workingRoot | Out-Null
Copy-Item -LiteralPath $sourceMedia -Destination $mediaRoot -Recurse
if (-not (Test-Path -LiteralPath $sourcesRoot)) {
    New-Item -ItemType Directory -Path $sourcesRoot | Out-Null
}
Copy-Item -LiteralPath $sourceWim `
    -Destination (Join-Path $sourcesRoot 'boot.wim')

function New-ValidationIso {
    param(
        [Parameter(Mandatory)]
        [string]$EfiBootImage,
        [Parameter(Mandatory)]
        [string]$Destination
    )

    $bootData = '-bootdata:2#p0,e,b{0}#pEF,e,b{1}' -f `
        $etfsboot, $EfiBootImage
    & $oscdimg $bootData '-u1' '-udfver102' $mediaRoot $Destination
    if ($LASTEXITCODE -ne 0 -or
        -not (Test-Path -LiteralPath $Destination -PathType Leaf)) {
        throw "OscdimgによるISO作成に失敗しました: $Destination"
    }
}

New-ValidationIso -EfiBootImage $efisys2011 -Destination $standardIso
New-ValidationIso -EfiBootImage $efisys2023 -Destination $bootexIso

$manifest = [ordered]@{
    schemaVersion = 1
    purpose = 'Y-TEC Tsumugi Drive WinPE boot validation only'
    generated = (Get-Date).ToString('o')
    repositoryContainsMicrosoftPayload = $false
    adkVersion = [string]$candidate.deploymentToolsVersion
    dismVersion = [string]$candidate.dismFileVersion
    servicingUpdate = 'KB5101684'
    sourceWim = [ordered]@{
        path = $sourceWim
        length = (Get-Item -LiteralPath $sourceWim).Length
        sha256 = (Get-FileHash -LiteralPath $sourceWim -Algorithm SHA256).Hash
    }
    iso2011Ca = [ordered]@{
        path = $standardIso
        length = (Get-Item -LiteralPath $standardIso).Length
        sha256 = (Get-FileHash -LiteralPath $standardIso -Algorithm SHA256).Hash
    }
    iso2023Ca = [ordered]@{
        path = $bootexIso
        length = (Get-Item -LiteralPath $bootexIso).Length
        sha256 = (Get-FileHash -LiteralPath $bootexIso -Algorithm SHA256).Hash
    }
}

$manifestPath = Join-Path $outputFullPath 'validation-media-manifest.json'
$manifestJson = $manifest | ConvertTo-Json -Depth 5
[IO.File]::WriteAllText(
    $manifestPath,
    $manifestJson,
    [Text.UTF8Encoding]::new($false))

Write-Output "WINPE_VALIDATION_MEDIA_PASS=$manifestPath"
