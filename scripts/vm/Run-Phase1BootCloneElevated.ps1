[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$workRoot = $PSScriptRoot
$cloneHarness = Join-Path $workRoot 'ytec-phase1-physical-clone-vm.exe'
$bcdBootHarness = Join-Path $workRoot 'ytec-phase1-bcdboot-vm.exe'
$cloneScript = Join-Path $workRoot 'Invoke-Phase1PhysicalClone.ps1'
$bcdBootScript = Join-Path $workRoot 'Invoke-Phase1BcdBoot.ps1'
$summaryPath = Join-Path $workRoot 'boot-clone-summary.json'
$donePath = Join-Path $workRoot 'boot-clone.done.txt'
$utf8 = [Text.UTF8Encoding]::new($false)
$stage = 'startup'
$isElevated = $false
$temporaryAccessPaths = [Collections.Generic.List[object]]::new()

function Write-Utf8Text {
    param(
        [Parameter(Mandatory)][string] $Path,
        [Parameter(Mandatory)][string] $Value
    )
    [IO.File]::WriteAllText($Path, $Value, $utf8)
}

function Invoke-Phase1Step {
    param(
        [Parameter(Mandatory)][string] $Name,
        [Parameter(Mandatory)][string] $ScriptPath,
        [Parameter(Mandatory)][string[]] $Arguments,
        [Parameter(Mandatory)][string] $ResultPath,
        [Parameter(Mandatory)][string] $StepDonePath
    )

    $powerShell = Join-Path $PSHOME 'powershell.exe'
    $null = & $powerShell `
        -NoProfile `
        -NonInteractive `
        -ExecutionPolicy Bypass `
        -File $ScriptPath @Arguments
    $exitCode = $LASTEXITCODE
    if (-not [IO.File]::Exists($StepDonePath)) {
        throw "$Name の完了マーカーがありません。終了コード: $exitCode"
    }
    $stepResult = [IO.File]::ReadAllText($StepDonePath).Trim()
    if ($exitCode -ne 0 -or $stepResult -ne 'PASS') {
        $detail = if ([IO.File]::Exists($ResultPath)) {
            [IO.File]::ReadAllText($ResultPath)
        }
        else {
            '結果ファイルなし'
        }
        throw "$Name が失敗しました。終了コード: $exitCode`n$detail"
    }
}

function Get-UniqueVmDisk {
    param(
        [Parameter(Mandatory)][UInt64] $ExpectedBytes,
        [Parameter(Mandatory)][string] $ExpectedStyle,
        [Parameter(Mandatory)][string] $Role
    )

    $matches = @(Get-Disk | Where-Object { [UInt64]$_.Size -eq $ExpectedBytes })
    if ($matches.Count -ne 1) {
        throw "$Role の固定容量VirtualBoxディスクを一意に特定できません。"
    }
    $disk = $matches[0]
    if ($disk.Number -eq 0 -or $disk.IsSystem -or $disk.IsBoot -or
        $disk.FriendlyName -notlike '*VBOX*' -or
        $disk.PartitionStyle.ToString() -ne $ExpectedStyle) {
        throw "$Role が期待した専用非システムディスクではありません。"
    }
    return $disk
}

function Get-PartitionRoot {
    param(
        [Parameter(Mandatory)][int] $DiskNumber,
        [Parameter(Mandatory)][int] $PartitionNumber,
        [Parameter(Mandatory)][char] $PreferredLetter
    )

    $partition = Get-Partition `
        -DiskNumber $DiskNumber `
        -PartitionNumber $PartitionNumber
    if ($null -ne $partition.DriveLetter) {
        return "$($partition.DriveLetter):\"
    }
    if (Get-Volume -DriveLetter $PreferredLetter -ErrorAction SilentlyContinue) {
        throw "一時ドライブ文字 $PreferredLetter`: は既に使用されています。"
    }
    $null = $partition | Set-Partition -NewDriveLetter $PreferredLetter
    [void]$temporaryAccessPaths.Add([pscustomobject]@{
        DiskNumber = $DiskNumber
        PartitionNumber = $PartitionNumber
        AccessPath = "$PreferredLetter`:\"
    })
    return "$PreferredLetter`:\"
}

function Get-FileSha256 {
    param([Parameter(Mandatory)][string] $Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "検証対象ファイルがありません: $Path"
    }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

try {
    $principal = [Security.Principal.WindowsPrincipal]::new(
        [Security.Principal.WindowsIdentity]::GetCurrent())
    if (-not $principal.IsInRole(
            [Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'UACで昇格した管理者トークンではありません。'
    }
    $isElevated = $true

    foreach ($requiredFile in @(
        $cloneHarness,
        $bcdBootHarness,
        $cloneScript,
        $bcdBootScript
    )) {
        if (-not [IO.File]::Exists($requiredFile)) {
            throw "必要ファイルがありません: $requiredFile"
        }
    }

    $stage = 'disk-preflight'
    $source = Get-UniqueVmDisk `
        -ExpectedBytes ([UInt64](96GB)) `
        -ExpectedStyle 'GPT' `
        -Role 'コピー元'
    $target = Get-UniqueVmDisk `
        -ExpectedBytes ([UInt64](110GB)) `
        -ExpectedStyle 'RAW' `
        -Role 'コピー先'
    if ($source.Number -eq $target.Number) {
        throw 'コピー元とコピー先が同一ディスクです。'
    }
    if ($source.IsOffline) {
        Set-Disk -Number $source.Number -IsOffline $false
        $source = Get-Disk -Number $source.Number
    }
    if ($source.IsReadOnly) {
        throw 'コピー元が読取り専用状態のためボリューム対応を安全に確認できません。'
    }
    $sourcePartitions = @(Get-Partition -DiskNumber $source.Number | Sort-Object Offset)
    $sourceWindows = @($sourcePartitions | Where-Object {
        $_.GptType -eq '{ebd0a0a2-b9e5-4433-87c0-68b6b72699c7}'
    })
    $sourceEsp = @($sourcePartitions | Where-Object {
        $_.GptType -eq '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}'
    })
    if ($sourceWindows.Count -ne 1 -or $sourceEsp.Count -ne 1 -or
        $sourcePartitions.Count -lt 3) {
        throw 'コピー元Windows GPTレイアウトを一意に特定できません。'
    }

    $sourceWindowsRoot = Get-PartitionRoot `
        -DiskNumber $source.Number `
        -PartitionNumber $sourceWindows[0].PartitionNumber `
        -PreferredLetter 'V'
    $sourceHashes = [ordered]@{
        ntoskrnl = Get-FileSha256 `
            -Path (Join-Path $sourceWindowsRoot 'Windows\System32\ntoskrnl.exe')
        winloadEfi = Get-FileSha256 `
            -Path (Join-Path $sourceWindowsRoot 'Windows\System32\winload.efi')
    }
    $sourceDiskGuid = $source.UniqueId
    $sourcePartitionGuids = @($sourcePartitions | ForEach-Object { $_.Guid.ToString() })

    $planResult = Join-Path $workRoot 'boot-clone-plan.txt'
    $planDone = Join-Path $workRoot 'boot-clone-plan.done.txt'
    $stage = 'clone-plan'
    Invoke-Phase1Step `
        -Name 'ブート用VMクローン計画' `
        -ScriptPath $cloneScript `
        -Arguments @(
            '-Mode', 'Plan',
            '-HarnessPath', $cloneHarness,
            '-SourceDiskNumber', $source.Number.ToString(),
            '-TargetDiskNumber', $target.Number.ToString(),
            '-BootTest',
            '-ResultPath', $planResult,
            '-DonePath', $planDone
        ) `
        -ResultPath $planResult `
        -StepDonePath $planDone
    $confirmationLine = @([IO.File]::ReadAllLines($planResult) | Where-Object {
        $_ -like 'confirmation=*'
    }) | Select-Object -Last 1
    if ($confirmationLine -notmatch '^confirmation=(.+)$') {
        throw 'ブート用クローン計画から確認文字列を取得できません。'
    }
    $cloneConfirmation = $Matches[1]

    $cloneResult = Join-Path $workRoot 'boot-clone.txt'
    $cloneDone = Join-Path $workRoot 'boot-clone-step.done.txt'
    $stage = 'clone-execute'
    Invoke-Phase1Step `
        -Name 'ブート用VM物理クローン' `
        -ScriptPath $cloneScript `
        -Arguments @(
            '-Mode', 'Execute',
            '-HarnessPath', $cloneHarness,
            '-SourceDiskNumber', $source.Number.ToString(),
            '-TargetDiskNumber', $target.Number.ToString(),
            '-BootTest',
            '-Confirmation', $cloneConfirmation,
            '-ResultPath', $cloneResult,
            '-DonePath', $cloneDone
        ) `
        -ResultPath $cloneResult `
        -StepDonePath $cloneDone
    if ([IO.File]::ReadAllText($cloneResult) -notmatch 'YDC_VM_CLONE_PASS') {
        throw 'ブート用物理クローン結果にPASSマーカーがありません。'
    }

    $stage = 'target-refresh'
    Update-HostStorageCache
    Start-Sleep -Seconds 2
    $target = Get-Disk -Number $target.Number
    if ($target.PartitionStyle -ne 'GPT' -or $target.IsOffline) {
        throw 'クローン後のコピー先がオンラインGPTとして再列挙されません。'
    }
    $targetPartitions = @(Get-Partition -DiskNumber $target.Number | Sort-Object Offset)
    $targetWindows = @($targetPartitions | Where-Object {
        $_.GptType -eq '{ebd0a0a2-b9e5-4433-87c0-68b6b72699c7}'
    })
    $targetEsp = @($targetPartitions | Where-Object {
        $_.GptType -eq '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}'
    })
    if ($targetWindows.Count -ne 1 -or $targetEsp.Count -ne 1 -or
        $targetPartitions.Count -ne $sourcePartitions.Count) {
        throw 'コピー先Windows GPTレイアウトがコピー元と対応しません。'
    }

    $targetWindowsRoot = Get-PartitionRoot `
        -DiskNumber $target.Number `
        -PartitionNumber $targetWindows[0].PartitionNumber `
        -PreferredLetter 'W'
    $targetEspRoot = Get-PartitionRoot `
        -DiskNumber $target.Number `
        -PartitionNumber $targetEsp[0].PartitionNumber `
        -PreferredLetter 'S'

    $bcdPlanResult = Join-Path $workRoot 'bcdboot-plan.txt'
    $bcdPlanDone = Join-Path $workRoot 'bcdboot-plan.done.txt'
    $stage = 'bcdboot-plan'
    Invoke-Phase1Step `
        -Name 'BCDBoot計画' `
        -ScriptPath $bcdBootScript `
        -Arguments @(
            '-Mode', 'Plan',
            '-HarnessPath', $bcdBootHarness,
            '-TargetDiskNumber', $target.Number.ToString(),
            '-WindowsRoot', $targetWindowsRoot,
            '-EspRoot', $targetEspRoot,
            '-ResultPath', $bcdPlanResult,
            '-DonePath', $bcdPlanDone
        ) `
        -ResultPath $bcdPlanResult `
        -StepDonePath $bcdPlanDone
    $bcdConfirmationLine = @([IO.File]::ReadAllLines($bcdPlanResult) | Where-Object {
        $_ -like 'confirmation=*'
    }) | Select-Object -Last 1
    if ($bcdConfirmationLine -notmatch '^confirmation=(.+)$') {
        throw 'BCDBoot計画から確認文字列を取得できません。'
    }
    $bcdConfirmation = $Matches[1]

    $bcdResult = Join-Path $workRoot 'bcdboot.txt'
    $bcdDone = Join-Path $workRoot 'bcdboot.done.txt'
    $stage = 'bcdboot-execute'
    Invoke-Phase1Step `
        -Name '署名検証済みBCDBoot' `
        -ScriptPath $bcdBootScript `
        -Arguments @(
            '-Mode', 'Execute',
            '-HarnessPath', $bcdBootHarness,
            '-TargetDiskNumber', $target.Number.ToString(),
            '-WindowsRoot', $targetWindowsRoot,
            '-EspRoot', $targetEspRoot,
            '-Confirmation', $bcdConfirmation,
            '-ResultPath', $bcdResult,
            '-DonePath', $bcdDone
        ) `
        -ResultPath $bcdResult `
        -StepDonePath $bcdDone
    if ([IO.File]::ReadAllText($bcdResult) -notmatch 'YDC_VM_BCDBOOT_PASS') {
        throw 'BCDBoot結果にPASSマーカーがありません。'
    }

    $stage = 'validation'
    $targetHashes = [ordered]@{
        ntoskrnl = Get-FileSha256 `
            -Path (Join-Path $targetWindowsRoot 'Windows\System32\ntoskrnl.exe')
        winloadEfi = Get-FileSha256 `
            -Path (Join-Path $targetWindowsRoot 'Windows\System32\winload.efi')
        bootmgfwEfi = Get-FileSha256 `
            -Path (Join-Path $targetEspRoot 'EFI\Microsoft\Boot\bootmgfw.efi')
        windowsBootmgfwEfi = Get-FileSha256 `
            -Path (Join-Path $targetWindowsRoot 'Windows\Boot\EFI\bootmgfw.efi')
    }
    if ($sourceHashes.ntoskrnl -ne $targetHashes.ntoskrnl -or
        $sourceHashes.winloadEfi -ne $targetHashes.winloadEfi -or
        $targetHashes.bootmgfwEfi -ne $targetHashes.windowsBootmgfwEfi) {
        throw 'WindowsまたはUEFIブートファイルのSHA-256が一致しません。'
    }
    $bcdPath = Join-Path $targetEspRoot 'EFI\Microsoft\Boot\BCD'
    if (-not (Test-Path -LiteralPath $bcdPath -PathType Leaf)) {
        throw 'コピー先ESPにBCDストアがありません。'
    }
    $targetPartitionGuids = @($targetPartitions | ForEach-Object { $_.Guid.ToString() })
    $partitionGuidsRegenerated = $true
    foreach ($sourceGuid in $sourcePartitionGuids) {
        if ($targetPartitionGuids -contains $sourceGuid) {
            $partitionGuidsRegenerated = $false
        }
    }
    if ($target.UniqueId -eq $sourceDiskGuid -or -not $partitionGuidsRegenerated) {
        throw 'コピー先のGPTディスクGUIDまたはパーティションGUIDが再生成されていません。'
    }
    for ($index = 0; $index -lt $sourcePartitions.Count; $index++) {
        if ($sourcePartitions[$index].GptType -ne $targetPartitions[$index].GptType -or
            $sourcePartitions[$index].Size -ne $targetPartitions[$index].Size) {
            throw "パーティション $index の種別または容量が一致しません。"
        }
    }

    $stage = 'complete'
    $summary = [ordered]@{
        schemaVersion = 1
        result = 'PASS'
        elevated = $isElevated
        sourceDiskNumber = [int]$source.Number
        targetDiskNumber = [int]$target.Number
        sourceDiskBytes = [UInt64]$source.Size
        targetDiskBytes = [UInt64]$target.Size
        sourcePartitionCount = $sourcePartitions.Count
        targetPartitionCount = $targetPartitions.Count
        diskGuidRegenerated = $target.UniqueId -ne $sourceDiskGuid
        partitionGuidsRegenerated = $partitionGuidsRegenerated
        windowsFileHashesMatched = $true
        efiBootFileHashMatched = $true
        bcdStorePresent = $true
        hashes = $targetHashes
        completedUtc = [DateTimeOffset]::UtcNow
    } | ConvertTo-Json -Depth 5
    Write-Utf8Text -Path $summaryPath -Value $summary
    Write-Utf8Text -Path $donePath -Value 'PASS'
    exit 0
}
catch {
    $errorRecord = $_
    $failure = [ordered]@{
        schemaVersion = 1
        result = 'FAIL'
        elevated = $isElevated
        stage = $stage
        message = $errorRecord.Exception.Message
        fullyQualifiedErrorId = $errorRecord.FullyQualifiedErrorId
        category = $errorRecord.CategoryInfo.Category.ToString()
        position = $errorRecord.InvocationInfo.PositionMessage
        scriptStackTrace = $errorRecord.ScriptStackTrace
        completedUtc = [DateTimeOffset]::UtcNow
    } | ConvertTo-Json -Depth 5
    Write-Utf8Text -Path $summaryPath -Value $failure
    Write-Utf8Text -Path $donePath -Value 'FAIL'
    exit 1
}
finally {
    foreach ($accessPath in $temporaryAccessPaths) {
        try {
            Remove-PartitionAccessPath `
                -DiskNumber $accessPath.DiskNumber `
                -PartitionNumber $accessPath.PartitionNumber `
                -AccessPath $accessPath.AccessPath `
                -ErrorAction Stop
        }
        catch {
            # The result already records the authoritative test outcome. A
            # temporary letter cleanup failure must not replace that evidence.
        }
    }
}
