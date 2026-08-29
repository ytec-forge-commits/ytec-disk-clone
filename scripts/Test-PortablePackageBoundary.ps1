$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$scriptPath = Join-Path $PSScriptRoot 'New-PortablePackage.ps1'
$versionHelperPath = Join-Path $PSScriptRoot 'ProductVersion.ps1'
. $versionHelperPath
$productVersion = Read-YtecProductVersion `
    -Path (Join-Path $repoRoot 'version.json')

foreach ($parsePath in @(
        $versionHelperPath,
        (Join-Path $PSScriptRoot 'Test-ProductVersion.ps1'),
        $scriptPath,
        (Join-Path $PSScriptRoot 'Test-PortablePackageArtifact.ps1'),
        $PSCommandPath)) {
    $tokens = $null
    $parseErrors = $null
    [Management.Automation.Language.Parser]::ParseFile(
        $parsePath,
        [ref]$tokens,
        [ref]$parseErrors) | Out-Null
    if ($parseErrors.Count -ne 0) {
        throw "Portable package script parse failed ($parsePath): $($parseErrors[0].Message)"
    }
}

function Assert-Rejected {
    param(
        [Parameter(Mandatory)]
        [string]$OutputRoot,
        [Parameter(Mandatory)]
        [string]$ExpectedMessage
    )

    try {
        & $scriptPath -OutputRoot $OutputRoot
        throw "拒否されるべき出力先が許可されました: $OutputRoot"
    } catch {
        if ($_.Exception.Message -notlike "*$ExpectedMessage*") {
            throw "想定外の拒否理由です: $($_.Exception.Message)"
        }
    }
}

Assert-Rejected `
    -OutputRoot (Join-Path $repoRoot 'out\forbidden-portable') `
    -ExpectedMessage 'リポジトリ外'
Assert-Rejected `
    -OutputRoot ([IO.Path]::GetPathRoot($repoRoot)) `
    -ExpectedMessage 'ドライブ直下'
Assert-Rejected `
    -OutputRoot $env:TEMP `
    -ExpectedMessage '既存の出力先'

$preflightOutput = Join-Path $env:TEMP `
    ('Y-TEC-Tsumugi-Drive-portable-preflight-' +
        [guid]::NewGuid().ToString('N'))
$preflightZip = $preflightOutput + '.zip'
$preflightZipSha256 = $preflightZip + '.sha256'
$result = & $scriptPath -OutputRoot $preflightOutput
if ($result -notlike 'TSUMUGI_PORTABLE_PREFLIGHT_PASS=*') {
    throw "事前検証の成功マーカーがありません: $result"
}
$preflight = $result.Substring(
    'TSUMUGI_PORTABLE_PREFLIGHT_PASS='.Length) | ConvertFrom-Json

if ($preflight.schemaVersion -ne 1 -or
    $preflight.product -ne 'Y-TEC Tsumugi Drive' -or
    $preflight.version -cne $productVersion.display -or
    $preflight.fileVersion -cne $productVersion.file -or
    $preflight.channel -cne $productVersion.channel -or
    $preflight.buildRequested -ne $false -or
    $preflight.repositoryContainsMicrosoftPayload -ne $false) {
    throw 'ポータブル配布物の事前検証メタデータが不正です。'
}
if ($preflight.outputRoot -ne [IO.Path]::GetFullPath($preflightOutput) -or
    $preflight.zipPath -ne [IO.Path]::GetFullPath($preflightZip) -or
    $preflight.zipSha256Path -ne
        [IO.Path]::GetFullPath($preflightZipSha256)) {
    throw 'フォルダー、ZIP、ZIP外部SHA-256の非上書き出力先が分離されていません。'
}

$requiredFiles = @(
    'windowsApp',
    'environment',
    'winpeCli',
    'winpeGui',
    'builderScript',
    'readme',
    'operationGuide',
    'safetyAndLimitations',
    'privacyAndNetwork',
    'securityReporting',
    'termsOfUse',
    'dataReadme',
    'projectLicense',
    'projectNotice',
    'projectTrademarks',
    'notices',
    'sbom',
    'licenseReadme',
    'lineSeedLicense',
    'zstandardLicense',
    'argon2License')
foreach ($name in $requiredFiles) {
    $file = $preflight.files.$name
    if ($null -eq $file -or
        $file.length -le 0 -or
        $file.sha256 -notmatch '^[0-9A-F]{64}$' -or
        -not (Test-Path -LiteralPath $file.path -PathType Leaf)) {
        throw "配布元ファイルの固定検証が不足しています: $name"
    }
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
foreach ($name in $expectedExecutableMetadata.Keys) {
    $metadata = $expectedExecutableMetadata[$name]
    $actualVersionInfo = Assert-YtecExecutableVersionInfo `
        -Path ([string]$preflight.files.$name.path) `
        -Version $productVersion `
        -OriginalFilename $metadata.originalFilename `
        -FileDescription $metadata.fileDescription
    $reportedVersionInfo = $preflight.executables.$name
    if ($reportedVersionInfo.fileVersion -cne
            $actualVersionInfo.fileVersion -or
        $reportedVersionInfo.productVersion -cne
            $actualVersionInfo.productVersion -or
        $reportedVersionInfo.originalFilename -cne
            $actualVersionInfo.originalFilename -or
        $reportedVersionInfo.fileDescription -cne
            $actualVersionInfo.fileDescription) {
        throw "事前検証のVERSIONINFO報告が実行ファイルと一致しません: $name"
    }
}
if ($preflight.sbom.packageVersion -cne $productVersion.display -or
    $preflight.sbom.documentNamespace -cne
        ('https://github.com/ytec-forge-commits/ytec-disk-clone/sbom/' +
            $productVersion.display)) {
    throw '事前検証のSBOM版報告が製品版正本と一致しません。'
}

$forbiddenExtensions = @(
    '.wim', '.iso', '.cab', '.msi', '.msix', '.vhd', '.vhdx')
foreach ($name in $requiredFiles) {
    $extension = [IO.Path]::GetExtension(
        [string]$preflight.files.$name.path).ToLowerInvariant()
    if ($forbiddenExtensions -contains $extension) {
        throw "事前検証へ配布禁止媒体が混入しました: $name"
    }
}

foreach ($path in @(
        $preflightOutput,
        $preflightZip,
        $preflightZipSha256)) {
    if (Test-Path -LiteralPath $path) {
        throw "事前検証だけで出力が作成されました: $path"
    }
}

function Remove-ExactPortableTestArtifact {
    param(
        [Parameter(Mandatory)]
        [string]$Path
    )

    $temporaryRoot = [IO.Path]::GetFullPath($env:TEMP).TrimEnd('\')
    $candidate = [IO.Path]::GetFullPath($Path)
    $parent = [IO.Path]::GetDirectoryName($candidate).TrimEnd('\')
    $leaf = [IO.Path]::GetFileName($candidate)
    if (-not $parent.Equals(
            $temporaryRoot,
            [StringComparison]::OrdinalIgnoreCase) -or
        $leaf -notmatch
            '^Y-TEC-Tsumugi-Drive-package-ci-[0-9a-f]{32}(?:\.zip(?:\.sha256)?)?$') {
        throw "一時配布物の削除対象が固定境界外です: $candidate"
    }
    if (-not (Test-Path -LiteralPath $candidate)) {
        return
    }
    $item = Get-Item -LiteralPath $candidate -Force
    if (($item.Attributes -band
            [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "一時配布物のreparse pointは削除しません: $candidate"
    }
    for ($attempt = 1; $attempt -le 5; ++$attempt) {
        try {
            if ($item.PSIsContainer) {
                Remove-Item -LiteralPath $candidate -Recurse -Force
            } else {
                Remove-Item -LiteralPath $candidate -Force
            }
            return
        } catch {
            if ($attempt -eq 5) {
                throw
            }
            # Antivirus scanners can briefly hold a newly created executable.
            Start-Sleep -Milliseconds (100 * $attempt)
        }
    }
}

$collisionRoot = Join-Path $env:TEMP `
    ('Y-TEC-Tsumugi-Drive-package-ci-' +
        [guid]::NewGuid().ToString('N'))
$collisionZip = $collisionRoot + '.zip'
$collisionZipSha256 = $collisionZip + '.sha256'
$collisionBytes = [Text.Encoding]::ASCII.GetBytes(
    'existing checksum must remain unchanged')
$collisionStream = [IO.File]::Open(
    $collisionZipSha256,
    [IO.FileMode]::CreateNew,
    [IO.FileAccess]::Write,
    [IO.FileShare]::None)
try {
    $collisionStream.Write($collisionBytes, 0, $collisionBytes.Length)
    $collisionStream.Flush($true)
} finally {
    $collisionStream.Dispose()
}
try {
    Assert-Rejected `
        -OutputRoot $collisionRoot `
        -ExpectedMessage '既存の出力先'
    if (-not [Linq.Enumerable]::SequenceEqual(
            [byte[]]$collisionBytes,
            [byte[]][IO.File]::ReadAllBytes($collisionZipSha256)) -or
        (Test-Path -LiteralPath $collisionRoot) -or
        (Test-Path -LiteralPath $collisionZip)) {
        throw '既存のZIP外部SHA-256または共存出力が変更されました。'
    }
} finally {
    Remove-ExactPortableTestArtifact -Path $collisionZipSha256
}

$artifactRoot = Join-Path $env:TEMP `
    ('Y-TEC-Tsumugi-Drive-package-ci-' +
        [guid]::NewGuid().ToString('N'))
$artifactZip = $artifactRoot + '.zip'
$artifactZipSha256 = $artifactZip + '.sha256'
try {
    $packageResult = & $scriptPath `
        -OutputRoot $artifactRoot `
        -BuildPackage
    $packageMarker = @($packageResult |
        Where-Object {
            $_ -like 'TSUMUGI_PORTABLE_PACKAGE_PASS=*'
        } |
        Select-Object -Last 1)
    if ($packageMarker.Count -ne 1) {
        throw '実配布ZIPの作成成功マーカーがありません。'
    }
    $package = $packageMarker[0].Substring(
        'TSUMUGI_PORTABLE_PACKAGE_PASS='.Length) | ConvertFrom-Json
    if ($package.outputRoot -ne [IO.Path]::GetFullPath($artifactRoot) -or
        $package.version -cne $productVersion.display -or
        $package.fileVersion -cne $productVersion.file -or
        $package.channel -cne $productVersion.channel -or
        $package.zip.path -ne [IO.Path]::GetFullPath($artifactZip) -or
        $package.zipSha256File.path -ne
            [IO.Path]::GetFullPath($artifactZipSha256) -or
        $package.zipSha256File.zipSha256 -cne $package.zip.sha256 -or
        $package.zipSha256File.sha256 -cne
            (Get-FileHash -LiteralPath $artifactZipSha256 `
                -Algorithm SHA256).Hash -or
        $package.repositoryContainsMicrosoftPayload -ne $false) {
        throw '実配布ZIPの完成報告が要求した出力と一致しません。'
    }
    $packagedMediaOutput = Join-Path $env:TEMP `
        ('Y-TEC-Tsumugi-Drive-packaged-media-preflight-' +
            [guid]::NewGuid().ToString('N'))
    $packagedIso = $packagedMediaOutput + '.iso'
    $packagedBuilder = Join-Path $artifactRoot `
        'tools\New-WinPEAppValidationMedia.ps1'
    $packagedDiagnostic = Join-Path $artifactRoot `
        'tools\ytec-winpe-environment.exe'
    $packagedDiagnosticText = (& $packagedDiagnostic --json | Out-String)
    $packagedDiagnosticExit = $LASTEXITCODE
    try {
        $packagedDiagnosticReport =
            $packagedDiagnosticText | ConvertFrom-Json
    } catch {
        throw "配布フォルダー内のWinPE環境診断JSONを解析できません: $($_.Exception.Message)"
    }
    $packagedDiagnosticCandidates =
        @($packagedDiagnosticReport.candidates)
    $packagedSelectedCandidateIndex =
        $packagedDiagnosticReport.selectedCandidateIndex
    $packagedSelectedCandidate = $null
    if (($packagedSelectedCandidateIndex -is [int] -or
            $packagedSelectedCandidateIndex -is [long]) -and
        $packagedSelectedCandidateIndex -ge 0 -and
        $packagedSelectedCandidateIndex -lt
            $packagedDiagnosticCandidates.Count) {
        $packagedSelectedCandidate =
            $packagedDiagnosticCandidates[$packagedSelectedCandidateIndex]
    }
    $candidateBooleanFields = @(
        'deploymentToolsPresent',
        'winpeAddonPresent',
        'microsoftToolsTrusted',
        'bootexSupported',
        'baseLayoutReady',
        'bootexLayoutReady',
        'oscdimgServicingPatchApplied',
        'dismServicingPatchApplied',
        'versionAndServicingVerified',
        'mediaCreationPermitted')
    $packagedSelectedCandidateReady =
        $null -ne $packagedSelectedCandidate
    if ($packagedSelectedCandidateReady) {
        foreach ($field in $candidateBooleanFields) {
            $value = $packagedSelectedCandidate.$field
            if ($value -isnot [bool] -or $value -ne $true) {
                $packagedSelectedCandidateReady = $false
            }
        }
    }
    $packagedAdkReady = $packagedDiagnosticExit -eq 0 -and
        ($packagedDiagnosticReport.schemaVersion -is [int] -or
            $packagedDiagnosticReport.schemaVersion -is [long]) -and
        $packagedDiagnosticReport.schemaVersion -eq 1 -and
        $packagedDiagnosticReport.architecture -is [string] -and
        $packagedDiagnosticReport.architecture -ceq 'amd64' -and
        $packagedDiagnosticReport.baseLayoutReady -is [bool] -and
        $packagedDiagnosticReport.baseLayoutReady -eq $true -and
        $packagedDiagnosticReport.bootexLayoutReady -is [bool] -and
        $packagedDiagnosticReport.bootexLayoutReady -eq $true -and
        $packagedDiagnosticReport.mediaCreationPermitted -is [bool] -and
        $packagedDiagnosticReport.mediaCreationPermitted -eq $true -and
        $packagedSelectedCandidateReady
    $packagedAdkMissingOnly = $packagedDiagnosticExit -eq 2 -and
        ($packagedDiagnosticReport.schemaVersion -is [int] -or
            $packagedDiagnosticReport.schemaVersion -is [long]) -and
        $packagedDiagnosticReport.schemaVersion -eq 1 -and
        $packagedDiagnosticReport.architecture -is [string] -and
        $packagedDiagnosticReport.architecture -ceq 'amd64' -and
        $packagedDiagnosticReport.baseLayoutReady -is [bool] -and
        $packagedDiagnosticReport.baseLayoutReady -eq $false -and
        $packagedDiagnosticReport.bootexLayoutReady -is [bool] -and
        $packagedDiagnosticReport.bootexLayoutReady -eq $false -and
        $packagedDiagnosticReport.mediaCreationPermitted -is [bool] -and
        $packagedDiagnosticReport.mediaCreationPermitted -eq $false -and
        $null -eq $packagedDiagnosticReport.selectedCandidateIndex
    if ($packagedAdkMissingOnly -and
        $packagedDiagnosticCandidates.Count -eq 0) {
        $packagedAdkMissingOnly = $false
    }
    if ($packagedAdkMissingOnly) {
        $missingPrerequisiteCodes = @(
            'ADK_ROOT_NOT_FOUND',
            'ADK_DEPLOYMENT_TOOLS_MISSING',
            'ADK_WINPE_ADDON_MISSING',
            'ADK_DISM_MISSING',
            'ADK_OSCDIMG_MISSING',
            'ADK_COPYPE_MISSING',
            'ADK_MAKEWINPEMEDIA_MISSING',
            'ADK_BASE_WINPE_WIM_MISSING')
        $requiredFalseFields = @(
            'baseLayoutReady',
            'bootexLayoutReady',
            'oscdimgServicingPatchApplied',
            'dismServicingPatchApplied',
            'versionAndServicingVerified',
            'mediaCreationPermitted')
        foreach ($candidate in $packagedDiagnosticCandidates) {
            foreach ($field in $candidateBooleanFields) {
                $value = $candidate.$field
                if ($value -isnot [bool]) {
                    $packagedAdkMissingOnly = $false
                }
            }
            foreach ($field in $requiredFalseFields) {
                if ($candidate.$field -ne $false) {
                    $packagedAdkMissingOnly = $false
                }
            }
            $candidateDiagnostics = @($candidate.diagnostics)
            if ($candidateDiagnostics.Count -eq 0 -or
                $candidateDiagnostics.Count -gt 5) {
                $packagedAdkMissingOnly = $false
            }
            $seenDiagnosticCodes = @{}
            foreach ($candidateDiagnostic in $candidateDiagnostics) {
                if ($candidateDiagnostic.code -is [string]) {
                    if ($seenDiagnosticCodes.ContainsKey(
                            [string]$candidateDiagnostic.code)) {
                        $packagedAdkMissingOnly = $false
                    } else {
                        $seenDiagnosticCodes.Add(
                            [string]$candidateDiagnostic.code, $true)
                    }
                }
                if ($candidateDiagnostic.severity -isnot [string] -or
                    $candidateDiagnostic.severity -cne 'エラー' -or
                    $candidateDiagnostic.code -isnot [string] -or
                    $candidateDiagnostic.code -cnotin
                        $missingPrerequisiteCodes -or
                    ($candidateDiagnostic.nativeCode -isnot [int] -and
                        $candidateDiagnostic.nativeCode -isnot [long]) -or
                        $candidateDiagnostic.nativeCode -ne 2) {
                    $packagedAdkMissingOnly = $false
                }
            }
            if ($seenDiagnosticCodes.ContainsKey('ADK_ROOT_NOT_FOUND')) {
                if ($candidateDiagnostics.Count -ne 1) {
                    $packagedAdkMissingOnly = $false
                }
                foreach ($field in $candidateBooleanFields) {
                    if ($candidate.$field -ne $false) {
                        $packagedAdkMissingOnly = $false
                    }
                }
            } else {
                $deploymentRootMissing = $seenDiagnosticCodes.ContainsKey(
                    'ADK_DEPLOYMENT_TOOLS_MISSING')
                $dismMissing = $seenDiagnosticCodes.ContainsKey(
                    'ADK_DISM_MISSING')
                $oscdimgMissing = $seenDiagnosticCodes.ContainsKey(
                    'ADK_OSCDIMG_MISSING')
                if ($deploymentRootMissing) {
                    if ($dismMissing -or $oscdimgMissing -or
                        $candidate.deploymentToolsPresent -ne $false -or
                        $candidate.microsoftToolsTrusted -ne $false) {
                        $packagedAdkMissingOnly = $false
                    }
                } else {
                    $deploymentReady = -not ($dismMissing -or $oscdimgMissing)
                    if ($candidate.deploymentToolsPresent -ne
                            $deploymentReady -or
                        $candidate.microsoftToolsTrusted -ne
                            $deploymentReady) {
                        $packagedAdkMissingOnly = $false
                    }
                }

                $winpeRootMissing = $seenDiagnosticCodes.ContainsKey(
                    'ADK_WINPE_ADDON_MISSING')
                $copypeMissing = $seenDiagnosticCodes.ContainsKey(
                    'ADK_COPYPE_MISSING')
                $makeMediaMissing = $seenDiagnosticCodes.ContainsKey(
                    'ADK_MAKEWINPEMEDIA_MISSING')
                $baseWimMissing = $seenDiagnosticCodes.ContainsKey(
                    'ADK_BASE_WINPE_WIM_MISSING')
                if ($winpeRootMissing) {
                    if ($copypeMissing -or $makeMediaMissing -or
                        $baseWimMissing -or
                        $candidate.winpeAddonPresent -ne $false -or
                        $candidate.bootexSupported -ne $false) {
                        $packagedAdkMissingOnly = $false
                    }
                } else {
                    $winpeAddonReady = -not (
                        $copypeMissing -or $makeMediaMissing -or
                        $baseWimMissing)
                    $bootexReady = -not $makeMediaMissing
                    if ($candidate.winpeAddonPresent -ne $winpeAddonReady -or
                        $candidate.bootexSupported -ne $bootexReady) {
                        $packagedAdkMissingOnly = $false
                    }
                }
            }
        }
    }
    if (-not $packagedAdkReady -and -not $packagedAdkMissingOnly) {
        throw "配布フォルダー内のWinPE環境診断が媒体境界テストの実行条件を満たしません（終了コード $packagedDiagnosticExit）。"
    }

    $packagedBuilderArguments = @{
        OutputRoot = $packagedMediaOutput
        FinalIsoPath = $packagedIso
        DiagnosticPath = $packagedDiagnostic
        WinPEAppPath = Join-Path $artifactRoot `
            'winpe\ytec-winpe-app.exe'
        WinPEGuiPath = Join-Path $artifactRoot `
            'winpe\ytec-winpe-gui.exe'
    }
    if ($packagedAdkReady) {
        $packagedMediaResult = & $packagedBuilder `
            @packagedBuilderArguments
        if ($packagedMediaResult -notlike
            'WINPE_APP_MEDIA_PREFLIGHT_PASS=*') {
            throw '配布フォルダー内の媒体Builderが事前検証を完走できません。'
        }
        $packagedMediaPreflight = $packagedMediaResult.Substring(
            'WINPE_APP_MEDIA_PREFLIGHT_PASS='.Length) | ConvertFrom-Json
        $expectedPackagedLicenseFiles = [ordered]@{
            projectLicense = 'LICENSE'
            projectNotice = 'NOTICE'
            notices = 'THIRD-PARTY-NOTICES.txt'
            sbom = 'SBOM.spdx.json'
            licenseReadme = 'licenses\README.md'
            lineSeedLicense = 'licenses\LINE-Seed-JP-OFL-1.1.txt'
            zstandardLicense = 'licenses\Zstandard-BSD-3-Clause.txt'
            argon2License = 'licenses\Argon2-Apache-2.0.txt'
        }
        foreach ($name in $expectedPackagedLicenseFiles.Keys) {
            $packagedSource = Join-Path $artifactRoot `
                $expectedPackagedLicenseFiles[$name]
            $report = $packagedMediaPreflight.thirdPartyPayload.$name
            if ($null -eq $report -or
                $report.path -cne [IO.Path]::GetFullPath($packagedSource) -or
                $report.sha256 -cne
                    (Get-FileHash -LiteralPath $packagedSource `
                        -Algorithm SHA256).Hash) {
                throw "配布フォルダー内Builderのライセンス正本が不正です: $name"
            }
        }
    } else {
        try {
            & $packagedBuilder @packagedBuilderArguments | Out-Null
            throw 'ADK未導入環境で配布フォルダー内の媒体Builderが許可されました。'
        } catch {
            if ($_.Exception.Message -cne
                'WinPE環境診断が終了コード 2 で作成を拒否しました。') {
                throw "配布フォルダー内BuilderのADK未導入時エラーが想定外です: $($_.Exception.Message)"
            }
        }
    }
    foreach ($unexpectedOutput in @(
            $packagedMediaOutput,
            $packagedIso,
            ($packagedIso + '.manifest.json'))) {
        if (Test-Path -LiteralPath $unexpectedOutput) {
            throw "媒体事前検証だけで出力が作成されました: $unexpectedOutput"
        }
    }
    $artifactResult = & (Join-Path $PSScriptRoot `
        'Test-PortablePackageArtifact.ps1') `
        -PackageRoot $artifactRoot `
        -ZipPath $artifactZip
    if ($artifactResult -notlike 'TSUMUGI_PORTABLE_ARTIFACT_PASS=*') {
        throw "実配布ZIPの監査成功マーカーがありません: $artifactResult"
    }
    $artifactReport = $artifactResult.Substring(
        'TSUMUGI_PORTABLE_ARTIFACT_PASS='.Length) | ConvertFrom-Json
    if ($artifactReport.zipSha256 -cne $package.zip.sha256 -or
        $artifactReport.version -cne $productVersion.display -or
        $artifactReport.fileVersion -cne $productVersion.file -or
        $artifactReport.channel -cne $productVersion.channel -or
        $artifactReport.zipSha256File.path -cne
            [IO.Path]::GetFullPath($artifactZipSha256) -or
        $artifactReport.zipSha256File.zipSha256 -cne
            $package.zip.sha256 -or
        $artifactReport.zipSha256File.sha256 -cne
            $package.zipSha256File.sha256) {
        throw 'ZIP外部SHA-256の生成報告と実体監査が一致しません。'
    }

    $hashListPath = Join-Path $artifactRoot 'SHA256SUMS.txt'
    $hashListLines = @([IO.File]::ReadAllLines(
            $hashListPath,
            [Text.UTF8Encoding]::new($false, $true)))
    if ($hashListLines.Count -lt 2) {
        throw 'SHA256SUMS.txt重複検査の前提行が不足しています。'
    }
    $hashListLines[$hashListLines.Count - 1] = $hashListLines[0]
    [IO.File]::WriteAllLines(
        $hashListPath,
        $hashListLines,
        [Text.UTF8Encoding]::new($false))
    try {
        & (Join-Path $PSScriptRoot `
            'Test-PortablePackageArtifact.ps1') `
            -PackageRoot $artifactRoot `
            -ZipPath $artifactZip | Out-Null
        throw 'SHA256SUMS.txtの重複パスが許可されました。'
    } catch {
        if ($_.Exception.Message -notlike '*重複パス*') {
            throw "SHA256SUMS.txt重複時の拒否理由が想定外です: $($_.Exception.Message)"
        }
    }
} finally {
    Remove-ExactPortableTestArtifact -Path $artifactZipSha256
    Remove-ExactPortableTestArtifact -Path $artifactZip
    Remove-ExactPortableTestArtifact -Path $artifactRoot
}

Write-Output 'Portable package boundary tests: PASS'
