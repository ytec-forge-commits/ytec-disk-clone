param(
    [Parameter(Mandatory)]
    [string]$BaseProductMediaRoot,

    [Parameter(Mandatory)]
    [string]$OutputRoot,

    [switch]$BuildMedia,

    [switch]$BcdBootResume
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
        throw "$Description のファイルサイズが許可範囲外です。"
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
    if ([BitConverter]::ToUInt16($bytes, $peOffset + 4) -ne 0x8664 -or
        [BitConverter]::ToUInt16($bytes, $peOffset + 24) -ne 0x020B) {
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
    throw '基礎WinPEメディアはリポジトリ外の検証済み生成物だけを使用できます。'
}
Assert-NoReparsePointInTree -Path $baseRoot -Description '基礎WinPEメディア'

$baseManifestPath = Join-Path $baseRoot 'winpe-app-media-manifest.json'
$baseBootWim = Join-Path $baseRoot 'working\media\sources\boot.wim'
Assert-RegularNonReparseFile -Path $baseManifestPath `
    -Description '基礎メディアmanifest'
Assert-RegularNonReparseFile -Path $baseBootWim `
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
Assert-RegularNonReparseFile -Path $diagnostic `
    -Description 'WinPE環境診断CLI'
$diagnosticReport = (& $diagnostic --json | Out-String) | ConvertFrom-Json
if ($LASTEXITCODE -ne 0 -or
    -not $diagnosticReport.mediaCreationPermitted -or
    $null -eq $diagnosticReport.selectedCandidateIndex) {
    throw 'WinPE環境診断の作成許可ゲートを通過していません。'
}
$candidate = $diagnosticReport.candidates[
    [int]$diagnosticReport.selectedCandidateIndex]
$adkRoot = [IO.Path]::GetFullPath([string]$candidate.root)
$oscdimgRoot = Join-Path $adkRoot 'Deployment Tools\amd64\Oscdimg'
$dism = Join-Path $adkRoot 'Deployment Tools\amd64\DISM\dism.exe'
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
Assert-RegularNonReparseFile -Path $efiBootImage `
    -Description 'UEFIブートイメージ'

$productApp = Join-Path $repoRoot `
    'out\build\msvc-x64-vm\src\WinPEApp\ytec-winpe-app.exe'
$mbr2GptHarness = Join-Path $repoRoot `
    'out\build\msvc-x64-vm-destructive\tests\ytec-phase4-mbr2gpt-vm.exe'
$bcdHarness = Join-Path $repoRoot `
    'out\build\msvc-x64-vm-destructive\tests\ytec-phase4-bcdboot-vm.exe'
$productReport = Get-Amd64PeReport `
    -Path $productApp `
    -Description '読み取り専用WinPEApp' `
    -AllowedDependencies @(
        'ADVAPI32.dll', 'CRYPT32.dll', 'KERNEL32.dll', 'SETUPAPI.dll',
        'WINTRUST.dll')
$mbr2GptReport = Get-Amd64PeReport `
    -Path $mbr2GptHarness `
    -Description 'Phase 4 MBR2GPT VM専用ハーネス' `
    -AllowedDependencies @(
        'ADVAPI32.dll', 'bcrypt.dll', 'CRYPT32.dll', 'KERNEL32.dll',
        'ole32.dll', 'SETUPAPI.dll', 'WINTRUST.dll')
$bcdReport = Get-Amd64PeReport `
    -Path $bcdHarness `
    -Description 'Phase 4 UEFI BCDBoot VM専用ハーネス' `
    -AllowedDependencies @(
        'ADVAPI32.dll', 'CRYPT32.dll', 'KERNEL32.dll', 'SETUPAPI.dll',
        'WINTRUST.dll')
if ($productReport.sha256 -ne [string]$baseManifest.winpeApp.sha256) {
    throw '基礎メディアのWinPEAppと現在の静的ランタイム製品が一致しません。'
}

$profileName = if ($BcdBootResume) {
    'bcdboot-resume'
} else {
    'mbr2gpt'
}
$purpose = if ($BcdBootResume) {
    'Phase 4 UEFI BCDBoot resume VM-only validation media'
} else {
    'Phase 4 MBR2GPT and UEFI BCDBoot VM-only validation media'
}
$preflight = [ordered]@{
    schemaVersion = 1
    purpose = $purpose
    profile = $profileName
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
    mbr2GptHarness = $mbr2GptReport
    bcdBootHarness = $bcdReport
    buildRequested = [bool]$BuildMedia
    administrator = Test-IsAdministrator
}
if (-not $BuildMedia) {
    Write-Output ('PHASE4_WINPE_MEDIA_PREFLIGHT_PASS=' +
        ($preflight | ConvertTo-Json -Depth 7 -Compress))
    return
}
if (-not $preflight.administrator) {
    throw '実WIMのマウント/コミットには管理者権限が必要です。UACを承認したpwshで-BuildMediaを実行してください。'
}

$baseMediaRoot = Join-Path $baseRoot 'working\media'
Assert-NoReparsePointInTree -Path $baseMediaRoot `
    -Description '基礎WinPE mediaツリー'
$workingRoot = Join-Path $outputFullPath 'working'
$mediaRoot = Join-Path $workingRoot 'media'
$mountRoot = Join-Path $workingRoot 'mount'
$bootWim = Join-Path $mediaRoot 'sources\boot.wim'
$isoProfileName = if ($BcdBootResume) {
    'BCDBoot-Resume'
} else {
    'MBR2GPT'
}
$isoPath = Join-Path $outputFullPath (
    'YDC-Phase4-{0}-VMOnly-amd64-{1}.iso' -f
        $isoProfileName,
        $baseManifest.certificateGeneration)
$manifestPath = Join-Path $outputFullPath `
    'phase4-winpe-media-manifest.json'

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
$diskPartReport = $null
try {
    Invoke-CheckedNative -Command $dism -Arguments @(
        '/Mount-Image',
        "/ImageFile:$bootWim",
        '/Index:1',
        "/MountDir:$mountRoot"
    ) -Operation 'Phase 4 WinPE boot.wimのマウント'
    $mounted = $true

    $payloadRoot = Join-Path $mountRoot 'YtecDiskClone'
    $mountedApp = Join-Path $payloadRoot 'ytec-winpe-app.exe'
    $mountedMbr2Gpt = Join-Path $payloadRoot `
        'ytec-phase4-mbr2gpt-vm.exe'
    $mountedBcd = Join-Path $payloadRoot 'ytec-phase4-bcdboot-vm.exe'
    $mountedLaunch = Join-Path $payloadRoot 'launch.cmd'
    $mountedWinpeShell = Join-Path $mountRoot `
        'Windows\System32\winpeshl.ini'
    $mountedDiskPart = Join-Path $mountRoot 'Windows\System32\diskpart.exe'
    if (-not (Test-Path -LiteralPath $mountedDiskPart -PathType Leaf)) {
        throw "WinPE DiskPartが基礎WIM内にありません: $mountedDiskPart"
    }
    $diskPartReport = [ordered]@{
        relativePath = $mountedDiskPart.Substring(
            $mountRoot.Length).TrimStart('\')
        length = (Get-Item -LiteralPath $mountedDiskPart).Length
        sha256 = (Get-FileHash -LiteralPath $mountedDiskPart `
            -Algorithm SHA256).Hash
        trustAnchor = 'SHA-256-validated Microsoft WinPE base WIM'
        standaloneAuthenticodeClaimed = $false
    }
    foreach ($required in @(
            $mountedApp, $mountedLaunch, $mountedWinpeShell)) {
        if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
            throw "基礎WIM内の予約済み製品ファイルがありません: $required"
        }
    }
    if ((Get-FileHash -LiteralPath $mountedApp -Algorithm SHA256).Hash -ne
        $productReport.sha256) {
        throw '基礎WIM内のWinPEAppが現在の製品ハッシュと一致しません。'
    }
    foreach ($reserved in @($mountedMbr2Gpt, $mountedBcd)) {
        if (Test-Path -LiteralPath $reserved) {
            throw "基礎WIM内のPhase 4予約先は上書きしません: $reserved"
        }
    }
    Copy-Item -LiteralPath $mbr2GptHarness -Destination $mountedMbr2Gpt
    Copy-Item -LiteralPath $bcdHarness -Destination $mountedBcd

    $passLine = if ($BcdBootResume) {
        'echo YDC_PHASE4_BCDBOOT_RESUME_PASS>>"%LOG%"'
    } else {
        'echo YDC_PHASE4_AUTOMATION_PASS>>"%LOG%"'
    }
    $launchLines = [Collections.Generic.List[string]]::new()
    $launchLines.AddRange([string[]]@(
        '@echo off',
        'setlocal EnableExtensions EnableDelayedExpansion',
        'chcp 65001 >nul',
        'set "ROOT=%SYSTEMDRIVE%\YtecDiskClone"',
        'set "LOG=%SYSTEMDRIVE%\YDC-Phase4-validation.log"',
        'mode com1: BAUD=115200 PARITY=n DATA=8 STOP=1 >nul 2>&1',
        'echo YDC_PHASE4_AUTOMATION_BEGIN>COM1',
        'echo YDC_PHASE4_AUTOMATION_BEGIN>"%LOG%"',
        ('echo profile={0} targetDisk=0 targetBytes=60129542144>>"%LOG%"' -f
            $profileName),
        'set "WINROOT="',
        'for %%D in (C D E F G H I J K L M N O P Q R S T U V W Y Z) do if not defined WINROOT if exist "%%D:\Windows\System32\ntoskrnl.exe" set "WINROOT=%%D:\"',
        'if not defined WINROOT goto windows_not_found',
        'echo windowsRoot=!WINROOT!>>"%LOG%"'
    ))
    if ($BcdBootResume) {
        $launchLines.Add('echo conversionSkipped=true reason=explicit-bcdboot-resume-profile>>"%LOG%"')
    } else {
        $launchLines.AddRange([string[]]@(
            '"%ROOT%\ytec-phase4-mbr2gpt-vm.exe" --plan --target 0 --windows-root !WINROOT! >"%ROOT%\mbr2gpt-plan.txt" 2>&1',
            'set "STEP_EXIT=!ERRORLEVEL!"',
            'type "%ROOT%\mbr2gpt-plan.txt">>"%LOG%"',
            'if not "!STEP_EXIT!"=="0" goto mbr2gpt_plan_failed',
            'for /f "usebackq tokens=1,* delims==" %%A in ("%ROOT%\mbr2gpt-plan.txt") do if /i "%%A"=="confirmation" set "MBR2GPT_CONFIRM=%%B"',
            'if not defined MBR2GPT_CONFIRM goto mbr2gpt_confirmation_failed',
            '"%ROOT%\ytec-phase4-mbr2gpt-vm.exe" --execute --target 0 --windows-root !WINROOT! --authorization YTEC-VM-ONLY-PHASE4-MBR2GPT --confirmation "!MBR2GPT_CONFIRM!" >>"%LOG%" 2>&1',
            'if errorlevel 1 goto mbr2gpt_execute_failed'
        ))
    }
    $launchLines.AddRange([string[]]@(
        'echo inventoryAfterConvertBegin>>"%LOG%"',
        '"%ROOT%\ytec-winpe-app.exe" --text >>"%LOG%" 2>&1',
        'if errorlevel 1 goto inventory_failed',
        'echo inventoryAfterConvertEnd>>"%LOG%"',
        'if exist S:\ goto esp_letter_in_use',
        '> "%ROOT%\mount-esp.txt" echo select disk 0',
        '>>"%ROOT%\mount-esp.txt" echo select partition 2',
        '>>"%ROOT%\mount-esp.txt" echo assign letter=S',
        '> "%ROOT%\unmount-esp.txt" echo select disk 0',
        '>>"%ROOT%\unmount-esp.txt" echo select partition 2',
        '>>"%ROOT%\unmount-esp.txt" echo remove letter=S',
        'diskpart.exe /s "%ROOT%\mount-esp.txt" >>"%LOG%" 2>&1',
        'set "STEP_EXIT=!ERRORLEVEL!"',
        'if not "!STEP_EXIT!"=="0" goto esp_mount_failed',
        'if not exist S:\ goto esp_mount_failed',
        '"%ROOT%\ytec-phase4-bcdboot-vm.exe" --plan --target 0 --windows-root !WINROOT! --esp-root S:\ >"%ROOT%\bcdboot-plan.txt" 2>&1',
        'set "STEP_EXIT=!ERRORLEVEL!"',
        'type "%ROOT%\bcdboot-plan.txt">>"%LOG%"',
        'if not "!STEP_EXIT!"=="0" goto bcdboot_plan_failed',
        'for /f "usebackq tokens=1,* delims==" %%A in ("%ROOT%\bcdboot-plan.txt") do if /i "%%A"=="confirmation" set "BCD_CONFIRM=%%B"',
        'if not defined BCD_CONFIRM goto bcdboot_confirmation_failed',
        '"%ROOT%\ytec-phase4-bcdboot-vm.exe" --execute --target 0 --windows-root !WINROOT! --esp-root S:\ --authorization YTEC-VM-ONLY-PHASE4-UEFI-BCDBOOT --confirmation "!BCD_CONFIRM!" >>"%LOG%" 2>&1',
        'if errorlevel 1 goto bcdboot_execute_failed',
        'diskpart.exe /s "%ROOT%\unmount-esp.txt" >>"%LOG%" 2>&1',
        'set "STEP_EXIT=!ERRORLEVEL!"',
        'if not "!STEP_EXIT!"=="0" goto esp_unmount_failed',
        'if exist S:\ goto esp_unmount_failed',
        'if exist X:\Windows\setupact.log copy /y X:\Windows\setupact.log "!WINROOT!YDC-MBR2GPT-setupact.log" >nul',
        'if exist X:\Windows\setuperr.log copy /y X:\Windows\setuperr.log "!WINROOT!YDC-MBR2GPT-setuperr.log" >nul',
        'if exist X:\Windows\DiagErr.xml copy /y X:\Windows\DiagErr.xml "!WINROOT!YDC-MBR2GPT-DiagErr.xml" >nul',
        'if exist X:\Windows\DiagWrn.xml copy /y X:\Windows\DiagWrn.xml "!WINROOT!YDC-MBR2GPT-DiagWrn.xml" >nul',
        $passLine,
        'copy /y "%LOG%" "!WINROOT!YDC-Phase4-validation.log" >nul',
        'type "%LOG%">COM1',
        'type "%LOG%"',
        'goto end',
        ':windows_not_found',
        'echo YDC_PHASE4_AUTOMATION_FAIL stage=windows-root>>"%LOG%"',
        'goto failed',
        ':mbr2gpt_plan_failed',
        'echo YDC_PHASE4_AUTOMATION_FAIL stage=mbr2gpt-plan exit=!STEP_EXIT!>>"%LOG%"',
        'goto failed',
        ':mbr2gpt_confirmation_failed',
        'echo YDC_PHASE4_AUTOMATION_FAIL stage=mbr2gpt-confirmation>>"%LOG%"',
        'goto failed',
        ':mbr2gpt_execute_failed',
        'echo YDC_PHASE4_AUTOMATION_FAIL stage=mbr2gpt-execute exit=!ERRORLEVEL!>>"%LOG%"',
        'goto failed',
        ':inventory_failed',
        'echo YDC_PHASE4_AUTOMATION_FAIL stage=post-convert-inventory exit=!ERRORLEVEL!>>"%LOG%"',
        'goto failed',
        ':esp_letter_in_use',
        'echo YDC_PHASE4_AUTOMATION_FAIL stage=esp-letter-in-use>>"%LOG%"',
        'goto failed',
        ':esp_mount_failed',
        'echo YDC_PHASE4_AUTOMATION_FAIL stage=esp-mount exit=!STEP_EXIT!>>"%LOG%"',
        'goto failed',
        ':bcdboot_plan_failed',
        'echo YDC_PHASE4_AUTOMATION_FAIL stage=bcdboot-plan exit=!STEP_EXIT!>>"%LOG%"',
        'goto failed',
        ':bcdboot_confirmation_failed',
        'echo YDC_PHASE4_AUTOMATION_FAIL stage=bcdboot-confirmation>>"%LOG%"',
        'goto failed',
        ':bcdboot_execute_failed',
        'echo YDC_PHASE4_AUTOMATION_FAIL stage=bcdboot-execute exit=!ERRORLEVEL!>>"%LOG%"',
        'goto failed',
        ':esp_unmount_failed',
        'echo YDC_PHASE4_AUTOMATION_FAIL stage=esp-unmount exit=!STEP_EXIT!>>"%LOG%"',
        ':failed',
        'if exist S:\ diskpart.exe /s "%ROOT%\unmount-esp.txt" >>"%LOG%" 2>&1',
        'if defined WINROOT copy /y "%LOG%" "!WINROOT!YDC-Phase4-validation.log" >nul 2>&1',
        'type "%LOG%">COM1',
        'type "%LOG%"',
        ':end',
        'echo.',
        'echo Phase 4 VM-only validation finished. Review the marker above.'
    ))
    $launchLines | Set-Content -LiteralPath $mountedLaunch -Encoding ascii
    @(
        '[LaunchApps]',
        '%SYSTEMROOT%\System32\wpeinit.exe',
        '%SYSTEMROOT%\System32\cmd.exe, /k %SYSTEMDRIVE%\YtecDiskClone\launch.cmd'
    ) | Set-Content -LiteralPath $mountedWinpeShell -Encoding ascii

    $changedFiles = @(
        foreach ($file in @(
                $mountedMbr2Gpt,
                $mountedBcd,
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
    if ((Get-FileHash -LiteralPath $mountedMbr2Gpt `
            -Algorithm SHA256).Hash -ne $mbr2GptReport.sha256 -or
        (Get-FileHash -LiteralPath $mountedBcd `
            -Algorithm SHA256).Hash -ne $bcdReport.sha256) {
        throw 'WIMへ追加したPhase 4ハーネスのSHA-256が一致しません。'
    }

    Invoke-CheckedNative -Command $dism -Arguments @(
        '/Unmount-Image',
        "/MountDir:$mountRoot",
        '/Commit',
        '/CheckIntegrity'
    ) -Operation 'Phase 4 WinPE boot.wimのコミット'
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
    throw 'Phase 4ハーネス追加後もboot.wimのSHA-256が変化していません。'
}

$bootData = '-bootdata:2#p0,e,b{0}#pEF,e,b{1}' -f `
    $etfsboot, $efiBootImage
Invoke-CheckedNative -Command $oscdimg -Arguments @(
    $bootData, '-u1', '-udfver102', $mediaRoot, $isoPath
) -Operation 'Phase 4 VM専用WinPE ISOの作成'
Assert-RegularNonReparseFile -Path $isoPath -Description '生成ISO'

$manifest = [ordered]@{
    schemaVersion = 1
    purpose = $purpose
    generated = (Get-Date).ToString('o')
    repositoryContainsMicrosoftPayload = $false
    productWriteServiceConnected = $false
    vmOnly = $true
    profile = $profileName
    fixedProfile = [ordered]@{
        initialFirmware = 'Legacy BIOS'
        finalFirmware = 'UEFI64 with Secure Boot'
        targetDiskNumber = 0
        targetDiskBytes = 56GB
        targetWindowsArchitecture = 'AMD64'
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
    mbr2GptHarness = $mbr2GptReport
    bcdBootHarness = $bcdReport
    winpeDiskPart = $diskPartReport
    stagedWimBefore = $stagedWimBefore
    changedFiles = $changedFiles
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
Write-Output "PHASE4_WINPE_MEDIA_PASS=$manifestPath"
