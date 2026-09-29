[CmdletBinding()]
param(
    [ValidateRange(5, 60)]
    [int] $TimeoutMinutes = 30,
    [switch] $AllowVerifiedAbortedRecovery,
    [switch] $ReusePreparedDisks
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$PSNativeCommandUseErrorActionPreference = $false

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$workspaceRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot '..\..'))
$labRoot = Join-Path $workspaceRoot 'business-apps\ytec-windows-backup'
$credentialRoot = Join-Path $labRoot '.validation\vm-secrets'
$vboxManage = 'C:\Program Files\Oracle\VirtualBox\VBoxManage.exe'
$vmName = 'YDC-Phase5-VSS-x64'
$guestUser = 'YbcTest'
$passwordFile = Join-Path $credentialRoot `
    'YWB-Win10-22H2-x64-Clean.password.txt'
$harness = Join-Path $repoRoot `
    'out\build\msvc-x64-vm-destructive\tests\ytec-phase5-file-staging-vm.exe'
$guestRunnerSource = Join-Path $repoRoot `
    'scripts\vm\Run-Phase5FileStagingValidationElevated.ps1'
$vmFolder = Join-Path $repoRoot ".validation\vms\$vmName"
$runStamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$sourceDisk = $null
$destinationDisk = $null
if (-not $ReusePreparedDisks) {
    $sourceDisk = Join-Path $vmFolder `
        "YDC-Phase5-FileSource-128MiB-$runStamp.vdi"
    $destinationDisk = Join-Path $vmFolder `
        "YDC-Phase5-FileDestination-512MiB-$runStamp.vdi"
}
$evidenceRoot = Join-Path $repoRoot `
    ".validation\evidence\phase5-file-staging-vm\$runStamp"
$guestRoot = "C:\Users\$guestUser\YDC-Phase5-File-Staging-$runStamp"
$guestHarness = "$guestRoot\ytec-phase5-file-staging-vm.exe"
$guestRunner = "$guestRoot\Run-Phase5FileStagingValidationElevated.ps1"
$guestLauncher = `
    "C:\Users\$guestUser\Desktop\YDC_Phase5_File_Staging__DOUBLE_CLICK.cmd"
$guestDone = "$guestRoot\done.txt"

foreach ($requiredFile in @(
    $vboxManage,
    $passwordFile,
    $harness,
    $guestRunnerSource
)) {
    if (-not (Test-Path -LiteralPath $requiredFile -PathType Leaf)) {
        throw "必要ファイルがありません: $requiredFile"
    }
}
if (-not (Test-Path -LiteralPath $vmFolder -PathType Container)) {
    throw "Phase 5専用VMフォルダーがありません: $vmFolder"
}

function Invoke-VBox {
    param(
        [Parameter(Mandatory)]
        [string[]] $Arguments,
        [Parameter(Mandatory)]
        [string] $Operation
    )

    $output = @(& $vboxManage @Arguments 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "VirtualBox操作が失敗しました: $Operation`n$($output -join "`n")"
    }
    return $output
}

function Get-MachineValue {
    param(
        [Parameter(Mandatory)]
        [string[]] $Information,
        [Parameter(Mandatory)]
        [string] $Name
    )

    $line = $Information |
        Where-Object { $_ -like "$Name=*" } |
        Select-Object -First 1
    if ($line -match '^[^=]+="(.*)"$') {
        return $Matches[1]
    }
    return $null
}

function Get-AttachedMedium {
    param(
        [Parameter(Mandatory)]
        [string[]] $Information,
        [Parameter(Mandatory)]
        [ValidateRange(0, 29)]
        [int] $Port
    )

    $key = '"SATA-' + $Port + '-0"'
    $line = $Information |
        Where-Object { $_ -like "$key=*" } |
        Select-Object -First 1
    if ($line -match '^"SATA-[0-9]+-0"="(.*)"$') {
        return $Matches[1]
    }
    return $null
}

function Wait-GuestReady {
    $deadline = (Get-Date).AddMinutes(12)
    while ((Get-Date) -lt $deadline) {
        $null = & $vboxManage guestcontrol $vmName stat `
            "--username=$guestUser" `
            "--passwordfile=$passwordFile" `
            'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe' `
            2>&1
        if ($LASTEXITCODE -eq 0) {
            return
        }
        $null = & $vboxManage guestcontrol $vmName closesession --all 2>&1
        Start-Sleep -Seconds 10
    }
    throw "VMのGuestControl準備を確認できませんでした: $vmName"
}

function Wait-PowerOff {
    $deadline = (Get-Date).AddMinutes(5)
    while ((Get-Date) -lt $deadline) {
        $info = @(Invoke-VBox `
            -Arguments @('showvminfo', $vmName, '--machinereadable') `
            -Operation 'VM停止待ち')
        if ((Get-MachineValue -Information $info -Name 'VMState') -eq
            'poweroff') {
            return
        }
        Start-Sleep -Seconds 5
    }
    throw 'Phase 5専用VMが5分以内に正常停止しませんでした。'
}

function Invoke-GuestRetry {
    param(
        [Parameter(Mandatory)]
        [string[]] $Arguments,
        [Parameter(Mandatory)]
        [string] $Operation
    )

    $lastOutput = @()
    foreach ($attempt in 1..8) {
        $lastOutput = @(
            & $vboxManage guestcontrol $vmName @Arguments 2>&1)
        if ($LASTEXITCODE -eq 0) {
            return $lastOutput
        }
        Start-Sleep -Seconds 3
    }
    throw "GuestControlが失敗しました: $Operation`n$($lastOutput -join "`n")"
}

function Copy-GuestArtifact {
    param(
        [Parameter(Mandatory)]
        [string] $GuestPath,
        [Parameter(Mandatory)]
        [string] $HostPath
    )

    $null = Invoke-GuestRetry `
        -Operation "証跡回収 $GuestPath" `
        -Arguments @(
            'copyfrom',
            "--username=$guestUser",
            "--passwordfile=$passwordFile",
            $GuestPath,
            $HostPath
        )
}

$running = @(& $vboxManage list runningvms 2>&1)
if ($LASTEXITCODE -ne 0) {
    throw '稼働中VirtualBox VMを確認できませんでした。'
}
if ($running.Count -ne 0) {
    throw "別VMが稼働中のため開始しません: $($running -join ', ')"
}

$initialInfo = @(Invoke-VBox `
    -Arguments @('showvminfo', $vmName, '--machinereadable') `
    -Operation 'Phase 5専用VM事前確認')
$initialState =
    Get-MachineValue -Information $initialInfo -Name 'VMState'
if ($initialState -eq 'aborted' -and
    -not $AllowVerifiedAbortedRecovery) {
    throw 'aborted状態です。通常起動確認後にだけ-AllowVerifiedAbortedRecoveryを指定できます。'
}
if ($initialState -notin @('saved', 'poweroff', 'aborted')) {
    throw "Phase 5専用VMがsaved/poweroff/承認済みabortedではありません: $initialState"
}
if ((Get-MachineValue -Information $initialInfo -Name 'nic1') -ne 'none' -or
    (Get-MachineValue -Information $initialInfo -Name 'firmware') -ne 'EFI64') {
    throw 'Phase 5専用VMのNICまたはUEFI設定が安全条件と一致しません。'
}

New-Item -ItemType Directory -Path $evidenceRoot -Force | Out-Null
$initialInfo | Set-Content `
    -LiteralPath (Join-Path $evidenceRoot 'vm-before.txt') `
    -Encoding utf8

if ($initialState -eq 'saved' -and -not $ReusePreparedDisks) {
    $null = Invoke-VBox `
        -Arguments @('startvm', $vmName, '--type', 'headless') `
        -Operation '保存状態VMの正常停止用復帰'
    Wait-GuestReady
    $null = Invoke-GuestRetry `
        -Operation 'Phase 5専用VM正常停止' `
        -Arguments @(
            'run',
            '--exe=C:\Windows\System32\shutdown.exe',
            '--arg0=shutdown.exe',
            "--username=$guestUser",
            "--passwordfile=$passwordFile",
            '--quiet',
            '--',
            '/s',
            '/t',
            '0'
        )
    Wait-PowerOff
}

if ($ReusePreparedDisks) {
    $sourceDisk = Get-AttachedMedium `
        -Information $initialInfo `
        -Port 1
    $destinationDisk = Get-AttachedMedium `
        -Information $initialInfo `
        -Port 2
    $expectedSourcePrefix = Join-Path $vmFolder `
        'YDC-Phase5-FileSource-128MiB-'
    $expectedDestinationPrefix = Join-Path $vmFolder `
        'YDC-Phase5-FileDestination-512MiB-'
    if ([string]::IsNullOrWhiteSpace($sourceDisk) -or
        [string]::IsNullOrWhiteSpace($destinationDisk) -or
        $sourceDisk -eq 'none' -or
        $destinationDisk -eq 'none' -or
        -not [IO.Path]::GetFullPath($sourceDisk).StartsWith(
            $expectedSourcePrefix,
            [StringComparison]::OrdinalIgnoreCase) -or
        -not [IO.Path]::GetFullPath($destinationDisk).StartsWith(
            $expectedDestinationPrefix,
            [StringComparison]::OrdinalIgnoreCase) -or
        -not (Test-Path -LiteralPath $sourceDisk -PathType Leaf) -or
        -not (Test-Path -LiteralPath $destinationDisk -PathType Leaf)) {
        throw '再試験用の固定合成VDI接続を確認できません。'
    }
    $attachedInfo = $initialInfo
}
else {
    $powerOffInfo = @(Invoke-VBox `
        -Arguments @('showvminfo', $vmName, '--machinereadable') `
        -Operation 'VDI接続前VM確認')
    $attachmentReadyState =
        Get-MachineValue -Information $powerOffInfo -Name 'VMState'
    if ($attachmentReadyState -ne 'poweroff' -and
        -not (
            $attachmentReadyState -eq 'aborted' -and
            $AllowVerifiedAbortedRecovery
        )) {
        throw '合成VDI接続前にPhase 5専用VMがpoweroffではありません。'
    }
    foreach ($port in @(1, 2)) {
        $current = Get-AttachedMedium `
            -Information $powerOffInfo `
            -Port $port
        if ($current -ne 'none' -and
            -not [string]::IsNullOrWhiteSpace($current)) {
            throw "SATA port $port に想定外媒体があります。"
        }
    }

    $null = Invoke-VBox `
        -Arguments @(
            'createmedium', 'disk',
            '--filename', $sourceDisk,
            '--size', '128',
            '--format', 'VDI',
            '--variant', 'Standard'
        ) `
        -Operation '128MiB合成ソースVDI作成'
    $null = Invoke-VBox `
        -Arguments @(
            'createmedium', 'disk',
            '--filename', $destinationDisk,
            '--size', '512',
            '--format', 'VDI',
            '--variant', 'Standard'
        ) `
        -Operation '512MiB合成保存先VDI作成'

    foreach ($attachment in @(
        @{ Port = '1'; Path = $sourceDisk }
        @{ Port = '2'; Path = $destinationDisk }
    )) {
        $null = Invoke-VBox `
            -Arguments @(
                'storageattach', $vmName,
                '--storagectl', 'SATA',
                '--port', $attachment.Port,
                '--device', '0',
                '--type', 'hdd',
                '--medium', $attachment.Path,
                '--nonrotational', 'on'
            ) `
            -Operation "合成VDI接続 port $($attachment.Port)"
    }

    $attachedInfo = @(Invoke-VBox `
        -Arguments @('showvminfo', $vmName, '--machinereadable') `
        -Operation '合成VDI接続後確認')
}

foreach ($expected in @(
    @{ Port = 1; Path = $sourceDisk }
    @{ Port = 2; Path = $destinationDisk }
)) {
    $actual = Get-AttachedMedium `
        -Information $attachedInfo `
        -Port $expected.Port
    if ([string]::IsNullOrWhiteSpace($actual) -or
        $actual -eq 'none' -or
        -not [IO.Path]::GetFullPath($actual).Equals(
            [IO.Path]::GetFullPath($expected.Path),
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "SATA port $($expected.Port)の合成VDIが一致しません。"
    }
}

$harnessHash = (
    Get-FileHash -LiteralPath $harness -Algorithm SHA256).Hash
$stagingRoot = Join-Path $evidenceRoot 'guest-scripts-utf8-bom'
New-Item -ItemType Directory -Path $stagingRoot -Force | Out-Null
$stagedRunner = Join-Path $stagingRoot `
    ([IO.Path]::GetFileName($guestRunnerSource))
[IO.File]::WriteAllText(
    $stagedRunner,
    [IO.File]::ReadAllText($guestRunnerSource),
    [Text.UTF8Encoding]::new($true))
$hostLauncher = Join-Path $evidenceRoot `
    'YDC_Phase5_File_Staging__DOUBLE_CLICK.cmd'
$guestRunnerArguments =
    "-NoProfile -NonInteractive -ExecutionPolicy Bypass -File $guestRunner " +
    "-ExpectedHarnessSha256 $harnessHash"
if ($ReusePreparedDisks) {
    $guestRunnerArguments += ' -ReusePreparedDestination'
}
$launcherContent = @"
@echo off
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe' -Verb RunAs -ArgumentList '$guestRunnerArguments' -WindowStyle Hidden"
exit /b
"@
[IO.File]::WriteAllText(
    $hostLauncher,
    $launcherContent,
    [Text.ASCIIEncoding]::new())

$started = $false
try {
    $null = Invoke-VBox `
        -Arguments @('startvm', $vmName, '--type', 'gui') `
        -Operation 'Phase 5実ファイル検証VM起動'
    $started = $true
    Wait-GuestReady

    $null = Invoke-GuestRetry `
        -Operation 'ゲスト検証ディレクトリ作成' `
        -Arguments @(
            'mkdir',
            '--parents',
            "--username=$guestUser",
            "--passwordfile=$passwordFile",
            $guestRoot
        )
    foreach ($copy in @(
        @{ Host = $harness; Guest = $guestHarness }
        @{ Host = $stagedRunner; Guest = $guestRunner }
        @{ Host = $hostLauncher; Guest = $guestLauncher }
    )) {
        $null = Invoke-GuestRetry `
            -Operation "検証ファイル配置 $($copy.Guest)" `
            -Arguments @(
                'copyto',
                "--username=$guestUser",
                "--passwordfile=$passwordFile",
                $copy.Host,
                $copy.Guest
            )
    }

    [ordered]@{
        schemaVersion = 1
        status = 'PREPARED'
        vmName = $vmName
        guestRoot = $guestRoot
        guestLauncher = $guestLauncher
        sourceDisk = $sourceDisk
        sourceDiskBytes = [UInt64](128MB)
        destinationDisk = $destinationDisk
        destinationDiskBytes = [UInt64](512MB)
        harnessSha256 = $harnessHash
        reusePreparedDisks = [bool]$ReusePreparedDisks
        nic = 'none'
        firmware = 'EFI64'
        preparedUtc = [DateTimeOffset]::UtcNow
    } | ConvertTo-Json -Depth 5 | Set-Content `
        -LiteralPath (Join-Path $evidenceRoot 'host-preparation.json') `
        -Encoding utf8

    Write-Output 'MANUAL_UAC_REQUIRED'
    Write-Output "VM: $vmName"
    Write-Output "Launcher: $guestLauncher"
    Write-Output "Evidence: $evidenceRoot"

    $deadline = (Get-Date).AddMinutes($TimeoutMinutes)
    $doneFound = $false
    while ((Get-Date) -lt $deadline) {
        $null = & $vboxManage guestcontrol $vmName stat `
            "--username=$guestUser" `
            "--passwordfile=$passwordFile" `
            $guestDone 2>&1
        if ($LASTEXITCODE -eq 0) {
            $doneFound = $true
            break
        }
        Start-Sleep -Seconds 5
    }
    if (-not $doneFound) {
        throw "UAC承認後のVM試験が${TimeoutMinutes}分以内に完了しませんでした。"
    }

    foreach ($artifact in @(
        'summary.json',
        'done.txt',
        'harness-stdout.txt',
        'harness-stderr.txt'
    )) {
        Copy-GuestArtifact `
            -GuestPath "$guestRoot\$artifact" `
            -HostPath (Join-Path $evidenceRoot $artifact)
    }
    $done = (
        Get-Content -LiteralPath (Join-Path $evidenceRoot 'done.txt') -Raw
    ).Trim()
    $summary = Get-Content `
        -LiteralPath (Join-Path $evidenceRoot 'summary.json') `
        -Raw |
        ConvertFrom-Json
    if ($done -ne 'PASS' -or $summary.result -ne 'PASS') {
        throw "Phase 5実ファイルVM試験が失敗しました: $($summary.message)"
    }
    $hostDcimg = Join-Path $evidenceRoot 'synthetic.dcimg'
    Copy-GuestArtifact `
        -GuestPath 'T:\YDC-Phase5-File-Staging\synthetic.dcimg' `
        -HostPath $hostDcimg
    $hostDcimgInfo = Get-Item -LiteralPath $hostDcimg
    $hostDcimgHash = (
        Get-FileHash -LiteralPath $hostDcimg -Algorithm SHA256).Hash
    if ([UInt64]$hostDcimgInfo.Length -ne [UInt64]$summary.finalBytes -or
        $hostDcimgHash -cne [string]$summary.finalSha256) {
        throw 'VMから回収したdcimgの長さまたはSHA-256がゲスト証跡と一致しません。'
    }

    [ordered]@{
        schemaVersion = 1
        result = 'PASS'
        vmName = $vmName
        firmware = 'EFI64'
        nic = 'none'
        interactiveUac = $true
        sourceDiskBytes = [UInt64]$summary.sourceDiskBytes
        destinationDiskBytes = [UInt64]$summary.destinationDiskBytes
        imageBytes = [UInt64]$summary.finalBytes
        imageSha256 = [string]$summary.finalSha256
        partialExists = [bool]$summary.partialExists
        hostArtifactVerified = $true
        completedUtc = [DateTimeOffset]::UtcNow
    } | ConvertTo-Json -Depth 5 | Set-Content `
        -LiteralPath (Join-Path $evidenceRoot 'host-summary.json') `
        -Encoding utf8

    Write-Output 'Phase 5 file staging VM test: PASS'
    Write-Output "Evidence: $evidenceRoot"
}
finally {
    if ($started) {
        $currentInfo = @(& $vboxManage showvminfo `
            $vmName `
            --machinereadable 2>&1)
        $currentState =
            Get-MachineValue -Information $currentInfo -Name 'VMState'
        if ($currentState -eq 'running') {
            $null = & $vboxManage controlvm $vmName savestate 2>&1
            if ($LASTEXITCODE -ne 0) {
                Write-Warning 'Phase 5専用VMを保存状態へ戻せませんでした。'
            }
        }
    }
}
