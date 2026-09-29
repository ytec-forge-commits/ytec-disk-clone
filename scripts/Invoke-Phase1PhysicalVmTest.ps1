[CmdletBinding()]
param(
    [switch] $InteractiveUac,
    [switch] $ReuseRunningHelper
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$PSNativeCommandUseErrorActionPreference = $false

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$workspaceRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot '..\..'))
$labRoot = Join-Path $workspaceRoot 'business-apps\ytec-windows-backup'
$labVmRoot = Join-Path $labRoot '.validation\vms'
$credentialRoot = Join-Path $labRoot '.validation\vm-secrets'
$vboxManage = 'C:\Program Files\Oracle\VirtualBox\VBoxManage.exe'
$sourceVm = 'YWB-Win11-25H2-x64-Clean'
$helperVm = 'YDC-Phase1-Physical-Helper'
$guestUser = 'YbcTest'
$passwordFile = Join-Path $credentialRoot "$sourceVm.password.txt"
$helperFolder = Join-Path $labVmRoot $helperVm
$helperSystemDisk = Join-Path $helperFolder 'YDC-Phase1-Helper-System.vdi'
$syntheticSourceDisk = Join-Path $helperFolder 'YDC-Phase1-Synthetic-Source-2GiB.vdi'
$syntheticTargetDisk = Join-Path $helperFolder 'YDC-Phase1-Synthetic-Target-3GiB.vdi'
$harness = Join-Path $repoRoot `
    'out\build\msvc-x64-vm-destructive\tests\ytec-phase1-physical-clone-vm.exe'
$guestInitScript = Join-Path $repoRoot `
    'scripts\vm\Initialize-Phase1SyntheticDisks.ps1'
$guestValidationScript = Join-Path $repoRoot `
    'scripts\vm\Test-Phase1SyntheticClone.ps1'
$guestCloneScript = Join-Path $repoRoot `
    'scripts\vm\Invoke-Phase1PhysicalClone.ps1'
$guestElevatedRunner = Join-Path $repoRoot `
    'scripts\vm\Run-Phase1PhysicalCloneElevated.ps1'

foreach ($requiredFile in @(
    $vboxManage,
    $passwordFile,
    $harness,
    $guestInitScript,
    $guestValidationScript,
    $guestCloneScript,
    $guestElevatedRunner
)) {
    if (-not (Test-Path -LiteralPath $requiredFile -PathType Leaf)) {
        throw "必要ファイルがありません: $requiredFile"
    }
}

function Invoke-VBox {
    param(
        [Parameter(Mandatory)]
        [string[]] $Arguments,
        [Parameter(Mandatory)]
        [string] $Operation
    )

    $output = & $vboxManage @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "VirtualBox操作が失敗しました: $Operation`n$($output -join "`n")"
    }
    return @($output)
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

function Test-VmRegistered {
    param([Parameter(Mandatory)][string] $Name)

    return @(& $vboxManage list vms) -match ('^"' + [regex]::Escape($Name) + '" ')
}

function Wait-GuestReady {
    param(
        [Parameter(Mandatory)]
        [string] $TargetVm,
        [switch] $FileSystemOnly
    )

    $deadline = (Get-Date).AddMinutes(12)
    while ((Get-Date) -lt $deadline) {
        if ($FileSystemOnly) {
            # The interactive path never needs an unelevated guest process.
            # A read-only stat verifies Guest Additions and credentials without
            # exercising VirtualBox's separate guest-process launch path.
            $null = & $vboxManage guestcontrol $TargetVm stat `
                "--username=$guestUser" `
                "--passwordfile=$passwordFile" `
                'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe' `
                2>&1
        }
        else {
            $null = & $vboxManage guestcontrol $TargetVm run `
                '--exe=C:\Windows\System32\cmd.exe' `
                '--arg0=cmd.exe' `
                "--username=$guestUser" `
                "--passwordfile=$passwordFile" `
                --quiet `
                --wait-stdout `
                -- `
                /d `
                /c `
                exit `
                0 2>&1
        }
        if ($LASTEXITCODE -eq 0) {
            return
        }
        # Saved-state resume can leave a terminated GuestControl session that
        # blocks the next login even though no guest process is active.
        $null = & $vboxManage guestcontrol $TargetVm closesession --all 2>&1
        Start-Sleep -Seconds 10
    }
    throw "VMのGuestControl準備を確認できませんでした: $TargetVm"
}

function Test-GuestAdministrator {
    $command = @'
$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if ($principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    'YDC_ADMIN_TRUE'
}
else {
    'YDC_ADMIN_FALSE'
}
'@
    $encodedCommand = [Convert]::ToBase64String(
        [Text.Encoding]::Unicode.GetBytes($command))
    $output = @(& $vboxManage guestcontrol $helperVm run `
        '--exe=C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe' `
        '--arg0=powershell.exe' `
        "--username=$guestUser" `
        "--passwordfile=$passwordFile" `
        --wait-stdout `
        --wait-stderr `
        -- `
        -NoProfile `
        -NonInteractive `
        -EncodedCommand `
        $encodedCommand 2>&1)
    return $LASTEXITCODE -eq 0 -and $output -contains 'YDC_ADMIN_TRUE'
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
        $lastOutput = @(& $vboxManage guestcontrol $helperVm @Arguments 2>&1)
        if ($LASTEXITCODE -eq 0) {
            return $lastOutput
        }
        Start-Sleep -Seconds 3
    }
    throw "GuestControlが失敗しました: $Operation`n$($lastOutput -join "`n")"
}

function Get-JsonResult {
    param(
        [Parameter(Mandatory)]
        [object[]] $Output,
        [Parameter(Mandatory)]
        [string] $Operation
    )

    $jsonLine = @($Output | Where-Object {
        $_ -is [string] -and $_.TrimStart().StartsWith('{')
    }) | Select-Object -Last 1
    if ([string]::IsNullOrWhiteSpace($jsonLine)) {
        throw "$Operation のJSON結果がありません。"
    }
    return $jsonLine | ConvertFrom-Json
}

function Invoke-GuestRecordedPowerShell {
    param(
        [Parameter(Mandatory)]
        [string] $ScriptPath,
        [Parameter(Mandatory)]
        [string[]] $Parameters,
        [Parameter(Mandatory)]
        [string] $GuestResultPath,
        [Parameter(Mandatory)]
        [string] $GuestDonePath,
        [Parameter(Mandatory)]
        [string] $HostResultPath,
        [Parameter(Mandatory)]
        [string] $Operation
    )

    $commandParts = [Collections.Generic.List[string]]::new()
    $escapedScriptPath = $ScriptPath.Replace("'", "''")
    $commandParts.Add("& '$escapedScriptPath'")
    foreach ($parameter in $Parameters) {
        if ($parameter -match '^-[A-Za-z][A-Za-z0-9]*$') {
            $commandParts.Add($parameter)
        }
        else {
            $escapedParameter = $parameter.Replace("'", "''")
            $commandParts.Add("'$escapedParameter'")
        }
    }
    $encodedCommand = [Convert]::ToBase64String(
        [Text.Encoding]::Unicode.GetBytes(($commandParts -join ' ')))

    $runArguments = @(
        'guestcontrol', $helperVm, 'run',
        '--exe=C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe',
        '--arg0=powershell.exe',
        "--username=$guestUser",
        "--passwordfile=$passwordFile",
        '--wait-stdout',
        '--wait-stderr',
        '--',
        '-NoProfile',
        '-NonInteractive',
        '-ExecutionPolicy',
        'Bypass',
        '-EncodedCommand',
        $encodedCommand
    )
    $null = & $vboxManage @runArguments 2>&1

    $deadline = (Get-Date).AddMinutes(10)
    $doneFound = $false
    while ((Get-Date) -lt $deadline) {
        $null = & $vboxManage guestcontrol $helperVm stat `
            "--username=$guestUser" `
            "--passwordfile=$passwordFile" `
            $GuestDonePath 2>&1
        if ($LASTEXITCODE -eq 0) {
            $doneFound = $true
            break
        }
        Start-Sleep -Seconds 5
    }
    if (-not $doneFound) {
        throw "$Operation のゲスト完了マーカーを確認できませんでした。"
    }

    $hostDonePath = "$HostResultPath.done.txt"
    $null = Invoke-GuestRetry `
        -Operation "$Operation result copyfrom" `
        -Arguments @(
            'copyfrom',
            "--username=$guestUser",
            "--passwordfile=$passwordFile",
            $GuestResultPath,
            $HostResultPath
        )
    $null = Invoke-GuestRetry `
        -Operation "$Operation done copyfrom" `
        -Arguments @(
            'copyfrom',
            "--username=$guestUser",
            "--passwordfile=$passwordFile",
            $GuestDonePath,
            $hostDonePath
        )
    $done = (Get-Content -LiteralPath $hostDonePath -Raw).Trim()
    $output = @(Get-Content -LiteralPath $HostResultPath)
    if ($done -ne 'PASS') {
        throw "$Operation がFAILを記録しました。`n$($output -join "`n")"
    }
    return $output
}

$runningVms = @(& $vboxManage list runningvms)
if ($ReuseRunningHelper) {
    if (-not $InteractiveUac -or
        $runningVms.Count -ne 1 -or
        $runningVms[0] -notmatch ('^"' + [regex]::Escape($helperVm) + '" ')) {
        throw '再利用モードは、対話UAC指定かつ専用ヘルパーVMだけが稼働中の場合に限ります。'
    }
}
elseif ($runningVms.Count -ne 0) {
    throw '別のVMが稼働中です。VMラボの直列利用を守るため開始しません。'
}

$sourceInfo = @(Invoke-VBox `
    -Arguments @('showvminfo', $sourceVm, '--machinereadable') `
    -Operation '参照元VM情報取得')
$sourceSystemDisk = Get-MachineValue -Information $sourceInfo -Name '"SATA-0-0"'
if ([string]::IsNullOrWhiteSpace($sourceSystemDisk)) {
    # Get-MachineValue handles keys without quotes; recover the quoted VBox key safely.
    $sourceLine = $sourceInfo | Where-Object { $_ -like '"SATA-0-0"=*' } |
        Select-Object -First 1
    if ($sourceLine -match '^"SATA-0-0"="(.*)"$') {
        $sourceSystemDisk = $Matches[1]
    }
}
$sourceSystemDisk = [IO.Path]::GetFullPath($sourceSystemDisk)
$canonicalLabRoot = [IO.Path]::GetFullPath($labVmRoot)
if (-not $sourceSystemDisk.StartsWith(
        $canonicalLabRoot,
        [StringComparison]::OrdinalIgnoreCase) -or
    -not (Test-Path -LiteralPath $sourceSystemDisk -PathType Leaf)) {
    throw '参照元システムVDIが共有VMラボ内の正規ファイルではありません。'
}

if (-not (Test-VmRegistered -Name $helperVm)) {
    $null = Invoke-VBox `
        -Arguments @(
            'createvm',
            '--name', $helperVm,
            '--basefolder', $labVmRoot,
            '--ostype', 'Windows11_64',
            '--register'
        ) `
        -Operation 'Phase 1専用ヘルパーVM作成'
    $null = Invoke-VBox `
        -Arguments @(
            'modifyvm', $helperVm,
            '--memory', '4096',
            '--cpus', '2',
            '--firmware', 'efi',
            '--boot1', 'disk',
            '--boot2', 'none',
            '--boot3', 'none',
            '--boot4', 'none',
            '--nic1', 'none',
            '--audio-enabled', 'off',
            '--usb-ohci', 'off',
            '--usb-ehci', 'off',
            '--usb-xhci', 'off'
        ) `
        -Operation 'Phase 1専用ヘルパーVM安全設定'
    $null = Invoke-VBox `
        -Arguments @('setextradata', $helperVm, 'YTEC/ValidationScope', 'ytec-disk-clone-phase1') `
        -Operation 'Phase 1専用VM識別設定'
    $null = Invoke-VBox `
        -Arguments @(
            'storagectl', $helperVm,
            '--name', 'SATA',
            '--add', 'sata',
            '--controller', 'IntelAhci',
            '--portcount', '4',
            '--bootable', 'on'
        ) `
        -Operation 'Phase 1専用SATAコントローラー作成'
}

$helperInfo = @(Invoke-VBox `
    -Arguments @('showvminfo', $helperVm, '--machinereadable') `
    -Operation 'Phase 1専用VM情報取得')
$helperState = Get-MachineValue -Information $helperInfo -Name 'VMState'
if ($helperState -notin @('poweroff', 'saved') -and
    -not ($ReuseRunningHelper -and $helperState -eq 'running')) {
    throw "Phase 1専用VMが安全に再利用できる状態ではありません: $helperState"
}

if (-not (Test-Path -LiteralPath $helperSystemDisk -PathType Leaf)) {
    if ($helperState -ne 'poweroff') {
        throw '保存状態の専用VMへ新規ディスクを追加しません。'
    }
    $null = Invoke-VBox `
        -Arguments @(
            'clonemedium', 'disk',
            $sourceSystemDisk,
            $helperSystemDisk,
            '--format', 'VDI'
        ) `
        -Operation 'ヘルパー用WindowsシステムVDIのフルクローン'
}
if (-not (Test-Path -LiteralPath $syntheticSourceDisk -PathType Leaf)) {
    if ($helperState -ne 'poweroff') {
        throw '保存状態の専用VMへ新規ディスクを追加しません。'
    }
    $null = Invoke-VBox `
        -Arguments @(
            'createmedium', 'disk',
            '--filename', $syntheticSourceDisk,
            '--size', '2048',
            '--format', 'VDI'
        ) `
        -Operation '2GiB合成コピー元VDI作成'
}
if (-not (Test-Path -LiteralPath $syntheticTargetDisk -PathType Leaf)) {
    if ($helperState -ne 'poweroff') {
        throw '保存状態の専用VMへ新規ディスクを追加しません。'
    }
    $null = Invoke-VBox `
        -Arguments @(
            'createmedium', 'disk',
            '--filename', $syntheticTargetDisk,
            '--size', '3072',
            '--format', 'VDI'
        ) `
        -Operation '3GiB合成コピー先VDI作成'
}

if ($helperState -eq 'poweroff') {
    $helperInfo = @(Invoke-VBox `
        -Arguments @('showvminfo', $helperVm, '--machinereadable') `
        -Operation '専用VMストレージ確認')
    $attachments = @(
        @{ Port = '0'; Path = $helperSystemDisk }
        @{ Port = '1'; Path = $syntheticSourceDisk }
        @{ Port = '2'; Path = $syntheticTargetDisk }
    )
    foreach ($attachment in $attachments) {
        $key = '"SATA-' + $attachment.Port + '-0"'
        $line = $helperInfo | Where-Object { $_ -like "$key=*" } |
            Select-Object -First 1
        $currentPath = $null
        if ($line -match '^"SATA-[0-9]+-0"="(.*)"$') {
            $currentPath = $Matches[1]
        }
        if ($currentPath -eq 'none' -or [string]::IsNullOrWhiteSpace($currentPath)) {
            $null = Invoke-VBox `
                -Arguments @(
                    'storageattach', $helperVm,
                    '--storagectl', 'SATA',
                    '--port', $attachment.Port,
                    '--device', '0',
                    '--type', 'hdd',
                    '--medium', $attachment.Path,
                    '--nonrotational', 'on'
                ) `
                -Operation "専用VDI接続 port $($attachment.Port)"
        }
        elseif (-not [IO.Path]::GetFullPath($currentPath).Equals(
                [IO.Path]::GetFullPath($attachment.Path),
                [StringComparison]::OrdinalIgnoreCase)) {
            throw "専用VMのSATA port $($attachment.Port)に想定外媒体があります。"
        }
    }
}

$helperInfo = @(Invoke-VBox `
    -Arguments @('showvminfo', $helperVm, '--machinereadable') `
    -Operation '専用VM最終設定確認')
if ((Get-MachineValue -Information $helperInfo -Name 'firmware') -ne 'EFI64' -and
    (Get-MachineValue -Information $helperInfo -Name 'VMState') -eq 'poweroff') {
    $null = Invoke-VBox `
        -Arguments @('modifyvm', $helperVm, '--firmware', 'efi64') `
        -Operation '専用VMのx64 UEFI固定'
    $helperInfo = @(Invoke-VBox `
        -Arguments @('showvminfo', $helperVm, '--machinereadable') `
        -Operation 'x64 UEFI固定後の再確認')
}
if ((Get-MachineValue -Information $helperInfo -Name 'nic1') -ne 'none' -or
    (Get-MachineValue -Information $helperInfo -Name 'firmware') -ne 'EFI64') {
    throw '専用VMのNICまたはUEFI設定が安全条件と一致しません。'
}

$runStamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$evidenceRoot = Join-Path $repoRoot ".validation\evidence\phase1-physical-vm\$runStamp"
$guestRoot = "C:\YtecDiskClonePhase1-$runStamp"
$guestManualLauncher = `
    'C:\Users\YbcTest\Desktop\YDC_Phase1_Test__DOUBLE_CLICK.cmd'
New-Item -ItemType Directory -Path $evidenceRoot -Force | Out-Null
$started = $false
try {
    if ($ReuseRunningHelper) {
        $started = $true
    }
    else {
        $vmStartType = if ($InteractiveUac) { 'gui' } else { 'headless' }
        $null = Invoke-VBox `
            -Arguments @('startvm', $helperVm, '--type', $vmStartType) `
            -Operation 'Phase 1専用VM起動'
        $started = $true
    }
    Wait-GuestReady -TargetVm $helperVm -FileSystemOnly:$InteractiveUac
    if (-not $InteractiveUac -and -not (Test-GuestAdministrator)) {
        throw 'VM専用物理クローン試験には、管理者トークンで実行できるGuestControlアカウントが必要です。UAC制限トークンのまま破壊的I/Oを試行しません。'
    }

    $null = Invoke-GuestRetry `
        -Operation 'ゲスト一時ディレクトリ作成' `
        -Arguments @(
            'mkdir', '--parents',
            "--username=$guestUser",
            "--passwordfile=$passwordFile",
            $guestRoot
        )
    $guestHarness = "$guestRoot\ytec-phase1-physical-clone-vm.exe"
    $guestInit = "$guestRoot\Initialize-Phase1SyntheticDisks.ps1"
    $guestValidate = "$guestRoot\Test-Phase1SyntheticClone.ps1"
    $guestClone = "$guestRoot\Invoke-Phase1PhysicalClone.ps1"
    $guestRunner = "$guestRoot\Run-Phase1PhysicalCloneElevated.ps1"
    $guestScriptStaging = Join-Path $evidenceRoot 'guest-scripts-utf8-bom'
    New-Item -ItemType Directory -Path $guestScriptStaging -Force | Out-Null
    $stagedInit = Join-Path $guestScriptStaging `
        ([IO.Path]::GetFileName($guestInitScript))
    $stagedValidate = Join-Path $guestScriptStaging `
        ([IO.Path]::GetFileName($guestValidationScript))
    $stagedClone = Join-Path $guestScriptStaging `
        ([IO.Path]::GetFileName($guestCloneScript))
    $stagedRunner = Join-Path $guestScriptStaging `
        ([IO.Path]::GetFileName($guestElevatedRunner))
    foreach ($staging in @(
        @{ Source = $guestInitScript; Destination = $stagedInit }
        @{ Source = $guestValidationScript; Destination = $stagedValidate }
        @{ Source = $guestCloneScript; Destination = $stagedClone }
        @{ Source = $guestElevatedRunner; Destination = $stagedRunner }
    )) {
        [IO.File]::WriteAllText(
            $staging.Destination,
            [IO.File]::ReadAllText($staging.Source),
            [Text.UTF8Encoding]::new($true))
    }
    foreach ($copy in @(
        @{ Host = $harness; Guest = $guestHarness }
        @{ Host = $stagedInit; Guest = $guestInit }
        @{ Host = $stagedValidate; Guest = $guestValidate }
        @{ Host = $stagedClone; Guest = $guestClone }
        @{ Host = $stagedRunner; Guest = $guestRunner }
    )) {
        $null = Invoke-GuestRetry `
            -Operation "copyto $($copy.Host)" `
            -Arguments @(
                'copyto',
                "--username=$guestUser",
                "--passwordfile=$passwordFile",
                $copy.Host,
                $copy.Guest
            )
    }

    if ($InteractiveUac) {
        $hostManualLauncher = Join-Path $evidenceRoot `
            'YDC_Phase1_Test__DOUBLE_CLICK.cmd'
        $manualLauncherContent = @"
@echo off
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe' -Verb RunAs -ArgumentList '-NoProfile -NonInteractive -ExecutionPolicy Bypass -File $guestRunner' -WindowStyle Hidden"
exit /b
"@
        [IO.File]::WriteAllText(
            $hostManualLauncher,
            $manualLauncherContent,
            [Text.ASCIIEncoding]::new())
        $null = Invoke-GuestRetry `
            -Operation '手動UAC起動ファイル配置' `
            -Arguments @(
                'copyto',
                "--username=$guestUser",
                "--passwordfile=$passwordFile",
                $hostManualLauncher,
                $guestManualLauncher
            )

        Write-Output 'MANUAL_UAC_REQUIRED'
        Write-Output "VM: $helperVm"
        Write-Output "Launcher: $guestManualLauncher"
        Write-Output "Evidence: $evidenceRoot"
        $interactiveDone = "$guestRoot\interactive.done.txt"
        $deadline = (Get-Date).AddMinutes(20)
        $doneFound = $false
        while ((Get-Date) -lt $deadline) {
            $null = & $vboxManage guestcontrol $helperVm stat `
                "--username=$guestUser" `
                "--passwordfile=$passwordFile" `
                $interactiveDone 2>&1
            if ($LASTEXITCODE -eq 0) {
                $doneFound = $true
                break
            }
            Start-Sleep -Seconds 5
        }
        if (-not $doneFound) {
            throw 'UAC承認後のVM試験が20分以内に完了しませんでした。'
        }

        foreach ($artifact in @(
            'interactive-summary.json',
            'interactive.done.txt',
            'initialize.json',
            'initialize.done.txt',
            'plan.txt',
            'plan.done.txt',
            'clone.txt',
            'clone.done.txt',
            'validation.json',
            'validation.done.txt'
        )) {
            $guestArtifact = "$guestRoot\$artifact"
            $null = & $vboxManage guestcontrol $helperVm stat `
                "--username=$guestUser" `
                "--passwordfile=$passwordFile" `
                $guestArtifact 2>&1
            if ($LASTEXITCODE -eq 0) {
                $null = Invoke-GuestRetry `
                    -Operation "対話試験証跡回収 $artifact" `
                    -Arguments @(
                        'copyfrom',
                        "--username=$guestUser",
                        "--passwordfile=$passwordFile",
                        $guestArtifact,
                        (Join-Path $evidenceRoot $artifact)
                    )
            }
        }

        $done = (Get-Content -LiteralPath `
            (Join-Path $evidenceRoot 'interactive.done.txt') -Raw).Trim()
        $summary = Get-Content -LiteralPath `
            (Join-Path $evidenceRoot 'interactive-summary.json') -Raw |
            ConvertFrom-Json
        if ($done -ne 'PASS' -or $summary.result -ne 'PASS') {
            throw "UAC昇格VM試験が失敗しました: $($summary.message)"
        }

        [ordered]@{
            schemaVersion = 1
            result = 'PASS'
            vmName = $helperVm
            firmware = 'EFI64'
            nic = 'none'
            interactiveUac = $true
            sourceDiskBytes = [UInt64]$summary.sourceDiskBytes
            targetDiskBytes = [UInt64]$summary.targetDiskBytes
            sourcePartitionCount = [int]$summary.sourcePartitionCount
            diskGuidRegenerated = [bool]$summary.diskGuidRegenerated
            partitionGuidsRegenerated = [bool]$summary.partitionGuidsRegenerated
            partitionCount = [int]$summary.partitionCount
            completedUtc = [DateTimeOffset]::UtcNow
        } | ConvertTo-Json -Depth 4 | Set-Content `
            -LiteralPath (Join-Path $evidenceRoot 'summary.json') `
            -Encoding utf8

        Write-Output 'Phase 1 physical VM interactive test: PASS'
        Write-Output "Evidence: $evidenceRoot"
        return
    }

    $initResult = "$guestRoot\initialize.json"
    $initDone = "$guestRoot\initialize.done.txt"
    $initOutput = @(Invoke-GuestRecordedPowerShell `
        -ScriptPath $guestInit `
        -Parameters @(
            '-ResultPath', $initResult,
            '-DonePath', $initDone
        ) `
        -GuestResultPath $initResult `
        -GuestDonePath $initDone `
        -HostResultPath (Join-Path $evidenceRoot 'initialize.json') `
        -Operation '合成ディスク初期化')
    $ready = Get-JsonResult -Output $initOutput -Operation '合成ディスク初期化'
    if ($ready.result -ne 'READY') {
        throw '合成ディスク初期化結果がREADYではありません。'
    }

    $sourceNumber = [int]$ready.sourceDiskNumber
    $targetNumber = [int]$ready.targetDiskNumber
    $planResult = "$guestRoot\plan.txt"
    $planDone = "$guestRoot\plan.done.txt"
    $planOutput = @(Invoke-GuestRecordedPowerShell `
        -ScriptPath $guestClone `
        -Parameters @(
            '-Mode', 'Plan',
            '-HarnessPath', $guestHarness,
            '-SourceDiskNumber', $sourceNumber,
            '-TargetDiskNumber', $targetNumber,
            '-ResultPath', $planResult,
            '-DonePath', $planDone
        ) `
        -GuestResultPath $planResult `
        -GuestDonePath $planDone `
        -HostResultPath (Join-Path $evidenceRoot 'plan.txt') `
        -Operation 'VM専用クローン計画')
    $confirmationLine = $planOutput |
        Where-Object { $_ -like 'confirmation=*' } |
        Select-Object -Last 1
    if ($confirmationLine -notmatch '^confirmation=(.+)$') {
        throw 'VM専用クローン計画から確認文字列を取得できません。'
    }
    $confirmation = $Matches[1]

    $cloneResult = "$guestRoot\clone.txt"
    $cloneDone = "$guestRoot\clone.done.txt"
    $cloneOutput = @(Invoke-GuestRecordedPowerShell `
        -ScriptPath $guestClone `
        -Parameters @(
            '-Mode', 'Execute',
            '-HarnessPath', $guestHarness,
            '-SourceDiskNumber', $sourceNumber,
            '-TargetDiskNumber', $targetNumber,
            '-Confirmation', $confirmation,
            '-ResultPath', $cloneResult,
            '-DonePath', $cloneDone
        ) `
        -GuestResultPath $cloneResult `
        -GuestDonePath $cloneDone `
        -HostResultPath (Join-Path $evidenceRoot 'clone.txt') `
        -Operation 'VM専用物理クローン')
    if (($cloneOutput -join "`n") -notmatch 'YDC_VM_CLONE_PASS') {
        throw 'VM専用物理クローン結果にPASSマーカーがありません。'
    }

    $validationResult = "$guestRoot\validation.json"
    $validationDone = "$guestRoot\validation.done.txt"
    $validationOutput = @(Invoke-GuestRecordedPowerShell `
        -ScriptPath $guestValidate `
        -Parameters @(
            '-SourceDiskNumber', $sourceNumber,
            '-TargetDiskNumber', $targetNumber,
            '-ResultPath', $validationResult,
            '-DonePath', $validationDone
        ) `
        -GuestResultPath $validationResult `
        -GuestDonePath $validationDone `
        -HostResultPath (Join-Path $evidenceRoot 'validation.json') `
        -Operation 'VM内コピー結果検証')
    $validation = Get-JsonResult -Output $validationOutput -Operation 'VM内コピー結果検証'
    if ($validation.result -ne 'PASS') {
        throw 'VM内コピー結果がPASSではありません。'
    }

    [ordered]@{
        schemaVersion = 1
        result = 'PASS'
        vmName = $helperVm
        firmware = 'EFI64'
        nic = 'none'
        sourceDiskBytes = [UInt64]$ready.sourceBytes
        targetDiskBytes = [UInt64]$ready.targetBytes
        sourcePartitionCount = [int]$ready.sourcePartitionCount
        diskGuidRegenerated = [bool]$validation.diskGuidRegenerated
        partitionGuidsRegenerated = [bool]$validation.partitionGuidsRegenerated
        partitionCount = [int]$validation.partitionCount
        completedUtc = [DateTimeOffset]::UtcNow
    } | ConvertTo-Json -Depth 4 | Set-Content `
        -LiteralPath (Join-Path $evidenceRoot 'summary.json') `
        -Encoding utf8

    Write-Output "Phase 1 physical VM test: PASS"
    Write-Output "Evidence: $evidenceRoot"
}
finally {
    if ($started) {
        if ($InteractiveUac) {
            $null = & $vboxManage guestcontrol $helperVm rm `
                "--username=$guestUser" `
                "--passwordfile=$passwordFile" `
                $guestManualLauncher 2>&1
        }
        $null = & $vboxManage guestcontrol $helperVm rm `
            '--recursive' `
            "--username=$guestUser" `
            "--passwordfile=$passwordFile" `
            $guestRoot 2>&1
        $null = & $vboxManage guestcontrol $helperVm closesession --all 2>&1
        if ($ReuseRunningHelper) {
            $null = & $vboxManage controlvm $helperVm savestate 2>&1
            if ($LASTEXITCODE -ne 0) {
                Write-Warning 'Phase 1専用VMを保存状態へ戻せませんでした。'
            }
        }
        else {
            $null = & $vboxManage guestcontrol $helperVm run `
                '--exe=C:\Windows\System32\shutdown.exe' `
                '--arg0=shutdown.exe' `
                "--username=$guestUser" `
                "--passwordfile=$passwordFile" `
                --quiet `
                -- `
                /s `
                /t `
                0 2>&1
            $shutdownDeadline = (Get-Date).AddMinutes(3)
            $poweredOff = $false
            while ((Get-Date) -lt $shutdownDeadline) {
                $stateLine = @(& $vboxManage showvminfo $helperVm --machinereadable) |
                    Where-Object { $_ -like 'VMState=*' } |
                    Select-Object -First 1
                if ($stateLine -eq 'VMState="poweroff"') {
                    $poweredOff = $true
                    break
                }
                Start-Sleep -Seconds 3
            }
            if (-not $poweredOff) {
                $null = & $vboxManage controlvm $helperVm savestate 2>&1
                if ($LASTEXITCODE -ne 0) {
                    Write-Warning 'Phase 1専用VMを停止または保存状態へ戻せませんでした。'
                }
            }
        }
    }
}
