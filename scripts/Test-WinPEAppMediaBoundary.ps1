$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$scriptPath = Join-Path $PSScriptRoot 'New-WinPEAppValidationMedia.ps1'
$elevatedWrapper = Join-Path $PSScriptRoot 'Invoke-WinPEAppMediaElevated.ps1'
$activeMediaHeader = Join-Path $repoRoot `
    'src\WinPEApp\include\ytec\winpeapp\active_rescue_media.h'
$activeMediaSource = Join-Path $repoRoot `
    'src\WinPEApp\src\active_rescue_media.cpp'
$winpeMainSource = Join-Path $repoRoot 'src\WinPEApp\src\main.cpp'

foreach ($file in @($scriptPath, $elevatedWrapper)) {
    $tokens = $null
    $parseErrors = $null
    [Management.Automation.Language.Parser]::ParseFile(
        $file,
        [ref]$tokens,
        [ref]$parseErrors) | Out-Null
    if ($parseErrors.Count -ne 0) {
        throw "WinPEApp media script parse failed: $($parseErrors[0].Message)"
    }
    if ((Get-Content -LiteralPath $file -Raw) -match
        '(?i)\butf8NoBOM\b') {
        throw "Windows PowerShell 5.1非互換のencoding指定があります: $file"
    }
}

$builderSource = Get-Content -LiteralPath $scriptPath -Raw
foreach ($requiredSafetyMarker in @(
        '0x80000008',
        'sourceWasWimProjection',
        'microsoftSignatureVerified',
        "subsystem = 'EFI Application'",
        'sourceHashBefore -ne $sourceHashAfter')) {
    if (-not $builderSource.Contains($requiredSafetyMarker)) {
        throw "WIM投影ファイル検証の安全条件がありません: $requiredSafetyMarker"
    }
}

foreach ($requiredThirdPartyPayloadMarker in @(
        "'LICENSE'",
        "'NOTICE'",
        "'THIRD-PARTY-NOTICES.txt'",
        "'SBOM.spdx.json'",
        "'licenses\README.md'",
        "'LINE-Seed-JP-OFL-1.1.txt'",
        "'Zstandard-BSD-3-Clause.txt'",
        "'Argon2-Apache-2.0.txt'",
        '$thirdPartyPayloadReport[$entry.Key].sha256',
        'thirdPartyPayload = $thirdPartyPayloadManifest')) {
    if (-not $builderSource.Contains($requiredThirdPartyPayloadMarker)) {
        throw "WinPE自己作成payloadのライセンス境界がありません: $requiredThirdPartyPayloadMarker"
    }
}

foreach ($requiredPersistentPayloadMarker in @(
        '$mediaPayloadData = Join-Path $mediaPayloadRoot ''data''',
        '$mountedData = Join-Path $payloadRoot ''data''',
        'mediaRootProductPayload = $mediaRootProductPayloadManifest',
        'USB media keeps active.checkpoint across reboot.',
        'ISO/CD-ROM media does not support persistent resume.',
        '%SYSTEMDRIVE%\YtecDiskClone\ytec-winpe-app.exe, --launch-gui-from-media')) {
    if (-not $builderSource.Contains($requiredPersistentPayloadMarker)) {
        throw "WinPEのEXE隣persistent data／媒体root launcher契約がありません: $requiredPersistentPayloadMarker"
    }
}

$activeMediaContractSource =
    (Get-Content -LiteralPath $activeMediaHeader -Raw) +
    (Get-Content -LiteralPath $activeMediaSource -Raw)
foreach ($requiredActiveMediaMarker in @(
        'ActiveRescueMediaStorageObservation',
        'resolve_active_rescue_media_storage',
        'query_active_rescue_media_storage_with_windows_apis',
        'marker_identity_from_open_handle',
        'matching_paths.size() != 1U',
        'DRIVE_FIXED',
        'DRIVE_REMOVABLE',
        'DRIVE_CDROM',
        'FILE_FLAG_OPEN_REPARSE_POINT',
        'GetFileInformationByHandleEx')) {
    if (-not $activeMediaContractSource.Contains($requiredActiveMediaMarker)) {
        throw "WinPE起動媒体のopened-handle一意照合契約がありません: $requiredActiveMediaMarker"
    }
}

$winpeMain = Get-Content -LiteralPath $winpeMainSource -Raw
foreach ($requiredLauncherMarker in @(
        '--launch-gui-from-media',
        'query_active_rescue_media_storage_with_windows_apis',
        'open_and_verify_normal_path',
        'read_marker_from_held_handle',
        'fresh_storage',
        'GetFinalPathNameByHandleW',
        'FILE_FLAG_OPEN_REPARSE_POINT',
        'CreateProcessW',
        'WaitForSingleObject',
        'GetExitCodeProcess')) {
    if (-not $winpeMain.Contains($requiredLauncherMarker)) {
        throw "WinPE媒体root GUI launcherの安全条件がありません: $requiredLauncherMarker"
    }
}
if ($winpeMain -match '\b(?:ShellExecute|WinExec)\w*\s*\(') {
    throw 'WinPE媒体root GUI launcherは検証済みCreateProcess以外を使用できません。'
}

foreach ($requiredUsbInitializationMarker in @(
        '$script:RescueUsbMinimumBytes = [UInt64](8GB)',
        '$script:RescueUsbBootPartitionBytes = [UInt64](4GB)',
        'function Get-UsbPartitionsAllowEmpty',
        'CmdletizationQuery_NotFound_DiskNumber,Get-Partition',
        'function Test-UsbDriveLetterAvailable',
        'function Select-UsbDriveLetter',
        'Get-Volume',
        '$preparedTarget.drive',
        'WINPE_APP_USB_DRIVE=',
        'function Initialize-VerifiedUsbTarget',
        'function Get-CanonicalUsbLayout',
        'function Get-CanonicalUsbLayoutDigest',
        'function Assert-UsbIdentityAndLayout',
        'function New-RescueUsbPartition',
        'function Format-RescueUsbPartition',
        'Clear-Disk',
        '-InputObject $before.disk',
        'Update-Disk',
        'Initialize-Disk',
        'Set-Disk',
        '-PartitionStyle MBR',
        'New-Partition',
        '-SizeBytes $script:RescueUsbBootPartitionBytes',
        '-UseMaximumSize',
        'Format-Volume',
        '-FileSystem FAT32',
        '-FileSystem $DataFileSystem',
        'function Get-VerifiedOwnedUsbMedia',
        'function New-RescueUsbOwnershipManifest',
        'function Invoke-RescueUsbBootUpdate',
        'DATA_TREE_SCAN_FAILED',
        'dataPreservation = [ordered]@{')) {
    if (-not $builderSource.Contains($requiredUsbInitializationMarker)) {
        throw "対象限定USB自動初期化の安全条件がありません: $requiredUsbInitializationMarker"
    }
}
$builderTokens = $null
$builderParseErrors = $null
$builderAst = [Management.Automation.Language.Parser]::ParseFile(
    $scriptPath,
    [ref]$builderTokens,
    [ref]$builderParseErrors)
foreach ($singleUsbWriter in @(
        'Clear-Disk',
        'Initialize-Disk',
        'Set-Disk',
        'New-Partition',
        'Format-Volume')) {
    $writerCommands = @($builderAst.FindAll(
        {
            param($node)
            $node -is [Management.Automation.Language.CommandAst] -and
                $node.GetCommandName() -eq $singleUsbWriter
        },
        $true))
    if ($writerCommands.Count -ne 1) {
        throw "USB自動初期化の${singleUsbWriter}は監査済み1箇所だけに制限します。"
    }
}
if ($builderSource -match '(?i)/UFD\s+/F') {
    throw 'MakeWinPEMedia /UFDはデータ領域を破壊するため使用できません。'
}

$partitionHelperAst = $builderAst.Find(
    {
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -eq 'Get-UsbPartitionsAllowEmpty'
    },
    $true)
if ($null -eq $partitionHelperAst) {
    throw '空USBパーティション照会ヘルパーを抽出できません。'
}
Invoke-Expression $partitionHelperAst.Extent.Text

$script:verifiedEmptyUsbCount = 0
function Get-VerifiedUsbDisk {
    param(
        [int]$DiskNumber,
        [UInt64]$SizeBytes,
        [AllowEmptyString()]
        [string]$SerialSuffix,
        [string]$DeviceInstanceId
    )

    $script:verifiedEmptyUsbCount++
    return [ordered]@{ diskNumber = 3 }
}
function Get-Partition {
    [CmdletBinding()]
    param([int]$DiskNumber)

    $record = [Management.Automation.ErrorRecord]::new(
        [InvalidOperationException]::new('no partitions'),
        'CmdletizationQuery_NotFound_DiskNumber',
        [Management.Automation.ErrorCategory]::ObjectNotFound,
        $DiskNumber)
    $PSCmdlet.ThrowTerminatingError($record)
}
$emptyPartitions = @(
    Get-UsbPartitionsAllowEmpty `
        -DiskNumber 3 `
        -SizeBytes 62136188928 `
        -SerialSuffix '050490C0' `
        -DeviceInstanceId 'USB\MOCK'
)
if ($emptyPartitions.Count -ne 0 -or
    $script:verifiedEmptyUsbCount -ne 1) {
    throw 'パーティション0件のObjectNotFoundを安全に空として扱えません。'
}

function Get-Partition {
    [CmdletBinding()]
    param([int]$DiskNumber)

    $record = [Management.Automation.ErrorRecord]::new(
        [InvalidOperationException]::new('provider failure'),
        'UnexpectedProviderFailure',
        [Management.Automation.ErrorCategory]::InvalidOperation,
        $DiskNumber)
    $PSCmdlet.ThrowTerminatingError($record)
}
try {
    Get-UsbPartitionsAllowEmpty `
        -DiskNumber 3 `
        -SizeBytes 62136188928 `
        -SerialSuffix '050490C0' `
        -DeviceInstanceId 'USB\MOCK' | Out-Null
    throw '想定外のパーティション照会エラーが許可されました。'
} catch {
    if ($_.FullyQualifiedErrorId -notlike
        'UnexpectedProviderFailure,Get-Partition*') {
        throw
    }
}
Remove-Item Function:\Get-Partition
Remove-Item Function:\Get-VerifiedUsbDisk
Remove-Item Function:\Get-UsbPartitionsAllowEmpty

$driveSelectorAst = $builderAst.Find(
    {
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -eq 'Select-UsbDriveLetter'
    },
    $true)
if ($null -eq $driveSelectorAst) {
    throw 'USBドライブ文字選択ヘルパーを抽出できません。'
}
Invoke-Expression $driveSelectorAst.Extent.Text
function Test-UsbDriveLetterAvailable {
    param([string]$Drive)
    return $Drive -eq 'K:'
}
$fallbackDrive = Select-UsbDriveLetter -PreferredDrive 'J:'
if ($fallbackDrive -ne 'K:') {
    throw "使用中J:から未使用K:へ切り替えられません: $fallbackDrive"
}
function Test-UsbDriveLetterAvailable {
    return $false
}
try {
    Select-UsbDriveLetter -PreferredDrive 'J:' | Out-Null
    throw '全ドライブ文字が使用中でもUSB割当が許可されました。'
} catch {
    if ($_.Exception.Message -notlike '*未使用ドライブ文字がありません*') {
        throw
    }
}
Remove-Item Function:\Test-UsbDriveLetterAvailable
Remove-Item Function:\Select-UsbDriveLetter

foreach ($functionName in @(
        'Assert-SafeMediaRelativePath',
        'Get-CanonicalUsbLayoutDigest',
        'Get-BoundedMediaTreeManifest',
        'Assert-MediaTreeManifestEqual',
        'Get-PrivateDataTreeSummary')) {
    $functionAst = $builderAst.Find(
        {
            param($node)
            $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
                $node.Name -eq $functionName
        },
        $true)
    if ($null -eq $functionAst) {
        throw "媒体tree境界ヘルパーを抽出できません: $functionName"
    }
    Invoke-Expression $functionAst.Extent.Text
}
$script:MaximumDataFileCount = 262144
$script:MaximumDataPathCharacters = [UInt64](64MB)
$script:MaximumDataLogicalBytes = [UInt64](2TB)
$wireLayout = [ordered]@{
    diskStyle = 'MBR'
    partitions = @(
        [ordered]@{
            number = 1
            style = 'MBR'
            type = 'FAT32 LBA'
            offsetBytes = [UInt64]1048576
            sizeBytes = [UInt64]4294967296
            bootable = $true
        },
        [ordered]@{
            number = 2
            style = 'MBR'
            type = 'IFS'
            offsetBytes = [UInt64]4296015872
            sizeBytes = [UInt64]30063722496
            bootable = $false
        })
}
if ((Get-CanonicalUsbLayoutDigest -LayoutValue $wireLayout) -cne
    'FAEA8977FDFD7359207E3F4935AFE1542F59329B93F1089737517D7F0B00DBDD') {
    throw 'PowerShell canonical layout digestがC++ wire契約と一致しません。'
}
$treeTestRoot = Join-Path ([IO.Path]::GetTempPath()) `
    ('ytec-rescue-tree-' + [Guid]::NewGuid().ToString('N'))
$privateFixtureName = 'PRIVATE_CUSTOMER_CASE_9F2A.txt'
try {
    New-Item -ItemType Directory -Path $treeTestRoot | Out-Null
    $fixturePath = Join-Path $treeTestRoot $privateFixtureName
    [IO.File]::WriteAllText(
        $fixturePath,
        'before',
        [Text.UTF8Encoding]::new($false))
    $privateBefore = Get-BoundedMediaTreeManifest `
        -Root $treeTestRoot `
        -MaximumFileCount 16 `
        -MaximumPathCharacters 4096 `
        -MaximumLogicalBytes 4096 `
        -FileSystem exFAT `
        -PrivacyPreservingSummary `
        -RedactPaths
    $privateJson = $privateBefore | ConvertTo-Json -Compress
    if ($privateJson.Contains($privateFixtureName) -or
        $null -ne $privateBefore.PSObject.Properties['files'] -or
        $null -ne $privateBefore.PSObject.Properties['directories'] -or
        $privateBefore.rootDigest -notmatch '^[0-9A-F]{64}$') {
        throw '保持データsummaryがファイル名を公開するかdigest形式が不正です。'
    }
    $privateSame = Get-BoundedMediaTreeManifest `
        -Root $treeTestRoot `
        -MaximumFileCount 16 `
        -MaximumPathCharacters 4096 `
        -MaximumLogicalBytes 4096 `
        -FileSystem exFAT `
        -PrivacyPreservingSummary `
        -RedactPaths
    Assert-MediaTreeManifestEqual `
        -Expected $privateBefore `
        -Observed $privateSame `
        -Description 'synthetic保持データ'
    [IO.File]::WriteAllText(
        $fixturePath,
        'after',
        [Text.UTF8Encoding]::new($false))
    $privateAfter = Get-BoundedMediaTreeManifest `
        -Root $treeTestRoot `
        -MaximumFileCount 16 `
        -MaximumPathCharacters 4096 `
        -MaximumLogicalBytes 4096 `
        -FileSystem exFAT `
        -PrivacyPreservingSummary `
        -RedactPaths
    try {
        Assert-MediaTreeManifestEqual `
            -Expected $privateBefore `
            -Observed $privateAfter `
            -Description 'synthetic保持データ'
        throw '保持データ内容変更がdigest比較を通過しました。'
    } catch {
        if ($_.Exception.Message -notlike '*変化しました*') {
            throw
        }
    }
    [IO.File]::WriteAllText(
        (Join-Path $treeTestRoot 'second.bin'),
        '2',
        [Text.UTF8Encoding]::new($false))
    try {
        Get-BoundedMediaTreeManifest `
            -Root $treeTestRoot `
            -MaximumFileCount 1 `
            -MaximumPathCharacters 4096 `
            -MaximumLogicalBytes 4096 `
            -FileSystem exFAT `
            -PrivacyPreservingSummary `
            -RedactPaths | Out-Null
        throw '媒体treeの列挙件数上限超過が許可されました。'
    } catch {
        if ($_.Exception.Message -notlike '*列挙件数*') {
            throw
        }
    }
    try {
        Get-BoundedMediaTreeManifest `
            -Root $treeTestRoot `
            -MaximumFileCount 16 `
            -MaximumPathCharacters 1 `
            -MaximumLogicalBytes 4096 `
            -FileSystem exFAT `
            -PrivacyPreservingSummary `
            -RedactPaths | Out-Null
        throw '媒体treeの総パス長上限超過が許可されました。'
    } catch {
        if ($_.Exception.Message -notlike '*総パス長*') {
            throw
        }
    }
    try {
        Get-BoundedMediaTreeManifest `
            -Root $treeTestRoot `
            -MaximumFileCount 16 `
            -MaximumPathCharacters 4096 `
            -MaximumLogicalBytes 1 `
            -FileSystem exFAT `
            -PrivacyPreservingSummary `
            -RedactPaths | Out-Null
        throw '媒体treeの総論理バイト上限超過が許可されました。'
    } catch {
        if ($_.Exception.Message -notlike '*総論理バイト*') {
            throw
        }
    }
    foreach ($invalidRelativePath in @('bad?.txt', 'CON.txt')) {
        try {
            Assert-SafeMediaRelativePath -RelativePath $invalidRelativePath
            throw "安全でないWindows名が許可されました: $invalidRelativePath"
        } catch {
            if ($_.Exception.Message -notlike '*安全に扱えない名前*') {
                throw
            }
        }
    }

    function Get-FileHash {
        param([string]$LiteralPath, [string]$Algorithm)
        throw [IO.IOException]::new(
            "provider leaked $privateFixtureName from $LiteralPath")
    }
    try {
        Get-PrivateDataTreeSummary `
            -Target ([ordered]@{
                dataRoot = $treeTestRoot
                dataFileSystem = 'exFAT'
            }) `
            -FsutilPath 'unused-for-exfat' | Out-Null
        throw 'data provider failureが固定privacy errorへ変換されませんでした。'
    } catch {
        $capturedError = (($_ | Out-String) + $_.Exception.ToString())
        if ($capturedError.Contains($privateFixtureName) -or
            $capturedError -notlike '*DATA_TREE_SCAN_FAILED*') {
            throw '保持データのprovider errorからbasenameが漏れるかstable codeがありません。'
        }
    } finally {
        Remove-Item Function:\Get-FileHash -ErrorAction SilentlyContinue
    }
} finally {
    if (Test-Path -LiteralPath $treeTestRoot) {
        $resolvedTreeTestRoot = [IO.Path]::GetFullPath($treeTestRoot)
        $expectedPrefix = [IO.Path]::GetFullPath(
            [IO.Path]::GetTempPath()).TrimEnd('\') + '\'
        if (-not $resolvedTreeTestRoot.StartsWith(
                $expectedPrefix,
                [StringComparison]::OrdinalIgnoreCase)) {
            throw 'synthetic媒体treeのcleanup先が一時領域外です。'
        }
        Remove-Item -LiteralPath $resolvedTreeTestRoot -Recurse -Force
    }
}
foreach ($functionName in @(
        'Assert-SafeMediaRelativePath',
        'Get-CanonicalUsbLayoutDigest',
        'Get-BoundedMediaTreeManifest',
        'Assert-MediaTreeManifestEqual',
        'Get-PrivateDataTreeSummary')) {
    Remove-Item -LiteralPath "Function:\$functionName"
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
    -OutputRoot (Join-Path $repoRoot 'out\forbidden-winpe') `
    -ExpectedMessage 'リポジトリ外'
Assert-Rejected `
    -OutputRoot ([IO.Path]::GetPathRoot($repoRoot)) `
    -ExpectedMessage 'ドライブ直下'
Assert-Rejected `
    -OutputRoot $env:TEMP `
    -ExpectedMessage '既存の出力先'

$diagnosticPath = Join-Path $repoRoot `
    'out\build\msvc-x64\src\MediaBuilder\ytec-winpe-environment.exe'
$diagnosticText = (& $diagnosticPath --json | Out-String)
$diagnosticExit = $LASTEXITCODE
try {
    $diagnosticReport = $diagnosticText | ConvertFrom-Json
} catch {
    throw "WinPE環境診断JSONを解析できません: $($_.Exception.Message)"
}
$diagnosticCandidates = @($diagnosticReport.candidates)
$selectedCandidateIndex = $diagnosticReport.selectedCandidateIndex
$selectedCandidate = $null
if (($selectedCandidateIndex -is [int] -or
        $selectedCandidateIndex -is [long]) -and
    $selectedCandidateIndex -ge 0 -and
    $selectedCandidateIndex -lt $diagnosticCandidates.Count) {
    $selectedCandidate = $diagnosticCandidates[$selectedCandidateIndex]
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
$selectedCandidateReady = $null -ne $selectedCandidate
if ($selectedCandidateReady) {
    foreach ($field in $candidateBooleanFields) {
        $value = $selectedCandidate.$field
        if ($value -isnot [bool] -or $value -ne $true) {
            $selectedCandidateReady = $false
        }
    }
}
$adkReady = $diagnosticExit -eq 0 -and
    ($diagnosticReport.schemaVersion -is [int] -or
        $diagnosticReport.schemaVersion -is [long]) -and
    $diagnosticReport.schemaVersion -eq 1 -and
    $diagnosticReport.architecture -is [string] -and
    $diagnosticReport.architecture -ceq 'amd64' -and
    $diagnosticReport.baseLayoutReady -is [bool] -and
    $diagnosticReport.baseLayoutReady -eq $true -and
    $diagnosticReport.bootexLayoutReady -is [bool] -and
    $diagnosticReport.bootexLayoutReady -eq $true -and
    $diagnosticReport.mediaCreationPermitted -is [bool] -and
    $diagnosticReport.mediaCreationPermitted -eq $true -and
    $selectedCandidateReady
$adkMissingOnly = $diagnosticExit -eq 2 -and
    ($diagnosticReport.schemaVersion -is [int] -or
        $diagnosticReport.schemaVersion -is [long]) -and
    $diagnosticReport.schemaVersion -eq 1 -and
    $diagnosticReport.architecture -is [string] -and
    $diagnosticReport.architecture -ceq 'amd64' -and
    $diagnosticReport.baseLayoutReady -is [bool] -and
    $diagnosticReport.baseLayoutReady -eq $false -and
    $diagnosticReport.bootexLayoutReady -is [bool] -and
    $diagnosticReport.bootexLayoutReady -eq $false -and
    $diagnosticReport.mediaCreationPermitted -is [bool] -and
    $diagnosticReport.mediaCreationPermitted -eq $false -and
    $null -eq $diagnosticReport.selectedCandidateIndex
if ($adkMissingOnly -and $diagnosticCandidates.Count -eq 0) {
    $adkMissingOnly = $false
}
if ($adkMissingOnly) {
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
    foreach ($candidate in $diagnosticCandidates) {
        foreach ($field in $candidateBooleanFields) {
            $value = $candidate.$field
            if ($value -isnot [bool]) {
                $adkMissingOnly = $false
            }
        }
        foreach ($field in $requiredFalseFields) {
            if ($candidate.$field -ne $false) {
                $adkMissingOnly = $false
            }
        }
        $candidateDiagnostics = @($candidate.diagnostics)
        if ($candidateDiagnostics.Count -eq 0 -or
            $candidateDiagnostics.Count -gt 5) {
            $adkMissingOnly = $false
        }
        $seenDiagnosticCodes = @{}
        foreach ($candidateDiagnostic in $candidateDiagnostics) {
            if ($candidateDiagnostic.code -is [string]) {
                if ($seenDiagnosticCodes.ContainsKey(
                        [string]$candidateDiagnostic.code)) {
                    $adkMissingOnly = $false
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
                $adkMissingOnly = $false
            }
        }
        if ($seenDiagnosticCodes.ContainsKey('ADK_ROOT_NOT_FOUND')) {
            if ($candidateDiagnostics.Count -ne 1) {
                $adkMissingOnly = $false
            }
            foreach ($field in $candidateBooleanFields) {
                if ($candidate.$field -ne $false) {
                    $adkMissingOnly = $false
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
                    $adkMissingOnly = $false
                }
            } else {
                $deploymentReady = -not ($dismMissing -or $oscdimgMissing)
                if ($candidate.deploymentToolsPresent -ne $deploymentReady -or
                    $candidate.microsoftToolsTrusted -ne $deploymentReady) {
                    $adkMissingOnly = $false
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
                if ($copypeMissing -or $makeMediaMissing -or $baseWimMissing -or
                    $candidate.winpeAddonPresent -ne $false -or
                    $candidate.bootexSupported -ne $false) {
                    $adkMissingOnly = $false
                }
            } else {
                $winpeAddonReady = -not (
                    $copypeMissing -or $makeMediaMissing -or $baseWimMissing)
                $bootexReady = -not $makeMediaMissing
                if ($candidate.winpeAddonPresent -ne $winpeAddonReady -or
                    $candidate.bootexSupported -ne $bootexReady) {
                    $adkMissingOnly = $false
                }
            }
        }
    }
}
if (-not $adkReady -and -not $adkMissingOnly) {
    throw "WinPE環境診断が媒体境界テストの実行条件を満たしません（終了コード $diagnosticExit）。"
}

$usbPreflightOutput = Join-Path $env:LOCALAPPDATA `
    ('YTEC\ytec-disk-clone\usb-preflight-only\' +
        [guid]::NewGuid().ToString('N'))

if ($adkReady) {
$preflightOutput = Join-Path $env:LOCALAPPDATA `
    ('YTEC\ytec-disk-clone\preflight-only\' + [guid]::NewGuid().ToString('N'))
$result = & $scriptPath -OutputRoot $preflightOutput
if ($result -notlike 'WINPE_APP_MEDIA_PREFLIGHT_PASS=*') {
    throw "事前検証の成功マーカーがありません: $result"
}
$preflight = $result.Substring(
    'WINPE_APP_MEDIA_PREFLIGHT_PASS='.Length) | ConvertFrom-Json
if ($preflight.winpeGui.machine -ne 'AMD64' -or
    $preflight.winpeGui.optionalHeader -ne 'PE32+' -or
    $preflight.winpeGui.sha256 -notmatch '^[0-9A-F]{64}$') {
    throw 'WinPE GUIのAMD64形式またはSHA-256事前検証が不足しています。'
}
foreach ($required in @(
        'COMCTL32.dll', 'COMDLG32.dll', 'GDI32.dll', 'POWRPROF.dll',
        'USER32.dll')) {
    if ($preflight.winpeGui.dependentDlls -notcontains $required) {
        throw "WinPE GUIの必須DLLが固定検査に含まれていません: $required"
    }
}
if (@($preflight.winpeGui.dynamicallyLoadedSystemDlls).Count -ne 0) {
    throw 'WinPE GUIに未監査の動的System32 DLLが記録されています。'
}
if ($preflight.japaneseFontSupport.repositoryCopy -ne $false -or
    $preflight.japaneseFontSupport.path -notlike
        '*\WinPE-FontSupport-JA-JP.cab' -or
    $preflight.japaneseFontSupport.sha256 -notmatch '^[0-9A-F]{64}$') {
    throw 'ローカルADK日本語フォントの非同梱境界またはSHA-256記録が不正です。'
}
if ($preflight.lineSeedLicense.name -ne 'LINE Seed JP' -or
    $preflight.lineSeedLicense.version -ne 'LINESeedJP_20241105' -or
    $preflight.lineSeedLicense.license -ne 'OFL-1.1' -or
    $preflight.lineSeedLicense.sha256 -notmatch '^[0-9A-F]{64}$') {
    throw 'LINE Seed JPの版・OFL・SHA-256記録が不正です。'
}
$expectedThirdPartyPayload = [ordered]@{
    projectLicense = 'LICENSE'
    projectNotice = 'NOTICE'
    notices = 'THIRD-PARTY-NOTICES.txt'
    sbom = 'SBOM.spdx.json'
    licenseReadme = 'licenses\README.md'
    lineSeedLicense = 'licenses\LINE-Seed-JP-OFL-1.1.txt'
    zstandardLicense = 'licenses\Zstandard-BSD-3-Clause.txt'
    argon2License = 'licenses\Argon2-Apache-2.0.txt'
}
$reportedThirdPartyPayload = @(
    $preflight.thirdPartyPayload.PSObject.Properties)
if ($reportedThirdPartyPayload.Count -ne $expectedThirdPartyPayload.Count) {
    throw 'WinPE自己作成payloadの第三者ライセンス資料数が不正です。'
}
foreach ($name in $expectedThirdPartyPayload.Keys) {
    $sourcePath = Join-Path $repoRoot $expectedThirdPartyPayload[$name]
    $report = $preflight.thirdPartyPayload.$name
    if ($null -eq $report -or
        $report.path -cne [IO.Path]::GetFullPath($sourcePath) -or
        $report.length -ne (Get-Item -LiteralPath $sourcePath).Length -or
        $report.sha256 -cne
            (Get-FileHash -LiteralPath $sourcePath -Algorithm SHA256).Hash) {
        throw "WinPE自己作成payloadの正本SHA-256記録が不正です: $name"
    }
}
if (Test-Path -LiteralPath $preflightOutput) {
    throw '事前検証だけで出力先が作成されました。'
}

$explicitOutput = Join-Path $env:LOCALAPPDATA `
    ('YTEC\ytec-disk-clone\product-preflight\' +
        [guid]::NewGuid().ToString('N'))
$explicitIso = Join-Path $env:TEMP `
    ('Y-TEC-Tsumugi-Drive-' + [guid]::NewGuid().ToString('N') + '.iso')
$explicitResult = & $scriptPath `
    -OutputRoot $explicitOutput `
    -FinalIsoPath $explicitIso `
    -DiagnosticPath (Join-Path $repoRoot `
        'out\build\msvc-x64\src\MediaBuilder\ytec-winpe-environment.exe') `
    -WinPEAppPath (Join-Path $repoRoot `
        'out\build\msvc-x64-vm\src\WinPEApp\ytec-winpe-app.exe') `
    -WinPEGuiPath (Join-Path $repoRoot `
        'out\build\msvc-x64-vm\src\WinPEApp\ytec-winpe-gui.exe')
if ($explicitResult -notlike 'WINPE_APP_MEDIA_PREFLIGHT_PASS=*') {
    throw "製品配置事前検証の成功マーカーがありません: $explicitResult"
}
$explicitPreflight = $explicitResult.Substring(
    'WINPE_APP_MEDIA_PREFLIGHT_PASS='.Length) | ConvertFrom-Json
if ($explicitPreflight.outputRoot -ne
        [IO.Path]::GetFullPath($explicitOutput) -or
    $explicitPreflight.finalIsoPath -ne
        [IO.Path]::GetFullPath($explicitIso) -or
    $explicitPreflight.finalManifestPath -ne
        ([IO.Path]::GetFullPath($explicitIso) + '.manifest.json')) {
    throw '製品配置の完成ISO／manifest／一時作業先が分離されていません。'
}
foreach ($path in @(
        $explicitOutput,
        $explicitIso,
        ($explicitIso + '.manifest.json'))) {
    if (Test-Path -LiteralPath $path) {
        throw "製品配置の事前検証だけで出力が作成されました: $path"
    }
}

$usbPreflightResult = & $scriptPath `
    -OutputRoot $usbPreflightOutput `
    -TargetUsbDrive 'Z:' `
    -ExpectedUsbDiskNumber 2147483000 `
    -ExpectedUsbSizeBytes 34359738368 `
    -ExpectedUsbSerialSuffix 'FAKE1234' `
    -ExpectedUsbDeviceInstanceId 'USB\VID_FAKE&PID_TEST\BOUNDARY' `
    -ExpectedUsbCanonicalLayoutSha256 `
        'FAEA8977FDFD7359207E3F4935AFE1542F59329B93F1089737517D7F0B00DBDD' `
    -UsbOperation Initialize `
    -UsbDataFileSystem NTFS `
    -BuildUsb
if ($usbPreflightResult -notlike
        'WINPE_APP_MEDIA_PREFLIGHT_PASS=*') {
    throw "USB事前検証の成功マーカーがありません: $usbPreflightResult"
}
$usbPreflight = $usbPreflightResult.Substring(
    'WINPE_APP_MEDIA_PREFLIGHT_PASS='.Length) | ConvertFrom-Json
if (-not $usbPreflight.buildUsbRequested -or
    $usbPreflight.targetUsbDrive -ne 'Z:' -or
    $usbPreflight.expectedUsbDiskNumber -ne 2147483000 -or
    $usbPreflight.expectedUsbCanonicalLayoutSha256 -ne
        'FAEA8977FDFD7359207E3F4935AFE1542F59329B93F1089737517D7F0B00DBDD' -or
    $usbPreflight.usbOperation -ne 'Initialize' -or
    $usbPreflight.usbDataFileSystem -ne 'NTFS') {
    throw 'USB事前検証が対象限定情報を正しく記録していません。'
}
} else {
    $missingAdkOutput = Join-Path $env:LOCALAPPDATA `
        ('YTEC\ytec-disk-clone\missing-adk-preflight\' +
            [guid]::NewGuid().ToString('N'))
    try {
        & $scriptPath `
            -OutputRoot $missingAdkOutput `
            -DiagnosticPath $diagnosticPath | Out-Null
        throw 'ADK未導入環境で媒体作成preflightが許可されました。'
    } catch {
        if ($_.Exception.Message -cne
            'WinPE環境診断が終了コード 2 で作成を拒否しました。') {
            throw "ADK未導入時の想定外エラーです: $($_.Exception.Message)"
        }
    }
    if (Test-Path -LiteralPath $missingAdkOutput) {
        throw 'ADK未導入の拒否前に出力先が作成されました。'
    }
    Write-Output 'WinPEApp missing-ADK fail-closed boundary: PASS'
}

$tooSmallOutput = Join-Path $env:LOCALAPPDATA `
    ('YTEC\ytec-disk-clone\usb-too-small\' +
        [guid]::NewGuid().ToString('N'))
try {
    & $scriptPath `
        -OutputRoot $tooSmallOutput `
        -TargetUsbDrive 'Z:' `
        -ExpectedUsbDiskNumber 2147483000 `
        -ExpectedUsbSizeBytes ([UInt64](8GB) - 1) `
        -ExpectedUsbDeviceInstanceId 'USB\FAKE' `
        -ExpectedUsbCanonicalLayoutSha256 `
            'FAEA8977FDFD7359207E3F4935AFE1542F59329B93F1089737517D7F0B00DBDD' `
        -UsbOperation Initialize `
        -UsbDataFileSystem NTFS `
        -BuildUsb | Out-Null
    throw '8GiB未満のレスキューUSBが事前検証を通過しました。'
} catch {
    if ($_.Exception.Message -notlike '*8GiB以上*') {
        throw "8GiB境界の想定外エラーです: $($_.Exception.Message)"
    }
}
if (Test-Path -LiteralPath $tooSmallOutput) {
    throw '8GiB境界の拒否前に出力先が作成されました。'
}

try {
    & $scriptPath `
        -OutputRoot $usbPreflightOutput `
        -TargetUsbDrive 'Z:' `
        -ExpectedUsbDiskNumber 7 `
        -ExpectedUsbSizeBytes 34359738368 `
        -ExpectedUsbDeviceInstanceId 'USB\FAKE' | Out-Null
    throw 'BuildUsbなしのUSB対象情報が許可されました。'
} catch {
    if ($_.Exception.Message -notlike '*-BuildUsb*') {
        throw "BuildUsb境界の想定外エラーです: $($_.Exception.Message)"
    }
}
if (Test-Path -LiteralPath $usbPreflightOutput) {
    throw 'BuildUsb境界の拒否前に出力先が作成されました。'
}

Write-Output 'WinPEApp media boundary tests: PASS'
