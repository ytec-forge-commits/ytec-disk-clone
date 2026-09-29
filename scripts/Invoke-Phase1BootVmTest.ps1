[CmdletBinding()]
param(
    [switch] $ResumeAfterClone
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
$workerSourceVm = 'YWB-Win11-25H2-x64-Clean'
$bootSourceVm = 'YWB-Win10-22H2-x64-Clean'
$previousWorkerVm = 'YDC-Phase1-Boot-Worker'
$workerVm = 'YDC-Phase1-Boot-Worker-Win11'
$bootCheckVm = 'YDC-Phase1-Boot-Check-Win10'
$guestUser = 'YbcTest'
$workerPasswordFile = Join-Path $credentialRoot "$workerSourceVm.password.txt"
$bootPasswordFile = Join-Path $credentialRoot "$bootSourceVm.password.txt"
$previousWorkerFolder = Join-Path $labVmRoot $previousWorkerVm
$workerFolder = Join-Path $labVmRoot $workerVm
$workerSystemDisk = Join-Path $previousWorkerFolder 'YDC-Phase1-Boot-Source-Win11-128GiB.vdi'
$bootSourceDisk = Join-Path $previousWorkerFolder 'YDC-Phase1-Boot-Worker-System-96GiB.vdi'
$bootTargetDisk = Join-Path $workerFolder 'YDC-Phase1-Boot-Target-110GiB.vdi'
$cloneHarness = Join-Path $repoRoot `
    'out\build\msvc-x64-vm-destructive\tests\ytec-phase1-physical-clone-vm.exe'
$bcdBootHarness = Join-Path $repoRoot `
    'out\build\msvc-x64-vm-destructive\tests\ytec-phase1-bcdboot-vm.exe'
$guestCloneScript = Join-Path $repoRoot `
    'scripts\vm\Invoke-Phase1PhysicalClone.ps1'
$guestBcdBootScript = Join-Path $repoRoot `
    'scripts\vm\Invoke-Phase1BcdBoot.ps1'
$guestElevatedRunner = if ($ResumeAfterClone) {
    Join-Path $repoRoot 'scripts\vm\Run-Phase1BootFinalizeElevated.ps1'
}
else {
    Join-Path $repoRoot 'scripts\vm\Run-Phase1BootCloneElevated.ps1'
}

foreach ($requiredFile in @(
    $vboxManage,
    $workerPasswordFile,
    $bootPasswordFile,
    $cloneHarness,
    $bcdBootHarness,
    $guestCloneScript,
    $guestBcdBootScript,
    $guestElevatedRunner
)) {
    if (-not (Test-Path -LiteralPath $requiredFile -PathType Leaf)) {
        throw "必要ファイルがありません: $requiredFile"
    }
}

function Invoke-VBox {
    param(
        [Parameter(Mandatory)][string[]] $Arguments,
        [Parameter(Mandatory)][string] $Operation
    )
    $output = & $vboxManage @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "VirtualBox操作が失敗しました: $Operation`n$($output -join "`n")"
    }
    return @($output)
}

function Test-VmRegistered {
    param([Parameter(Mandatory)][string] $Name)
    return @(& $vboxManage list vms) -match ('^"' + [regex]::Escape($Name) + '" ')
}

function Get-MachineValue {
    param(
        [Parameter(Mandatory)][string[]] $Information,
        [Parameter(Mandatory)][string] $Name
    )
    $line = $Information |
        Where-Object { $_ -like "$Name=*" } |
        Select-Object -First 1
    if ($line -match '^[^=]+="(.*)"$') {
        return $Matches[1]
    }
    return $null
}

function Get-SataMedium {
    param(
        [Parameter(Mandatory)][string[]] $Information,
        [Parameter(Mandatory)][int] $Port
    )
    $key = '"SATA-' + $Port + '-0"'
    $line = $Information | Where-Object { $_ -like "$key=*" } |
        Select-Object -First 1
    if ($line -match '^"SATA-[0-9]+-0"="(.*)"$') {
        return $Matches[1]
    }
    return $null
}

function Assert-LabMediumPath {
    param([Parameter(Mandatory)][string] $Path)
    $canonicalPath = [IO.Path]::GetFullPath($Path)
    $canonicalLabRoot = [IO.Path]::GetFullPath($labVmRoot) + `
        [IO.Path]::DirectorySeparatorChar
    if (-not $canonicalPath.StartsWith(
            $canonicalLabRoot,
            [StringComparison]::OrdinalIgnoreCase) -or
        -not (Test-Path -LiteralPath $canonicalPath -PathType Leaf)) {
        throw "共有VMラボ内の正規媒体ではありません: $canonicalPath"
    }
    return $canonicalPath
}

function Get-SourceSystemMedium {
    param(
        [Parameter(Mandatory)][string] $VmName,
        [Parameter(Mandatory)][int] $ExpectedCapacityMb
    )
    $information = @(Invoke-VBox `
        -Arguments @('showvminfo', $VmName, '--machinereadable') `
        -Operation "$VmName 情報取得")
    if ((Get-MachineValue -Information $information -Name 'VMState') -ne 'saved' -or
        (Get-MachineValue -Information $information -Name 'nic1') -ne 'none') {
        throw "参照元VMはsavedかつNIC無効でなければなりません: $VmName"
    }
    $path = Assert-LabMediumPath `
        -Path (Get-SataMedium -Information $information -Port 0)
    Assert-MediumCapacity -Path $path -ExpectedCapacityMb $ExpectedCapacityMb
    return $path
}

function Assert-MediumCapacity {
    param(
        [Parameter(Mandatory)][string] $Path,
        [Parameter(Mandatory)][int] $ExpectedCapacityMb
    )
    $information = @(Invoke-VBox `
        -Arguments @('showmediuminfo', 'disk', $Path) `
        -Operation "仮想ディスク容量確認 $Path")
    $capacityLine = $information | Where-Object { $_ -match '^Capacity:' } |
        Select-Object -First 1
    if ($capacityLine -notmatch '^Capacity:\s+([0-9]+) MBytes$' -or
        [int]$Matches[1] -ne $ExpectedCapacityMb) {
        throw "仮想ディスクの論理容量が固定値と一致しません: $Path"
    }
}

function Wait-GuestFileSystem {
    param(
        [Parameter(Mandatory)][string] $VmName,
        [Parameter(Mandatory)][string] $PasswordFile,
        [Parameter(Mandatory)][string] $Path,
        [int] $TimeoutMinutes = 15
    )
    $deadline = (Get-Date).AddMinutes($TimeoutMinutes)
    while ((Get-Date) -lt $deadline) {
        $null = & $vboxManage guestcontrol $VmName stat `
            "--username=$guestUser" `
            "--passwordfile=$PasswordFile" `
            $Path 2>&1
        if ($LASTEXITCODE -eq 0) {
            return
        }
        $null = & $vboxManage guestcontrol $VmName closesession --all 2>&1
        Start-Sleep -Seconds 10
    }
    throw "VM内ファイルシステムの準備を確認できませんでした: $VmName"
}

function Invoke-GuestRetry {
    param(
        [Parameter(Mandatory)][string] $VmName,
        [Parameter(Mandatory)][string] $PasswordFile,
        [Parameter(Mandatory)][string[]] $Arguments,
        [Parameter(Mandatory)][string] $Operation
    )
    $lastOutput = @()
    foreach ($attempt in 1..8) {
        $lastOutput = @(& $vboxManage guestcontrol $VmName @Arguments 2>&1)
        if ($LASTEXITCODE -eq 0) {
            return $lastOutput
        }
        $null = & $vboxManage guestcontrol $VmName closesession --all 2>&1
        Start-Sleep -Seconds 3
    }
    throw "GuestControlが失敗しました: $Operation`n$($lastOutput -join "`n")"
}

function Stop-DedicatedVm {
    param([Parameter(Mandatory)][string] $VmName)
    $state = Get-MachineValue `
        -Information @(Invoke-VBox `
            -Arguments @('showvminfo', $VmName, '--machinereadable') `
            -Operation "$VmName 停止前状態取得") `
        -Name 'VMState'
    if ($state -ne 'running') {
        return
    }
    $null = & $vboxManage controlvm $VmName acpipowerbutton 2>&1
    $deadline = (Get-Date).AddMinutes(5)
    while ((Get-Date) -lt $deadline) {
        $information = @(& $vboxManage showvminfo $VmName --machinereadable)
        if ((Get-MachineValue -Information $information -Name 'VMState') -eq 'poweroff') {
            return
        }
        Start-Sleep -Seconds 5
    }
    $null = & $vboxManage controlvm $VmName poweroff 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "専用VMを停止できませんでした: $VmName"
    }
}

function Ensure-Vm {
    param(
        [Parameter(Mandatory)][string] $Name,
        [Parameter(Mandatory)][string] $OsType,
        [Parameter(Mandatory)][int] $PortCount,
        [Parameter(Mandatory)][string] $Scope
    )
    if (-not (Test-VmRegistered -Name $Name)) {
        $null = Invoke-VBox `
            -Arguments @(
                'createvm', '--name', $Name,
                '--basefolder', $labVmRoot,
                '--ostype', $OsType,
                '--register'
            ) `
            -Operation "$Name 作成"
        $null = Invoke-VBox `
            -Arguments @(
                'modifyvm', $Name,
                '--memory', '4096',
                '--cpus', '2',
                '--firmware', 'efi64',
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
            -Operation "$Name 安全設定"
        $null = Invoke-VBox `
            -Arguments @('setextradata', $Name, 'YTEC/ValidationScope', $Scope) `
            -Operation "$Name 識別設定"
        $null = Invoke-VBox `
            -Arguments @(
                'storagectl', $Name,
                '--name', 'SATA',
                '--add', 'sata',
                '--controller', 'IntelAhci',
                '--portcount', $PortCount.ToString(),
                '--bootable', 'on'
            ) `
            -Operation "$Name SATA作成"
    }
    $information = @(Invoke-VBox `
        -Arguments @('showvminfo', $Name, '--machinereadable') `
        -Operation "$Name 再確認")
    $state = Get-MachineValue -Information $information -Name 'VMState'
    $scopeValue = @(& $vboxManage getextradata $Name 'YTEC/ValidationScope')
    if ($state -ne 'poweroff' -or
        (Get-MachineValue -Information $information -Name 'firmware') -ne 'EFI64' -or
        (Get-MachineValue -Information $information -Name 'nic1') -ne 'none' -or
        ($scopeValue -join "`n") -notmatch [regex]::Escape($Scope)) {
        throw "$Name の状態、UEFI、NIC、または専用識別が一致しません。"
    }
}

function Ensure-Attachment {
    param(
        [Parameter(Mandatory)][string] $VmName,
        [Parameter(Mandatory)][int] $Port,
        [Parameter(Mandatory)][string] $MediumPath
    )
    $information = @(Invoke-VBox `
        -Arguments @('showvminfo', $VmName, '--machinereadable') `
        -Operation "$VmName ストレージ確認")
    $current = Get-SataMedium -Information $information -Port $Port
    if ([string]::IsNullOrWhiteSpace($current) -or $current -eq 'none') {
        $null = Invoke-VBox `
            -Arguments @(
                'storageattach', $VmName,
                '--storagectl', 'SATA',
                '--port', $Port.ToString(),
                '--device', '0',
                '--type', 'hdd',
                '--medium', $MediumPath,
                '--nonrotational', 'on'
            ) `
            -Operation "$VmName SATA port $Port 接続"
    }
    elseif (-not [IO.Path]::GetFullPath($current).Equals(
            [IO.Path]::GetFullPath($MediumPath),
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "$VmName SATA port $Port に想定外の媒体があります。"
    }
}

function Remove-ExpectedAttachment {
    param(
        [Parameter(Mandatory)][string] $VmName,
        [Parameter(Mandatory)][int] $Port,
        [Parameter(Mandatory)][string] $ExpectedMediumPath
    )
    $information = @(Invoke-VBox `
        -Arguments @('showvminfo', $VmName, '--machinereadable') `
        -Operation "$VmName 切離し前確認")
    if ((Get-MachineValue -Information $information -Name 'VMState') -ne 'poweroff') {
        throw "$VmName が電源OFFではないため媒体を切り離しません。"
    }
    $current = Get-SataMedium -Information $information -Port $Port
    if ([string]::IsNullOrWhiteSpace($current) -or $current -eq 'none') {
        return
    }
    if (-not [IO.Path]::GetFullPath($current).Equals(
            [IO.Path]::GetFullPath($ExpectedMediumPath),
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "$VmName SATA port $Port に想定外の媒体があるため切り離しません。"
    }
    $null = Invoke-VBox `
        -Arguments @(
            'storageattach', $VmName,
            '--storagectl', 'SATA',
            '--port', $Port.ToString(),
            '--device', '0',
            '--type', 'hdd',
            '--medium', 'none'
        ) `
        -Operation "$VmName SATA port $Port 専用媒体切離し"
}

$runningVms = @(& $vboxManage list runningvms)
if ($runningVms.Count -ne 0) {
    throw '別のVMが稼働中です。VMラボの直列利用を守るため開始しません。'
}
$freeBytes = (Get-PSDrive -Name ([IO.Path]::GetPathRoot($labVmRoot).Substring(0, 1))).Free
if ($freeBytes -lt 64GB) {
    throw 'VM専用ディスク作成に必要な64GiB以上の空き容量がありません。'
}

$workerSourceMedium = Get-SourceSystemMedium `
    -VmName $workerSourceVm `
    -ExpectedCapacityMb 131072
$bootSourceMedium = Get-SourceSystemMedium `
    -VmName $bootSourceVm `
    -ExpectedCapacityMb 98304

if (Test-VmRegistered -Name $previousWorkerVm) {
    Remove-ExpectedAttachment `
        -VmName $previousWorkerVm `
        -Port 0 `
        -ExpectedMediumPath $bootSourceDisk
    Remove-ExpectedAttachment `
        -VmName $previousWorkerVm `
        -Port 1 `
        -ExpectedMediumPath $workerSystemDisk
}

Ensure-Vm `
    -Name $workerVm `
    -OsType 'Windows11_64' `
    -PortCount 4 `
    -Scope 'ytec-disk-clone-phase1-boot-worker'

if (-not (Test-Path -LiteralPath $workerSystemDisk -PathType Leaf)) {
    $null = Invoke-VBox `
        -Arguments @(
            'clonemedium', 'disk',
            $workerSourceMedium,
            $workerSystemDisk,
            '--format', 'VDI'
        ) `
        -Operation 'Win10 WorkerシステムVDIのフルクローン'
}
Assert-MediumCapacity -Path $workerSystemDisk -ExpectedCapacityMb 131072

if (-not (Test-Path -LiteralPath $bootSourceDisk -PathType Leaf)) {
    $null = Invoke-VBox `
        -Arguments @(
            'clonemedium', 'disk',
            $bootSourceMedium,
            $bootSourceDisk,
            '--format', 'VDI'
        ) `
        -Operation 'Win11ブートコピー元VDIのフルクローン'
}
Assert-MediumCapacity -Path $bootSourceDisk -ExpectedCapacityMb 98304

if (-not (Test-Path -LiteralPath $bootTargetDisk -PathType Leaf)) {
    $null = Invoke-VBox `
        -Arguments @(
            'createmedium', 'disk',
            '--filename', $bootTargetDisk,
            '--size', '112640',
            '--format', 'VDI'
        ) `
        -Operation '110GiBブートコピー先VDI作成'
}
Assert-MediumCapacity -Path $bootTargetDisk -ExpectedCapacityMb 112640

Ensure-Attachment -VmName $workerVm -Port 0 -MediumPath $workerSystemDisk
Ensure-Attachment -VmName $workerVm -Port 1 -MediumPath $bootSourceDisk
Ensure-Attachment -VmName $workerVm -Port 2 -MediumPath $bootTargetDisk

$runStamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$evidenceRoot = Join-Path $repoRoot ".validation\evidence\phase1-boot-vm\$runStamp"
$guestRoot = "C:\YtecDiskCloneBoot-$runStamp"
$guestLauncher = 'C:\Users\YbcTest\Desktop\YDC_Boot_Clone__DOUBLE_CLICK.cmd'
New-Item -ItemType Directory -Path $evidenceRoot -Force | Out-Null
$workerStarted = $false
$bootCheckStarted = $false

try {
    $null = Invoke-VBox `
        -Arguments @('startvm', $workerVm, '--type', 'gui') `
        -Operation 'ブートクローンWorker VM起動'
    $workerStarted = $true
    Wait-GuestFileSystem `
        -VmName $workerVm `
        -PasswordFile $workerPasswordFile `
        -Path 'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe'

    $null = Invoke-GuestRetry `
        -VmName $workerVm `
        -PasswordFile $workerPasswordFile `
        -Operation 'ゲスト一時ディレクトリ作成' `
        -Arguments @(
            'mkdir', '--parents',
            "--username=$guestUser",
            "--passwordfile=$workerPasswordFile",
            $guestRoot
        )

    $guestFiles = @(
        @{ Host = $cloneHarness; Guest = "$guestRoot\ytec-phase1-physical-clone-vm.exe"; Bom = $false }
        @{ Host = $bcdBootHarness; Guest = "$guestRoot\ytec-phase1-bcdboot-vm.exe"; Bom = $false }
        @{ Host = $guestCloneScript; Guest = "$guestRoot\Invoke-Phase1PhysicalClone.ps1"; Bom = $true }
        @{ Host = $guestBcdBootScript; Guest = "$guestRoot\Invoke-Phase1BcdBoot.ps1"; Bom = $true }
        @{ Host = $guestElevatedRunner; Guest = "$guestRoot\Run-Phase1BootCloneElevated.ps1"; Bom = $true }
    )
    $stagingRoot = Join-Path $evidenceRoot 'guest-scripts-utf8-bom'
    New-Item -ItemType Directory -Path $stagingRoot -Force | Out-Null
    foreach ($copy in $guestFiles) {
        $copySource = $copy.Host
        if ($copy.Bom) {
            $copySource = Join-Path $stagingRoot ([IO.Path]::GetFileName($copy.Host))
            [IO.File]::WriteAllText(
                $copySource,
                [IO.File]::ReadAllText($copy.Host),
                [Text.UTF8Encoding]::new($true))
        }
        $null = Invoke-GuestRetry `
            -VmName $workerVm `
            -PasswordFile $workerPasswordFile `
            -Operation "ゲスト配置 $($copy.Guest)" `
            -Arguments @(
                'copyto',
                "--username=$guestUser",
                "--passwordfile=$workerPasswordFile",
                $copySource,
                $copy.Guest
            )
    }

    $hostLauncher = Join-Path $evidenceRoot 'YDC_Boot_Clone__DOUBLE_CLICK.cmd'
    $guestRunner = "$guestRoot\Run-Phase1BootCloneElevated.ps1"
    $launcherContent = @"
@echo off
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe' -Verb RunAs -ArgumentList '-NoProfile -NonInteractive -ExecutionPolicy Bypass -File $guestRunner' -WindowStyle Hidden"
exit /b
"@
    [IO.File]::WriteAllText(
        $hostLauncher,
        $launcherContent,
        [Text.ASCIIEncoding]::new())
    $null = Invoke-GuestRetry `
        -VmName $workerVm `
        -PasswordFile $workerPasswordFile `
        -Operation '手動UACランチャー配置' `
        -Arguments @(
            'copyto',
            "--username=$guestUser",
            "--passwordfile=$workerPasswordFile",
            $hostLauncher,
            $guestLauncher
        )

    Write-Output 'MANUAL_UAC_REQUIRED'
    Write-Output "VM: $workerVm"
    Write-Output "Launcher: $guestLauncher"
    Write-Output "Evidence: $evidenceRoot"

    $guestDone = "$guestRoot\boot-clone.done.txt"
    $deadline = (Get-Date).AddMinutes(90)
    $doneFound = $false
    while ((Get-Date) -lt $deadline) {
        $null = & $vboxManage guestcontrol $workerVm stat `
            "--username=$guestUser" `
            "--passwordfile=$workerPasswordFile" `
            $guestDone 2>&1
        if ($LASTEXITCODE -eq 0) {
            $doneFound = $true
            break
        }
        Start-Sleep -Seconds 10
    }
    if (-not $doneFound) {
        throw '手動UAC承認後のブートクローン試験が90分以内に完了しませんでした。'
    }

    foreach ($artifact in @(
        'boot-clone-summary.json',
        'boot-clone.done.txt',
        'boot-clone-plan.txt',
        'boot-clone-plan.done.txt',
        'boot-clone.txt',
        'boot-clone-step.done.txt',
        'bcdboot-plan.txt',
        'bcdboot-plan.done.txt',
        'bcdboot.txt',
        'bcdboot.done.txt'
    )) {
        $guestArtifact = "$guestRoot\$artifact"
        $null = & $vboxManage guestcontrol $workerVm stat `
            "--username=$guestUser" `
            "--passwordfile=$workerPasswordFile" `
            $guestArtifact 2>&1
        if ($LASTEXITCODE -eq 0) {
            $null = Invoke-GuestRetry `
                -VmName $workerVm `
                -PasswordFile $workerPasswordFile `
                -Operation "証跡回収 $artifact" `
                -Arguments @(
                    'copyfrom',
                    "--username=$guestUser",
                    "--passwordfile=$workerPasswordFile",
                    $guestArtifact,
                    (Join-Path $evidenceRoot $artifact)
                )
        }
    }
    $guestSummary = Get-Content `
        -LiteralPath (Join-Path $evidenceRoot 'boot-clone-summary.json') `
        -Raw | ConvertFrom-Json
    $guestDoneValue = (Get-Content `
        -LiteralPath (Join-Path $evidenceRoot 'boot-clone.done.txt') `
        -Raw).Trim()
    if ($guestDoneValue -ne 'PASS' -or $guestSummary.result -ne 'PASS') {
        throw "ゲスト内ブートクローン試験が失敗しました: $($guestSummary.message)"
    }

    $null = & $vboxManage guestcontrol $workerVm rm `
        "--username=$guestUser" `
        "--passwordfile=$workerPasswordFile" `
        $guestLauncher 2>&1
    $null = & $vboxManage guestcontrol $workerVm closesession --all 2>&1
    Stop-DedicatedVm -VmName $workerVm
    $workerStarted = $false

    $workerInfo = @(Invoke-VBox `
        -Arguments @('showvminfo', $workerVm, '--machinereadable') `
        -Operation 'Worker停止後ストレージ確認')
    $attachedTarget = Get-SataMedium -Information $workerInfo -Port 2
    if ([string]::IsNullOrWhiteSpace($attachedTarget) -or
        -not [IO.Path]::GetFullPath($attachedTarget).Equals(
            [IO.Path]::GetFullPath($bootTargetDisk),
            [StringComparison]::OrdinalIgnoreCase)) {
        throw 'WorkerのSATA port 2が専用コピー先と一致しません。'
    }
    $null = Invoke-VBox `
        -Arguments @(
            'storageattach', $workerVm,
            '--storagectl', 'SATA',
            '--port', '2',
            '--device', '0',
            '--type', 'hdd',
            '--medium', 'none'
        ) `
        -Operation 'Workerから専用コピー先を切離し'

    Ensure-Vm `
        -Name $bootCheckVm `
        -OsType 'Windows10_64' `
        -PortCount 1 `
        -Scope 'ytec-disk-clone-phase1-boot-check'
    Ensure-Attachment -VmName $bootCheckVm -Port 0 -MediumPath $bootTargetDisk
    $bootInfo = @(Invoke-VBox `
        -Arguments @('showvminfo', $bootCheckVm, '--machinereadable') `
        -Operation 'Boot-Check最終設定確認')
    if ((Get-SataMedium -Information $bootInfo -Port 0) -eq $bootSourceDisk -or
        -not [string]::IsNullOrWhiteSpace(
            (Get-SataMedium -Information $bootInfo -Port 1))) {
        throw 'Boot-Check VMにコピー元または余分な媒体があります。'
    }

    $null = Invoke-VBox `
        -Arguments @('startvm', $bootCheckVm, '--type', 'headless') `
        -Operation 'コピー元不在Boot-Check VM起動'
    $bootCheckStarted = $true
    Wait-GuestFileSystem `
        -VmName $bootCheckVm `
        -PasswordFile $bootPasswordFile `
        -Path 'C:\Windows\System32\winload.efi' `
        -TimeoutMinutes 20

    $screenshotPath = Join-Path $evidenceRoot 'boot-check.png'
    $null = Invoke-VBox `
        -Arguments @('controlvm', $bootCheckVm, 'screenshotpng', $screenshotPath) `
        -Operation 'Boot-Check画面証跡取得'
    $guestProperties = @(Invoke-VBox `
        -Arguments @('guestproperty', 'enumerate', $bootCheckVm) `
        -Operation 'Boot-Check Guest Additions証跡取得')
    $guestProperties | Set-Content `
        -LiteralPath (Join-Path $evidenceRoot 'boot-check-guest-properties.txt') `
        -Encoding utf8
    $workerInfo | Set-Content `
        -LiteralPath (Join-Path $evidenceRoot 'worker-vm-info.txt') `
        -Encoding utf8
    $bootInfo | Set-Content `
        -LiteralPath (Join-Path $evidenceRoot 'boot-check-vm-info-before-start.txt') `
        -Encoding utf8
    @(Invoke-VBox `
        -Arguments @('showmediuminfo', 'disk', $bootTargetDisk) `
        -Operation 'コピー先媒体最終情報取得') | Set-Content `
        -LiteralPath (Join-Path $evidenceRoot 'target-medium-info.txt') `
        -Encoding utf8

    $summary = [ordered]@{
        schemaVersion = 1
        result = 'PASS'
        workerVm = $workerVm
        bootCheckVm = $bootCheckVm
        firmware = 'EFI64'
        nic = 'none'
        sourceDiskAbsentAtBoot = $true
        targetDiskOnlyAtBoot = $true
        guestAdditionsFileSystemReady = $true
        secureBootTested = $false
        guestClone = $guestSummary
        completedUtc = [DateTimeOffset]::UtcNow
    }
    $summary | ConvertTo-Json -Depth 7 | Set-Content `
        -LiteralPath (Join-Path $evidenceRoot 'summary.json') `
        -Encoding utf8
    Get-ChildItem -LiteralPath $evidenceRoot -File | ForEach-Object {
        [pscustomobject]@{
            file = $_.Name
            sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    } | ConvertTo-Json | Set-Content `
        -LiteralPath (Join-Path $evidenceRoot 'evidence-sha256.json') `
        -Encoding utf8

    Write-Output 'Phase 1 bootable UEFI clone VM test: PASS'
    Write-Output "Evidence: $evidenceRoot"
}
finally {
    if ($bootCheckStarted) {
        $null = & $vboxManage guestcontrol $bootCheckVm closesession --all 2>&1
        try {
            Stop-DedicatedVm -VmName $bootCheckVm
        }
        catch {
            Write-Warning $_.Exception.Message
        }
    }
    if ($workerStarted) {
        $null = & $vboxManage guestcontrol $workerVm closesession --all 2>&1
        try {
            Stop-DedicatedVm -VmName $workerVm
        }
        catch {
            Write-Warning $_.Exception.Message
        }
    }
}
