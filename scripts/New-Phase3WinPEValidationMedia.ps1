param(
    [Parameter(Mandatory)]
    [string]$BaseProductMediaRoot,

    [Parameter(Mandatory)]
    [string]$OutputRoot,

    [switch]$BuildMedia
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$OutputEncoding = [Text.UTF8Encoding]::new($false)

function Assert-ExternalNewOutputPath {
    param(
        [Parameter(Mandatory)][string]$RepositoryRoot,
        [Parameter(Mandatory)][string]$CandidatePath
    )

    $repositoryFullPath = [IO.Path]::GetFullPath($RepositoryRoot)
    $outputFullPath = [IO.Path]::GetFullPath($CandidatePath)
    $repositoryPrefix = $repositoryFullPath.TrimEnd('\') + '\'
    if ($outputFullPath.Equals(
            $repositoryFullPath,
            [StringComparison]::OrdinalIgnoreCase) -or
        $outputFullPath.StartsWith(
            $repositoryPrefix,
            [StringComparison]::OrdinalIgnoreCase)) {
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

    $ancestor = Split-Path -Parent $outputFullPath
    while (-not [string]::IsNullOrWhiteSpace($ancestor) -and
        -not (Test-Path -LiteralPath $ancestor)) {
        $next = Split-Path -Parent $ancestor
        if ($next -eq $ancestor) {
            break
        }
        $ancestor = $next
    }
    while (-not [string]::IsNullOrWhiteSpace($ancestor)) {
        $item = Get-Item -LiteralPath $ancestor -Force
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "出力先の既存祖先にreparse pointがあります: $ancestor"
        }
        $next = Split-Path -Parent $ancestor
        if ([string]::IsNullOrWhiteSpace($next) -or $next -eq $ancestor) {
            break
        }
        $ancestor = $next
    }
    return $outputFullPath
}

function Assert-RegularNonReparseFile {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$Description
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Description が見つかりません: $Path"
    }
    $item = Get-Item -LiteralPath $Path -Force
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "$Description のreparse pointは使用しません: $Path"
    }
}

function Assert-NoReparsePointInTree {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$Description
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        throw "$Description が見つかりません: $Path"
    }
    $root = Get-Item -LiteralPath $Path -Force
    if (($root.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "$Description のreparse pointは使用しません: $Path"
    }
    $reparse = Get-ChildItem -LiteralPath $Path -Recurse -Force |
        Where-Object {
            ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0
        } |
        Select-Object -First 1
    if ($null -ne $reparse) {
        throw "$Description 内のreparse pointは使用しません: $($reparse.FullName)"
    }
}

function Assert-MicrosoftSignature {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$Description
    )

    Assert-RegularNonReparseFile -Path $Path -Description $Description
    $signature = Get-AuthenticodeSignature -FilePath $Path
    if ($signature.Status -ne
            [Management.Automation.SignatureStatus]::Valid -or
        $null -eq $signature.SignerCertificate -or
        $signature.SignerCertificate.Subject -notmatch
            '(^|,\s*)O=Microsoft Corporation(,|$)') {
        throw "$Description の有効なMicrosoft署名を確認できません: $Path"
    }
}

function Get-Amd64PeReport {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$Description,
        [Parameter(Mandatory)][string[]]$AllowedDependencies
    )

    Assert-RegularNonReparseFile -Path $Path -Description $Description
    $item = Get-Item -LiteralPath $Path
    if ($item.Length -lt 512 -or $item.Length -gt 64MB) {
        throw "$Description のファイルサイズが許可範囲外です: $($item.Length)"
    }

    $bytes = [IO.File]::ReadAllBytes($item.FullName)
    if ($bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) {
        throw "$Description にDOS MZ署名がありません。"
    }
    $peOffset = [BitConverter]::ToInt32($bytes, 0x3C)
    if ($peOffset -lt 0x40 -or $peOffset -gt ($bytes.Length - 26) -or
        $bytes[$peOffset] -ne 0x50 -or
        $bytes[$peOffset + 1] -ne 0x45 -or
        $bytes[$peOffset + 2] -ne 0 -or
        $bytes[$peOffset + 3] -ne 0) {
        throw "$Description のPEヘッダーが不正です。"
    }
    $machine = [BitConverter]::ToUInt16($bytes, $peOffset + 4)
    $optionalHeaderMagic = [BitConverter]::ToUInt16($bytes, $peOffset + 24)
    if ($machine -ne 0x8664 -or $optionalHeaderMagic -ne 0x020B) {
        throw "$Description はAMD64 PE32+でなければなりません。"
    }

    $ascii = [Text.Encoding]::ASCII.GetString($bytes)
    $dependencies = @(
        [regex]::Matches($ascii, '(?i)[A-Za-z0-9._-]+\.dll') |
            ForEach-Object Value |
            Sort-Object -Unique
    )
    $unexpected = @(
        $dependencies | Where-Object { $AllowedDependencies -notcontains $_ }
    )
    $missing = @(
        $AllowedDependencies | Where-Object { $dependencies -notcontains $_ }
    )
    if ($unexpected.Count -gt 0 -or $missing.Count -gt 0) {
        throw ("$Description のDLL依存が固定許可リストと一致しません。" +
            " unexpected=[$($unexpected -join ',')]" +
            " missing=[$($missing -join ',')]")
    }

    return [ordered]@{
        path = $item.FullName
        length = $item.Length
        sha256 = (Get-FileHash -LiteralPath $item.FullName `
            -Algorithm SHA256).Hash
        machine = 'AMD64'
        optionalHeader = 'PE32+'
        dependentDlls = $dependencies
    }
}

function Invoke-CheckedNative {
    param(
        [Parameter(Mandatory)][string]$Command,
        [Parameter(Mandatory)][string[]]$Arguments,
        [Parameter(Mandatory)][string]$Operation
    )

    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Operation が終了コード $LASTEXITCODE で失敗しました。"
    }
}

function Test-IsAdministrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    return $principal.IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}

$repoRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$outputFullPath = Assert-ExternalNewOutputPath `
    -RepositoryRoot $repoRoot `
    -CandidatePath $OutputRoot
$baseRoot = [IO.Path]::GetFullPath($BaseProductMediaRoot)
$repositoryPrefix = $repoRoot.TrimEnd('\') + '\'
if ($baseRoot.Equals($repoRoot, [StringComparison]::OrdinalIgnoreCase) -or
    $baseRoot.StartsWith(
        $repositoryPrefix,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw '基礎WinPEメディアはリポジトリ外にある検証済み生成物だけを使用できます。'
}
Assert-NoReparsePointInTree `
    -Path $baseRoot `
    -Description '基礎WinPEメディア'

$baseManifestPath = Join-Path $baseRoot 'winpe-app-media-manifest.json'
$baseBootWim = Join-Path $baseRoot 'working\media\sources\boot.wim'
Assert-RegularNonReparseFile `
    -Path $baseManifestPath `
    -Description '基礎メディアmanifest'
Assert-RegularNonReparseFile `
    -Path $baseBootWim `
    -Description '基礎メディアboot.wim'
$baseManifest = Get-Content -LiteralPath $baseManifestPath -Raw |
    ConvertFrom-Json
if ($baseManifest.schemaVersion -ne 1 -or
    $baseManifest.purpose -notin @(
        'Y-TEC Disk Clone WinPEApp validation media',
        'Y-TEC Tsumugi Drive WinPEApp validation media'
    ) -or
    $baseManifest.repositoryContainsMicrosoftPayload -ne $false -or
    $baseManifest.certificateGeneration -notin @('2011CA', '2023CA')) {
    throw '基礎メディアmanifestの安全境界が一致しません。'
}
$baseWimHash = (Get-FileHash -LiteralPath $baseBootWim `
    -Algorithm SHA256).Hash
if ($baseWimHash -ne [string]$baseManifest.stagedWimAfter.sha256 -or
    (Get-Item -LiteralPath $baseBootWim).Length -ne
        [long]$baseManifest.stagedWimAfter.length) {
    throw '基礎メディアboot.wimがmanifestのSHA-256/容量と一致しません。'
}

$diagnostic = Join-Path $repoRoot `
    'out\build\msvc-x64\src\MediaBuilder\ytec-winpe-environment.exe'
Assert-RegularNonReparseFile `
    -Path $diagnostic `
    -Description 'WinPE環境診断CLI'
$diagnosticText = (& $diagnostic --json | Out-String)
if ($LASTEXITCODE -ne 0) {
    throw 'WinPE環境診断がPhase 3メディア作成を拒否しました。'
}
$diagnosticReport = $diagnosticText | ConvertFrom-Json
if (-not $diagnosticReport.mediaCreationPermitted -or
    $null -eq $diagnosticReport.selectedCandidateIndex) {
    throw 'WinPE環境診断の作成許可ゲートを通過していません。'
}
$candidate = $diagnosticReport.candidates[
    [int]$diagnosticReport.selectedCandidateIndex]
$adkRoot = [IO.Path]::GetFullPath([string]$candidate.root)
$oscdimgRoot = Join-Path $adkRoot `
    'Deployment Tools\amd64\Oscdimg'
$dism = Join-Path $adkRoot `
    'Deployment Tools\amd64\DISM\dism.exe'
$oscdimg = Join-Path $oscdimgRoot 'oscdimg.exe'
$etfsboot = Join-Path $oscdimgRoot 'etfsboot.com'
$efiBootImage = if ($baseManifest.certificateGeneration -eq '2023CA') {
    Join-Path $oscdimgRoot 'efisys_EX.bin'
} else {
    Join-Path $oscdimgRoot 'efisys.bin'
}
Assert-MicrosoftSignature -Path $dism -Description 'DISM'
Assert-MicrosoftSignature -Path $oscdimg -Description 'Oscdimg'
Assert-RegularNonReparseFile -Path $etfsboot -Description 'BIOSブートイメージ'
Assert-RegularNonReparseFile -Path $efiBootImage -Description 'UEFIブートイメージ'

$productApp = Join-Path $repoRoot `
    'out\build\msvc-x64-vm\src\WinPEApp\ytec-winpe-app.exe'
$cloneHarness = Join-Path $repoRoot `
    'out\build\msvc-x64-vm-destructive\tests\ytec-phase1-physical-clone-vm.exe'
$bcdHarness = Join-Path $repoRoot `
    'out\build\msvc-x64-vm-destructive\tests\ytec-phase3-bcdboot-vm.exe'
$productReport = Get-Amd64PeReport `
    -Path $productApp `
    -Description '読み取り専用WinPEApp' `
    -AllowedDependencies @(
        'ADVAPI32.dll', 'CRYPT32.dll', 'KERNEL32.dll', 'SETUPAPI.dll',
        'WINTRUST.dll')
$cloneReport = Get-Amd64PeReport `
    -Path $cloneHarness `
    -Description 'Phase 3 MBRクローンVM専用ハーネス' `
    -AllowedDependencies @(
        'ADVAPI32.dll', 'bcrypt.dll', 'KERNEL32.dll', 'ole32.dll',
        'SETUPAPI.dll')
$bcdReport = Get-Amd64PeReport `
    -Path $bcdHarness `
    -Description 'Phase 3 BCDBoot VM専用ハーネス' `
    -AllowedDependencies @(
        'ADVAPI32.dll', 'bcrypt.dll', 'CRYPT32.dll', 'KERNEL32.dll',
        'ole32.dll', 'SETUPAPI.dll', 'WINTRUST.dll')
if ($productReport.sha256 -ne [string]$baseManifest.winpeApp.sha256) {
    throw '基礎メディアのWinPEAppと現在の静的ランタイム製品が一致しません。'
}

$preflight = [ordered]@{
    schemaVersion = 1
    purpose = 'Phase 3 Legacy BIOS VM-only clone and BCDBoot validation media'
    baseProductMediaRoot = $baseRoot
    baseWim = [ordered]@{
        length = (Get-Item -LiteralPath $baseBootWim).Length
        sha256 = $baseWimHash
    }
    outputRoot = $outputFullPath
    outputExists = $false
    certificateGeneration = [string]$baseManifest.certificateGeneration
    adkVersion = [string]$candidate.deploymentToolsVersion
    dismVersion = [string]$candidate.dismFileVersion
    productApp = $productReport
    cloneHarness = $cloneReport
    bcdBootHarness = $bcdReport
    buildRequested = [bool]$BuildMedia
    administrator = Test-IsAdministrator
}
if (-not $BuildMedia) {
    Write-Output ('PHASE3_WINPE_MEDIA_PREFLIGHT_PASS=' +
        ($preflight | ConvertTo-Json -Depth 7 -Compress))
    return
}
if (-not $preflight.administrator) {
    throw '実WIMのマウント/コミットには管理者権限が必要です。UACを承認したpwshで-BuildMediaを実行してください。'
}

$baseMediaRoot = Join-Path $baseRoot 'working\media'
Assert-NoReparsePointInTree `
    -Path $baseMediaRoot `
    -Description '基礎WinPE mediaツリー'
$workingRoot = Join-Path $outputFullPath 'working'
$mediaRoot = Join-Path $workingRoot 'media'
$mountRoot = Join-Path $workingRoot 'mount'
$bootWim = Join-Path $mediaRoot 'sources\boot.wim'
$isoPath = Join-Path $outputFullPath (
    'YDC-Phase3-LegacyBIOS-VMOnly-amd64-{0}.iso' -f
        $baseManifest.certificateGeneration)
$manifestPath = Join-Path $outputFullPath 'phase3-winpe-media-manifest.json'

New-Item -ItemType Directory -Path $outputFullPath | Out-Null
New-Item -ItemType Directory -Path $workingRoot | Out-Null
Copy-Item -LiteralPath $baseMediaRoot -Destination $mediaRoot -Recurse
New-Item -ItemType Directory -Path $mountRoot | Out-Null
(Get-Item -LiteralPath $bootWim).IsReadOnly = $false
if ((Get-FileHash -LiteralPath $bootWim -Algorithm SHA256).Hash -ne
    $baseWimHash) {
    throw '複製した基礎boot.wimのSHA-256が変化しました。'
}
$stagedWimBefore = [ordered]@{
    length = (Get-Item -LiteralPath $bootWim).Length
    sha256 = (Get-FileHash -LiteralPath $bootWim -Algorithm SHA256).Hash
}

$mounted = $false
try {
    Invoke-CheckedNative `
        -Command $dism `
        -Arguments @(
            '/Mount-Image',
            "/ImageFile:$bootWim",
            '/Index:1',
            "/MountDir:$mountRoot"
        ) `
        -Operation 'Phase 3 WinPE boot.wimのマウント'
    $mounted = $true

    $payloadRoot = Join-Path $mountRoot 'YtecDiskClone'
    $mountedApp = Join-Path $payloadRoot 'ytec-winpe-app.exe'
    $mountedLaunch = Join-Path $payloadRoot 'launch.cmd'
    $mountedWinpeShell = Join-Path $mountRoot `
        'Windows\System32\winpeshl.ini'
    foreach ($required in @($mountedApp, $mountedLaunch, $mountedWinpeShell)) {
        # DISMのWIMマウントは通常ファイルにもWIMオーバーレイ属性を
        # 付けるため、ホスト側入力用のreparse拒否規則は適用しない。
        # 信頼境界は固定mountRoot、検証済みWIMハッシュ、個別SHA-256で
        # 維持する。
        if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
            throw "基礎WIM内の予約済み製品ファイルがありません: $required"
        }
    }
    if ((Get-FileHash -LiteralPath $mountedApp -Algorithm SHA256).Hash -ne
        $productReport.sha256) {
        throw '基礎WIM内のWinPEAppが現在の製品ハッシュと一致しません。'
    }

    $mountedClone = Join-Path $payloadRoot `
        'ytec-phase1-physical-clone-vm.exe'
    $mountedBcd = Join-Path $payloadRoot `
        'ytec-phase3-bcdboot-vm.exe'
    $diskpartScript = Join-Path $payloadRoot 'mount-phase3.txt'
    foreach ($reserved in @($mountedClone, $mountedBcd, $diskpartScript)) {
        if (Test-Path -LiteralPath $reserved) {
            throw "基礎WIM内のPhase 3予約先は上書きしません: $reserved"
        }
    }
    Copy-Item -LiteralPath $cloneHarness -Destination $mountedClone
    Copy-Item -LiteralPath $bcdHarness -Destination $mountedBcd
    @(
        'select disk 1',
        'online disk noerr',
        'rescan',
        'select disk 1',
        'select partition 1',
        'remove all noerr',
        'assign letter=W',
        'exit'
    ) | Set-Content -LiteralPath $diskpartScript -Encoding ascii
    @(
        '@echo off',
        'setlocal EnableExtensions EnableDelayedExpansion',
        'chcp 65001 >nul',
        'set "ROOT=%SYSTEMDRIVE%\YtecDiskClone"',
        'set "LOG=%SYSTEMDRIVE%\YDC-Phase3-validation.log"',
        'mode com1: BAUD=115200 PARITY=n DATA=8 STOP=1 >nul 2>&1',
        'echo YDC_PHASE3_AUTOMATION_BEGIN>COM1',
        'echo YDC_PHASE3_AUTOMATION_BEGIN>"%LOG%"',
        'echo profile=legacy-bios sourceDisk=0 targetDisk=1>>"%LOG%"',
        '"%ROOT%\ytec-phase1-physical-clone-vm.exe" --plan --source 0 --target 1 --legacy-bios-test >"%ROOT%\clone-plan.txt" 2>&1',
        'set "STEP_EXIT=!ERRORLEVEL!"',
        'type "%ROOT%\clone-plan.txt">>"%LOG%"',
        'if not "!STEP_EXIT!"=="0" goto clone_plan_failed',
        'for /f "usebackq tokens=1,* delims==" %%A in ("%ROOT%\clone-plan.txt") do if /i "%%A"=="confirmation" set "CLONE_CONFIRM=%%B"',
        'if not defined CLONE_CONFIRM goto clone_confirmation_failed',
        '"%ROOT%\ytec-phase1-physical-clone-vm.exe" --execute --source 0 --target 1 --legacy-bios-test --authorization YTEC-VM-ONLY-PHASE3-MBR-CLONE --confirmation "!CLONE_CONFIRM!" >>"%LOG%" 2>&1',
        'if errorlevel 1 goto clone_failed',
        'diskpart.exe /s "%ROOT%\mount-phase3.txt" >>"%LOG%" 2>&1',
        'if errorlevel 1 goto mount_failed',
        'if not exist W:\Windows\System32\winload.exe goto mount_failed',
        '"%ROOT%\ytec-phase3-bcdboot-vm.exe" --plan --target 1 --windows-root W:\ --system-root W:\ >"%ROOT%\bcdboot-plan.txt" 2>&1',
        'set "STEP_EXIT=!ERRORLEVEL!"',
        'type "%ROOT%\bcdboot-plan.txt">>"%LOG%"',
        'if not "!STEP_EXIT!"=="0" goto bcdboot_plan_failed',
        'for /f "usebackq tokens=1,* delims==" %%A in ("%ROOT%\bcdboot-plan.txt") do if /i "%%A"=="confirmation" set "BCD_CONFIRM=%%B"',
        'if not defined BCD_CONFIRM goto bcdboot_confirmation_failed',
        '"%ROOT%\ytec-phase3-bcdboot-vm.exe" --execute --target 1 --windows-root W:\ --system-root W:\ --authorization YTEC-VM-ONLY-PHASE3-BIOS-BCDBOOT --confirmation "!BCD_CONFIRM!" >>"%LOG%" 2>&1',
        'if errorlevel 1 goto bcdboot_failed',
        'echo YDC_PHASE3_AUTOMATION_PASS>>"%LOG%"',
        'copy /y "%LOG%" W:\YDC-Phase3-validation.log >nul',
        'type "%LOG%">COM1',
        'type "%LOG%"',
        'goto end',
        ':clone_plan_failed',
        'echo YDC_PHASE3_AUTOMATION_FAIL stage=clone-plan exit=!STEP_EXIT!>>"%LOG%"',
        'goto failed',
        ':clone_confirmation_failed',
        'echo YDC_PHASE3_AUTOMATION_FAIL stage=clone-confirmation>>"%LOG%"',
        'goto failed',
        ':clone_failed',
        'echo YDC_PHASE3_AUTOMATION_FAIL stage=clone-execute exit=!ERRORLEVEL!>>"%LOG%"',
        'goto failed',
        ':mount_failed',
        'echo YDC_PHASE3_AUTOMATION_FAIL stage=mount-target exit=!ERRORLEVEL!>>"%LOG%"',
        'goto failed',
        ':bcdboot_plan_failed',
        'echo YDC_PHASE3_AUTOMATION_FAIL stage=bcdboot-plan exit=!STEP_EXIT!>>"%LOG%"',
        'goto failed',
        ':bcdboot_confirmation_failed',
        'echo YDC_PHASE3_AUTOMATION_FAIL stage=bcdboot-confirmation>>"%LOG%"',
        'goto failed',
        ':bcdboot_failed',
        'echo YDC_PHASE3_AUTOMATION_FAIL stage=bcdboot-execute exit=!ERRORLEVEL!>>"%LOG%"',
        ':failed',
        'type "%LOG%">COM1',
        'type "%LOG%"',
        ':end',
        'echo.',
        'echo Phase 3 VM-only validation finished. Review the marker above.'
    ) | Set-Content -LiteralPath $mountedLaunch -Encoding ascii
    @(
        '[LaunchApps]',
        '%SYSTEMROOT%\System32\wpeinit.exe',
        '%SYSTEMROOT%\System32\cmd.exe, /k %SYSTEMDRIVE%\YtecDiskClone\launch.cmd'
    ) | Set-Content -LiteralPath $mountedWinpeShell -Encoding ascii

    $addedFiles = @(
        foreach ($file in @(
                $mountedClone,
                $mountedBcd,
                $diskpartScript,
                $mountedLaunch,
                $mountedWinpeShell)) {
            [ordered]@{
                relativePath = $file.Substring($mountRoot.Length).TrimStart('\')
                length = (Get-Item -LiteralPath $file).Length
                sha256 = (Get-FileHash -LiteralPath $file `
                    -Algorithm SHA256).Hash
            }
        }
    )
    if ((Get-FileHash -LiteralPath $mountedClone -Algorithm SHA256).Hash -ne
            $cloneReport.sha256 -or
        (Get-FileHash -LiteralPath $mountedBcd -Algorithm SHA256).Hash -ne
            $bcdReport.sha256) {
        throw 'WIMへ追加したVM専用ハーネスのSHA-256が元ファイルと一致しません。'
    }

    Invoke-CheckedNative `
        -Command $dism `
        -Arguments @(
            '/Unmount-Image',
            "/MountDir:$mountRoot",
            '/Commit',
            '/CheckIntegrity'
        ) `
        -Operation 'Phase 3 WinPE boot.wimのコミット'
    $mounted = $false
} catch {
    if ($mounted) {
        & $dism '/Unmount-Image' "/MountDir:$mountRoot" '/Discard'
    }
    throw
}

$stagedWimAfter = [ordered]@{
    length = (Get-Item -LiteralPath $bootWim).Length
    sha256 = (Get-FileHash -LiteralPath $bootWim -Algorithm SHA256).Hash
}
if ($stagedWimAfter.sha256 -eq $stagedWimBefore.sha256) {
    throw 'Phase 3ハーネス追加後もboot.wimのSHA-256が変化していません。'
}

$bootData = '-bootdata:2#p0,e,b{0}#pEF,e,b{1}' -f `
    $etfsboot, $efiBootImage
Invoke-CheckedNative `
    -Command $oscdimg `
    -Arguments @($bootData, '-u1', '-udfver102', $mediaRoot, $isoPath) `
    -Operation 'Phase 3 VM専用WinPE ISOの作成'
Assert-RegularNonReparseFile -Path $isoPath -Description '生成ISO'

$manifest = [ordered]@{
    schemaVersion = 1
    purpose = 'Phase 3 Legacy BIOS VM-only clone and BCDBoot validation media'
    generated = (Get-Date).ToString('o')
    repositoryContainsMicrosoftPayload = $false
    productWriteServiceConnected = $false
    vmOnly = $true
    fixedProfile = [ordered]@{
        firmware = 'Legacy BIOS'
        sourceDiskNumber = 0
        sourceDiskBytes = 48GB
        sourcePartitionCount = 1
        sourcePartitionType = 'MBR 0x07 Active Windows/system combined'
        targetDiskNumber = 1
        targetDiskBytes = 56GB
        network = 'none'
    }
    baseProductMedia = [ordered]@{
        root = $baseRoot
        manifestSha256 = (Get-FileHash -LiteralPath $baseManifestPath `
            -Algorithm SHA256).Hash
        bootWim = $preflight.baseWim
    }
    certificateGeneration = [string]$baseManifest.certificateGeneration
    adkVersion = [string]$candidate.deploymentToolsVersion
    dismVersion = [string]$candidate.dismFileVersion
    productApp = $productReport
    cloneHarness = $cloneReport
    bcdBootHarness = $bcdReport
    stagedWimBefore = $stagedWimBefore
    changedFiles = $addedFiles
    stagedWimAfter = $stagedWimAfter
    iso = [ordered]@{
        path = $isoPath
        length = (Get-Item -LiteralPath $isoPath).Length
        sha256 = (Get-FileHash -LiteralPath $isoPath `
            -Algorithm SHA256).Hash
    }
}
$manifestJson = $manifest | ConvertTo-Json -Depth 9
[IO.File]::WriteAllText(
    $manifestPath,
    $manifestJson,
    [Text.UTF8Encoding]::new($false))
Write-Output "PHASE3_WINPE_MEDIA_PASS=$manifestPath"
