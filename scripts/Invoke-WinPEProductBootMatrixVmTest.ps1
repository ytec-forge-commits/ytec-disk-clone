[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $Iso2011Ca,

    [Parameter(Mandatory)]
    [string] $Iso2023Ca,

    [ValidateRange(30, 120)]
    [int] $BootWaitSeconds = 65,

    [ValidatePattern('^YDC-WinPE-Boot-Matrix$')]
    [string] $VmName = 'YDC-WinPE-Boot-Matrix'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$PSNativeCommandUseErrorActionPreference = $false

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$workspaceRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot '..\..'))
$labRoot = Join-Path $workspaceRoot 'business-apps\ytec-windows-backup'
$labVmRoot = Join-Path $labRoot '.validation\vms'
$vboxManage = 'C:\Program Files\Oracle\VirtualBox\VBoxManage.exe'
$currentWinpeApp = Join-Path $repoRoot `
    'out\build\msvc-x64-vm\src\WinPEApp\ytec-winpe-app.exe'
$currentWinpeGui = Join-Path $repoRoot `
    'out\build\msvc-x64-vm\src\WinPEApp\ytec-winpe-gui.exe'
$timestamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$evidenceRoot = Join-Path $repoRoot `
    ".validation\evidence\winpe-product-boot-matrix\$timestamp"
$summaryPath = Join-Path $evidenceRoot 'result.json'
$vmBeforePath = Join-Path $evidenceRoot 'vm-before.txt'
$vmAfterPath = Join-Path $evidenceRoot 'vm-after.txt'

function Invoke-VBox {
    param(
        [Parameter(Mandatory)][string[]] $Arguments,
        [Parameter(Mandatory)][string] $Operation
    )

    $previousPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $output = @(& $vboxManage @Arguments 2>&1)
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousPreference
    }
    if ($exitCode -ne 0) {
        throw "VirtualBox操作が失敗しました: $Operation`n$($output -join "`n")"
    }
    return $output
}

function Get-VmInformation {
    return @(Invoke-VBox `
        -Arguments @('showvminfo', $VmName, '--machinereadable') `
        -Operation "$VmName 情報取得")
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

function Get-AttachmentValue {
    param(
        [Parameter(Mandatory)][string[]] $Information,
        [Parameter(Mandatory)][string] $Controller,
        [Parameter(Mandatory)][int] $Port,
        [int] $Device = 0
    )

    $key = '"' + $Controller + '-' + $Port + '-' + $Device + '"'
    $line = $Information |
        Where-Object { $_ -like "$key=*" } |
        Select-Object -First 1
    if ($line -match '^"[^\"]+"="(.*)"$') {
        return $Matches[1].Replace('\\', '\')
    }
    return $null
}

function Assert-NetworkDisabled {
    param([Parameter(Mandatory)][string[]] $Information)

    foreach ($index in 1..8) {
        $value = Get-MachineValue `
            -Information $Information `
            -Name "nic$index"
        if ($null -ne $value -and $value -ne 'none') {
            throw "VMのNIC$index が無効ではありません: $value"
        }
    }
}

function Assert-SafeIsoPath {
    param([Parameter(Mandatory)][string] $Path)

    $fullPath = [IO.Path]::GetFullPath($Path)
    $allowedPrefix = [IO.Path]::GetFullPath(
        (Join-Path $env:LOCALAPPDATA 'YTEC\ytec-disk-clone')) + '\'
    if (-not $fullPath.StartsWith(
            $allowedPrefix,
            [StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetExtension($fullPath) -ne '.iso' -or
        -not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
        throw "許可された外部WinPE ISOではありません: $fullPath"
    }
    $item = Get-Item -LiteralPath $fullPath -Force
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "WinPE ISOのreparse pointは使用しません: $fullPath"
    }
    return $fullPath
}

function Get-IsoReport {
    param(
        [Parameter(Mandatory)][string] $Path,
        [Parameter(Mandatory)][ValidateSet('2011CA', '2023CA')]
        [string] $ExpectedGeneration
    )

    $isoPath = Assert-SafeIsoPath -Path $Path
    $manifestPath = Join-Path `
        (Split-Path -Parent $isoPath) `
        'winpe-app-media-manifest.json'
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
        throw "WinPE ISO manifestがありません: $manifestPath"
    }
    $manifestItem = Get-Item -LiteralPath $manifestPath -Force
    if (($manifestItem.Attributes -band
            [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "WinPE ISO manifestのreparse pointは使用しません: $manifestPath"
    }
    $manifest = Get-Content -LiteralPath $manifestPath -Raw |
        ConvertFrom-Json
    $item = Get-Item -LiteralPath $isoPath -Force
    $sha256 = (Get-FileHash -LiteralPath $isoPath -Algorithm SHA256).Hash
    $bootManagers = @($manifest.uefiBootManagers)
    $expectedBootManagerCount = if ($ExpectedGeneration -eq '2023CA') {
        2
    }
    else {
        1
    }
    $guiEntries = @($manifest.addedFiles |
        Where-Object { $_.relativePath -eq 'YtecDiskClone\ytec-winpe-gui.exe' })
    if ([string]$manifest.certificateGeneration -ne $ExpectedGeneration -or
        [string]$manifest.validationScenario -ne 'Inventory' -or
        [bool]$manifest.repositoryContainsMicrosoftPayload -ne $false -or
        [IO.Path]::GetFullPath([string]$manifest.iso.path) -ne $isoPath -or
        [int64]$manifest.iso.length -ne $item.Length -or
        [string]$manifest.iso.sha256 -ne $sha256 -or
        [string]$manifest.winpeApp.sha256 -notmatch '^[0-9A-F]{64}$' -or
        $bootManagers.Count -ne $expectedBootManagerCount -or
        $guiEntries.Count -ne 1 -or
        [string]$guiEntries[0].sha256 -notmatch '^[0-9A-F]{64}$') {
        throw "WinPE ISOとmanifestが一致しません: $isoPath"
    }
    foreach ($bootManager in $bootManagers) {
        if (-not [bool]$bootManager.sourceWasWimProjection -or
            [string]$bootManager.reparseTag -ne '0x80000008' -or
            -not [bool]$bootManager.microsoftSignatureVerified -or
            [string]$bootManager.machine -ne 'AMD64' -or
            [string]$bootManager.subsystem -ne 'EFI Application') {
            throw "WinPE ISOのUEFIブートマネージャー監査が不正です: $isoPath"
        }
    }
    return [ordered]@{
        certificateGeneration = $ExpectedGeneration
        path = $isoPath
        bytes = $item.Length
        sha256 = $sha256
        manifestPath = $manifestItem.FullName
        manifestSha256 = (Get-FileHash `
            -LiteralPath $manifestItem.FullName `
            -Algorithm SHA256).Hash
        winpeAppSha256 = [string]$manifest.winpeApp.sha256
        winpeGuiSha256 = [string]$guiEntries[0].sha256
        uefiBootManagers = $bootManagers
    }
}

function Get-VmState {
    return Get-MachineValue -Information (Get-VmInformation) -Name 'VMState'
}

function Set-SecureBoot {
    param([Parameter(Mandatory)][bool] $Enabled)

    $setting = if ($Enabled) { '--enable' } else { '--disable' }
    $null = Invoke-VBox `
        -Arguments @('modifynvram', $VmName, 'secureboot', $setting) `
        -Operation "$VmName Secure Boot設定"
}

function Set-ScenarioConfiguration {
    param(
        [Parameter(Mandatory)][ValidateSet('BIOS', 'EFI64')]
        [string] $Firmware,
        [Parameter(Mandatory)][bool] $SecureBoot,
        [Parameter(Mandatory)][string] $IsoPath
    )

    $current = Get-VmInformation
    $currentFirmware = Get-MachineValue `
        -Information $current `
        -Name 'firmware'
    if ($Firmware -eq 'BIOS') {
        if ($currentFirmware -ne 'EFI64') {
            if ($currentFirmware -ne 'BIOS') {
                throw "想定外の現在firmwareです: $currentFirmware"
            }
        }
        else {
            Set-SecureBoot -Enabled $false
            $null = Invoke-VBox `
                -Arguments @('modifyvm', $VmName, '--firmware', 'bios') `
                -Operation "$VmName Legacy BIOS設定"
        }
    }
    else {
        if ($currentFirmware -eq 'BIOS') {
            $null = Invoke-VBox `
                -Arguments @('modifyvm', $VmName, '--firmware', 'efi64') `
                -Operation "$VmName UEFI64設定"
        }
        elseif ($currentFirmware -ne 'EFI64') {
            throw "想定外の現在firmwareです: $currentFirmware"
        }
        Set-SecureBoot -Enabled $SecureBoot
    }

    $null = Invoke-VBox `
        -Arguments @(
            'storageattach', $VmName,
            '--storagectl', 'IDE',
            '--port', '0',
            '--device', '0',
            '--type', 'dvddrive',
            '--medium', $IsoPath
        ) `
        -Operation "$VmName WinPE ISO接続"
    $null = Invoke-VBox `
        -Arguments @(
            'modifyvm', $VmName,
            '--boot1', 'dvd',
            '--boot2', 'none',
            '--boot3', 'none',
            '--boot4', 'none'
        ) `
        -Operation "$VmName DVD起動固定"

    $prepared = Get-VmInformation
    Assert-NetworkDisabled -Information $prepared
    if ((Get-MachineValue -Information $prepared -Name 'firmware') -ne
        $Firmware) {
        throw "VMのfirmwareを固定できませんでした: $Firmware"
    }
    if ($Firmware -eq 'EFI64') {
        $expectedSecureBoot = if ($SecureBoot) { 'on' } else { 'off' }
        if ((Get-MachineValue `
                -Information $prepared `
                -Name 'SecureBoot') -ne $expectedSecureBoot) {
            throw "VMのSecure Boot状態を固定できませんでした: $expectedSecureBoot"
        }
    }
    if ((Get-AttachmentValue `
            -Information $prepared `
            -Controller 'IDE' `
            -Port 0) -ne $IsoPath) {
        throw 'VMへ指定したWinPE ISOを接続できませんでした。'
    }
    return $prepared
}

function Save-Screenshot {
    param([Parameter(Mandatory)][string] $Path)

    $null = Invoke-VBox `
        -Arguments @('controlvm', $VmName, 'screenshotpng', $Path) `
        -Operation "$VmName 画面保存"
    $item = Get-Item -LiteralPath $Path -Force
    if ($item.Length -lt 10KB) {
        throw "VM画面証跡が小さすぎます: $Path"
    }
    return [ordered]@{
        name = $item.Name
        bytes = $item.Length
        sha256 = (Get-FileHash -LiteralPath $item.FullName `
            -Algorithm SHA256).Hash
    }
}

function Stop-TestVm {
    if ((Get-VmState) -eq 'running') {
        $null = Invoke-VBox `
            -Arguments @('controlvm', $VmName, 'poweroff') `
            -Operation "$VmName 試験後停止"
    }
    $deadline = (Get-Date).AddSeconds(20)
    while ((Get-Date) -lt $deadline) {
        if ((Get-VmState) -eq 'poweroff') {
            return
        }
        Start-Sleep -Milliseconds 500
    }
    throw "$VmName をpoweroffへ戻せませんでした。"
}

function Write-Json {
    param(
        [Parameter(Mandatory)][string] $Path,
        [Parameter(Mandatory)] $Value
    )

    [IO.File]::WriteAllText(
        $Path,
        ($Value | ConvertTo-Json -Depth 12),
        [Text.UTF8Encoding]::new($false))
}

foreach ($requiredFile in @(
    $vboxManage,
    $currentWinpeApp,
    $currentWinpeGui
)) {
    if (-not (Test-Path -LiteralPath $requiredFile -PathType Leaf)) {
        throw "必要ファイルがありません: $requiredFile"
    }
}
$iso2011 = Get-IsoReport -Path $Iso2011Ca -ExpectedGeneration '2011CA'
$iso2023 = Get-IsoReport -Path $Iso2023Ca -ExpectedGeneration '2023CA'
if ($iso2011.winpeAppSha256 -ne $iso2023.winpeAppSha256) {
    throw '2011 CA版と2023 CA版の製品WinPEApp SHA-256が一致しません。'
}
if ($iso2011.winpeGuiSha256 -ne $iso2023.winpeGuiSha256) {
    throw '2011 CA版と2023 CA版の製品WinPE GUI SHA-256が一致しません。'
}
if ((Get-FileHash -LiteralPath $currentWinpeApp -Algorithm SHA256).Hash -ne
        $iso2011.winpeAppSha256 -or
    (Get-FileHash -LiteralPath $currentWinpeGui -Algorithm SHA256).Hash -ne
        $iso2011.winpeGuiSha256) {
    throw '生成済みISOが現在の製品WinPE CLI/GUIと一致しません。'
}

New-Item -ItemType Directory -Path $evidenceRoot | Out-Null
$scenarios = @(
    [ordered]@{ key = 'bios-2011ca'; firmware = 'BIOS'; secureBoot = $false; iso = $iso2011 },
    [ordered]@{ key = 'bios-2023ca'; firmware = 'BIOS'; secureBoot = $false; iso = $iso2023 },
    [ordered]@{ key = 'uefi-2011ca-secureboot-off'; firmware = 'EFI64'; secureBoot = $false; iso = $iso2011 },
    [ordered]@{ key = 'uefi-2023ca-secureboot-off'; firmware = 'EFI64'; secureBoot = $false; iso = $iso2023 },
    [ordered]@{ key = 'uefi-2011ca-secureboot-on'; firmware = 'EFI64'; secureBoot = $true; iso = $iso2011 },
    [ordered]@{ key = 'uefi-2023ca-secureboot-on'; firmware = 'EFI64'; secureBoot = $true; iso = $iso2023 }
)

$originalInformation = $null
$originalMedium = $null
$originalFirmware = $null
$originalSecureBoot = $null
$originalBootOrder = $null
$configurationChanged = $false
$results = [Collections.Generic.List[object]]::new()
$failureMessage = $null
$cleanupFailure = $null

try {
    $running = @(Invoke-VBox `
        -Arguments @('list', 'runningvms') `
        -Operation '稼働中VM確認')
    if ($running.Count -ne 0) {
        throw "別VMが稼働中のため開始しません: $($running -join ', ')"
    }

    $originalInformation = Get-VmInformation
    $originalInformation | Set-Content `
        -LiteralPath $vmBeforePath `
        -Encoding UTF8
    if ((Get-MachineValue `
            -Information $originalInformation `
            -Name 'VMState') -ne 'poweroff') {
        throw '対象VMはpoweroffでなければなりません。'
    }
    Assert-NetworkDisabled -Information $originalInformation
    $controllerNames = @($originalInformation |
        Where-Object { $_ -like 'storagecontrollername*=*' })
    if ($controllerNames.Count -ne 1 -or
        $controllerNames[0] -ne 'storagecontrollername0="IDE"') {
        throw '起動マトリクスVMがHDDなしIDE専用構成ではありません。'
    }
    $originalMedium = Get-AttachmentValue `
        -Information $originalInformation `
        -Controller 'IDE' `
        -Port 0
    $originalMedium = Assert-SafeIsoPath -Path $originalMedium
    foreach ($position in @(
        @{ Port = 0; Device = 1 },
        @{ Port = 1; Device = 0 },
        @{ Port = 1; Device = 1 }
    )) {
        $attachment = Get-AttachmentValue `
            -Information $originalInformation `
            -Controller 'IDE' `
            -Port $position.Port `
            -Device $position.Device
        if ($attachment -ne 'none') {
            throw "起動マトリクスVMに想定外の追加媒体があります: IDE $($position.Port):$($position.Device)"
        }
    }
    $originalFirmware = Get-MachineValue `
        -Information $originalInformation `
        -Name 'firmware'
    $originalSecureBoot = Get-MachineValue `
        -Information $originalInformation `
        -Name 'SecureBoot'
    $originalBootOrder = foreach ($index in 1..4) {
        Get-MachineValue `
            -Information $originalInformation `
            -Name "boot$index"
    }

    foreach ($scenario in $scenarios) {
        Write-Output "TSUMUGI_BOOT_MATRIX_START=$($scenario.key)"
        $configurationChanged = $true
        $prepared = Set-ScenarioConfiguration `
            -Firmware $scenario.firmware `
            -SecureBoot $scenario.secureBoot `
            -IsoPath $scenario.iso.path
        $prepared | Set-Content `
            -LiteralPath (Join-Path $evidenceRoot `
                ($scenario.key + '-vm.txt')) `
            -Encoding UTF8

        $started = Get-Date
        $null = Invoke-VBox `
            -Arguments @('startvm', $VmName, '--type', 'headless') `
            -Operation "$VmName headless起動"
        foreach ($attempt in 1..15) {
            Start-Sleep -Milliseconds 750
            $null = Invoke-VBox `
                -Arguments @(
                    'controlvm', $VmName,
                    'keyboardputscancode', '39', 'b9'
                ) `
                -Operation 'WinPE DVD起動キー送信'
        }
        Start-Sleep -Seconds $BootWaitSeconds
        if ((Get-VmState) -ne 'running') {
            throw "$($scenario.key) の画面取得前にVMがrunningではなくなりました。"
        }
        $screenshotPath = Join-Path $evidenceRoot ($scenario.key + '.png')
        $screenshot = Save-Screenshot -Path $screenshotPath
        $captured = Get-Date
        $null = $results.Add([ordered]@{
            key = $scenario.key
            firmware = $scenario.firmware
            secureBoot = $scenario.secureBoot
            certificateGeneration = $scenario.iso.certificateGeneration
            isoPath = $scenario.iso.path
            isoSha256 = $scenario.iso.sha256
            winpeAppSha256 = $scenario.iso.winpeAppSha256
            started = $started.ToString('o')
            captured = $captured.ToString('o')
            elapsedSeconds = [Math]::Round(
                ($captured - $started).TotalSeconds,
                3)
            screenshot = $screenshot
            result = 'CAPTURED_PENDING_VISUAL_INSPECTION'
        })
        Stop-TestVm
        Write-Output "TSUMUGI_BOOT_MATRIX_CAPTURED=$($scenario.key)"
    }
}
catch {
    $failureMessage = $_.Exception.Message
}
finally {
    try {
        Stop-TestVm
        if ($configurationChanged) {
            $currentFirmware = Get-MachineValue `
                -Information (Get-VmInformation) `
                -Name 'firmware'
            if ($originalFirmware -eq 'EFI64') {
                if ($currentFirmware -eq 'BIOS') {
                    $null = Invoke-VBox `
                        -Arguments @(
                            'modifyvm', $VmName,
                            '--firmware', 'efi64'
                        ) `
                        -Operation "$VmName 元UEFI64復元"
                }
                Set-SecureBoot -Enabled ($originalSecureBoot -eq 'on')
            }
            elseif ($originalFirmware -eq 'BIOS') {
                if ($currentFirmware -eq 'EFI64') {
                    Set-SecureBoot -Enabled $false
                }
                $null = Invoke-VBox `
                    -Arguments @('modifyvm', $VmName, '--firmware', 'bios') `
                    -Operation "$VmName 元BIOS復元"
            }
            else {
                throw "復元できない元firmwareです: $originalFirmware"
            }

            $null = Invoke-VBox `
                -Arguments @(
                    'storageattach', $VmName,
                    '--storagectl', 'IDE',
                    '--port', '0',
                    '--device', '0',
                    '--type', 'dvddrive',
                    '--medium', $originalMedium
                ) `
                -Operation "$VmName 元ISO復元"
            $bootArguments = [Collections.Generic.List[string]]::new()
            $null = $bootArguments.Add('modifyvm')
            $null = $bootArguments.Add($VmName)
            for ($index = 0; $index -lt 4; ++$index) {
                $null = $bootArguments.Add('--boot' + ($index + 1))
                $null = $bootArguments.Add($originalBootOrder[$index])
            }
            $null = Invoke-VBox `
                -Arguments $bootArguments.ToArray() `
                -Operation "$VmName 元起動順序復元"
        }
    }
    catch {
        $cleanupFailure = $_.Exception.Message
    }
}

$restored = $false
$runningAfter = @()
$finalInformation = $null
try {
    $finalInformation = Get-VmInformation
    $finalInformation | Set-Content `
        -LiteralPath $vmAfterPath `
        -Encoding UTF8
    Assert-NetworkDisabled -Information $finalInformation
    $restored =
        (Get-MachineValue -Information $finalInformation -Name 'VMState') -eq
            'poweroff' -and
        (Get-MachineValue -Information $finalInformation -Name 'firmware') -eq
            $originalFirmware -and
        (Get-MachineValue -Information $finalInformation -Name 'SecureBoot') -eq
            $originalSecureBoot -and
        (Get-AttachmentValue `
            -Information $finalInformation `
            -Controller 'IDE' `
            -Port 0) -eq $originalMedium
    foreach ($index in 1..4) {
        $restored = $restored -and
            (Get-MachineValue `
                -Information $finalInformation `
                -Name "boot$index") -eq $originalBootOrder[$index - 1]
    }
    $runningAfter = @(Invoke-VBox `
        -Arguments @('list', 'runningvms') `
        -Operation '完了後稼働VM確認')
}
catch {
    if ([string]::IsNullOrWhiteSpace($cleanupFailure)) {
        $cleanupFailure = $_.Exception.Message
    }
}

$summary = [ordered]@{
    schemaVersion = 1
    purpose = 'Latest Y-TEC Tsumugi Drive product WinPE boot matrix'
    generated = (Get-Date).ToString('o')
    result = if ([string]::IsNullOrWhiteSpace($failureMessage) -and
        [string]::IsNullOrWhiteSpace($cleanupFailure) -and
        $restored -and
        $runningAfter.Count -eq 0 -and
        $results.Count -eq $scenarios.Count) {
        'CAPTURED_PENDING_VISUAL_INSPECTION'
    }
    else {
        'FAIL'
    }
    virtualBoxVersion = (@(Invoke-VBox `
        -Arguments @('--version') `
        -Operation 'VirtualBox版取得') | Select-Object -First 1)
    vm = $VmName
    physicalDiskOrUsbUsed = $false
    networkAdapters = 'none'
    hardDiskCount = 0
    iso2011Ca = $iso2011
    iso2023Ca = $iso2023
    tests = $results.ToArray()
    originalState = [ordered]@{
        firmware = $originalFirmware
        secureBoot = $originalSecureBoot
        dvd = $originalMedium
        bootOrder = $originalBootOrder
    }
    restored = $restored
    finalVmState = if ($null -eq $finalInformation) {
        $null
    }
    else {
        Get-MachineValue -Information $finalInformation -Name 'VMState'
    }
    runningVmCountAfter = $runningAfter.Count
    failure = $failureMessage
    cleanupFailure = $cleanupFailure
}
Write-Json -Path $summaryPath -Value $summary

if ($summary.result -eq 'FAIL') {
    throw "製品WinPE起動マトリクスの取得に失敗しました。証跡: $summaryPath"
}
Write-Output "TSUMUGI_BOOT_MATRIX_CAPTURE_PASS=$summaryPath"
