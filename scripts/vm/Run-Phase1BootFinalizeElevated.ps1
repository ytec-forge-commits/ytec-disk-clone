[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$workRoot = $PSScriptRoot
$bcdBootHarness = Join-Path $workRoot 'ytec-phase1-bcdboot-vm.exe'
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
    $output = @(& $powerShell `
        -NoProfile `
        -NonInteractive `
        -ExecutionPolicy Bypass `
        -File $ScriptPath @Arguments 2>&1)
    $exitCode = $LASTEXITCODE
    if (-not [IO.File]::Exists($StepDonePath)) {
        throw "$Name の完了マーカーがありません。終了コード: $exitCode`n$($output -join "`n")"
    }
    $stepResult = [IO.File]::ReadAllText($StepDonePath).Trim()
    if ($exitCode -ne 0 -or $stepResult -ne 'PASS') {
        $detail = if ([IO.File]::Exists($ResultPath)) {
            [IO.File]::ReadAllText($ResultPath)
        }
        else {
            $output -join "`n"
        }
        throw "$Name が失敗しました。終了コード: $exitCode`n$detail"
    }
}

function Get-UniqueVmDisk {
    param(
        [Parameter(Mandatory)][UInt64] $ExpectedBytes,
        [Parameter(Mandatory)][string] $Role
    )

    $matches = @(Get-Disk | Where-Object { [UInt64]$_.Size -eq $ExpectedBytes })
    if ($matches.Count -ne 1) {
        throw "$Role の固定容量VirtualBoxディスクを一意に特定できません。"
    }
    $disk = $matches[0]
    if ($disk.Number -eq 0 -or $disk.IsSystem -or $disk.IsBoot -or
        $disk.FriendlyName -notlike '*VBOX*' -or
        $disk.PartitionStyle -ne 'GPT') {
        throw "$Role が期待した専用非システムGPTディスクではありません。"
    }
    return $disk
}

function Get-SinglePartition {
    param(
        [Parameter(Mandatory)][object[]] $Partitions,
        [Parameter(Mandatory)][string] $GptType,
        [Parameter(Mandatory)][string] $Role
    )
    $matches = @($Partitions | Where-Object { $_.GptType -eq $GptType })
    if ($matches.Count -ne 1) {
        throw "$Role を一意に特定できません。"
    }
    return $matches[0]
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
    $currentDriveLetter = [char]$partition.DriveLetter
    if ($currentDriveLetter -ne [char]0) {
        return [string]"$currentDriveLetter`:\"
    }
    if (Get-Volume -DriveLetter $PreferredLetter -ErrorAction SilentlyContinue) {
        throw "一時ドライブ文字 $PreferredLetter`: は既に使用されています。"
    }
    $partition | Set-Partition -NewDriveLetter $PreferredLetter | Out-Null
    [void]$temporaryAccessPaths.Add([pscustomobject]@{
        DiskNumber = $DiskNumber
        PartitionNumber = $PartitionNumber
        AccessPath = "$PreferredLetter`:\"
    })
    return [string]"$PreferredLetter`:\"
}

function Get-SingleRoot {
    param(
        [Parameter(Mandatory)][scriptblock] $Operation,
        [Parameter(Mandatory)][string] $Role
    )
    $values = @(& $Operation)
    $roots = @($values | Where-Object {
        $_ -is [string] -and $_ -match '^[A-Za-z]:\\$'
    })
    if ($roots.Count -ne 1) {
        $descriptions = @($values | ForEach-Object {
            if ($null -eq $_) {
                '<null>'
            }
            else {
                '{0}=[{1}]' -f $_.GetType().FullName, ([string]$_)
            }
        })
        throw "$Role のドライブ文字ルートが単一文字列ではありません。" +
            " values=$($values.Count), roots=$($roots.Count), " +
            "details=$($descriptions -join '; ')"
    }
    return [string]$roots[0]
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
    foreach ($requiredFile in @($bcdBootHarness, $bcdBootScript)) {
        if (-not [IO.File]::Exists($requiredFile)) {
            throw "必要ファイルがありません: $requiredFile"
        }
    }

    $stage = 'disk-preflight'
    $source = Get-UniqueVmDisk -ExpectedBytes ([UInt64](96GB)) -Role 'コピー元'
    $target = Get-UniqueVmDisk -ExpectedBytes ([UInt64](110GB)) -Role 'コピー先'
    if ($source.Number -eq $target.Number) {
        throw 'コピー元とコピー先が同一ディスクです。'
    }
    if ($source.IsOffline) {
        Set-Disk -Number $source.Number -IsOffline $false
        $source = Get-Disk -Number $source.Number
    }
    if ($target.IsOffline) {
        Set-Disk -Number $target.Number -IsOffline $false
        $target = Get-Disk -Number $target.Number
    }
    if ($source.IsReadOnly -or $target.IsReadOnly) {
        throw 'コピー元またはコピー先が読取り専用状態です。'
    }

    $efiType = '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}'
    $basicType = '{ebd0a0a2-b9e5-4433-87c0-68b6b72699c7}'
    $sourcePartitions = @(Get-Partition -DiskNumber $source.Number | Sort-Object Offset)
    $targetPartitions = @(Get-Partition -DiskNumber $target.Number | Sort-Object Offset)
    if ($sourcePartitions.Count -ne $targetPartitions.Count -or
        $sourcePartitions.Count -lt 3) {
        throw 'コピー元とコピー先のGPT区画数が対応しません。'
    }
    for ($index = 0; $index -lt $sourcePartitions.Count; $index++) {
        if ($sourcePartitions[$index].GptType -ne $targetPartitions[$index].GptType -or
            $sourcePartitions[$index].Size -ne $targetPartitions[$index].Size) {
            throw "パーティション $index の種別または容量が一致しません。"
        }
    }
    $sourceWindows = Get-SinglePartition `
        -Partitions $sourcePartitions `
        -GptType $basicType `
        -Role 'コピー元Windowsパーティション'
    $targetWindows = Get-SinglePartition `
        -Partitions $targetPartitions `
        -GptType $basicType `
        -Role 'コピー先Windowsパーティション'
    $targetEsp = Get-SinglePartition `
        -Partitions $targetPartitions `
        -GptType $efiType `
        -Role 'コピー先EFIシステムパーティション'

    # MSFT_Disk.UniqueId is a transport identifier. VirtualBox SATA disks can
    # legitimately expose the same value ("ATAVBOX HARDDISK") for every disk,
    # so GPT identity must be compared with MSFT_Disk.Guid instead.
    $sourceDiskGuid = [string]$source.Guid
    $targetDiskGuid = [string]$target.Guid
    $sourcePartitionGuids = @($sourcePartitions | ForEach-Object { $_.Guid.ToString() })
    $targetPartitionGuids = @($targetPartitions | ForEach-Object { $_.Guid.ToString() })
    $partitionGuidsRegenerated = $true
    foreach ($sourceGuid in $sourcePartitionGuids) {
        if ($targetPartitionGuids -contains $sourceGuid) {
            $partitionGuidsRegenerated = $false
        }
    }
    if ([string]::IsNullOrWhiteSpace($sourceDiskGuid) -or
        [string]::IsNullOrWhiteSpace($targetDiskGuid) -or
        $sourceDiskGuid -eq $targetDiskGuid -or
        -not $partitionGuidsRegenerated) {
        throw 'コピー先のGPTディスクGUIDまたはパーティションGUIDが再生成されていません。'
    }

    $sourceWindowsRoot = Get-SingleRoot `
        -Role 'コピー元Windows' `
        -Operation {
            Get-PartitionRoot `
                -DiskNumber $source.Number `
                -PartitionNumber $sourceWindows.PartitionNumber `
                -PreferredLetter 'V'
        }
    $targetWindowsRoot = Get-SingleRoot `
        -Role 'コピー先Windows' `
        -Operation {
            Get-PartitionRoot `
                -DiskNumber $target.Number `
                -PartitionNumber $targetWindows.PartitionNumber `
                -PreferredLetter 'W'
        }
    $targetEspRoot = Get-SingleRoot `
        -Role 'コピー先ESP' `
        -Operation {
            Get-PartitionRoot `
                -DiskNumber $target.Number `
                -PartitionNumber $targetEsp.PartitionNumber `
                -PreferredLetter 'S'
        }
    if ($targetWindowsRoot[0] -eq $targetEspRoot[0]) {
        throw 'コピー先WindowsとESPのドライブ文字が同一です。'
    }

    $sourceHashes = [ordered]@{
        ntoskrnl = Get-FileSha256 `
            -Path (Join-Path $sourceWindowsRoot 'Windows\System32\ntoskrnl.exe')
        winloadEfi = Get-FileSha256 `
            -Path (Join-Path $sourceWindowsRoot 'Windows\System32\winload.efi')
    }
    $targetHashes = [ordered]@{
        ntoskrnl = Get-FileSha256 `
            -Path (Join-Path $targetWindowsRoot 'Windows\System32\ntoskrnl.exe')
        winloadEfi = Get-FileSha256 `
            -Path (Join-Path $targetWindowsRoot 'Windows\System32\winload.efi')
    }
    if ($sourceHashes.ntoskrnl -ne $targetHashes.ntoskrnl -or
        $sourceHashes.winloadEfi -ne $targetHashes.winloadEfi) {
        throw 'コピー元とコピー先のWindowsファイルSHA-256が一致しません。'
    }

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
    $confirmationLine = @([IO.File]::ReadAllLines($bcdPlanResult) | Where-Object {
        $_ -like 'confirmation=*'
    }) | Select-Object -Last 1
    if ($confirmationLine -notmatch '^confirmation=(.+)$') {
        throw 'BCDBoot計画から確認文字列を取得できません。'
    }
    $confirmation = $Matches[1]

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
            '-Confirmation', $confirmation,
            '-ResultPath', $bcdResult,
            '-DonePath', $bcdDone
        ) `
        -ResultPath $bcdResult `
        -StepDonePath $bcdDone
    if ([IO.File]::ReadAllText($bcdResult) -notmatch 'YDC_VM_BCDBOOT_PASS') {
        throw 'BCDBoot結果にPASSマーカーがありません。'
    }

    $stage = 'validation'
    $targetHashes['bootmgfwEfi'] = Get-FileSha256 `
        -Path (Join-Path $targetEspRoot 'EFI\Microsoft\Boot\bootmgfw.efi')
    $targetHashes['windowsBootmgfwEfi'] = Get-FileSha256 `
        -Path (Join-Path $targetWindowsRoot 'Windows\Boot\EFI\bootmgfw.efi')
    if ($targetHashes['bootmgfwEfi'] -ne
        $targetHashes['windowsBootmgfwEfi']) {
        throw 'コピー先ESPとWindowsのUEFIブートファイルSHA-256が一致しません。'
    }
    if (-not (Test-Path `
            -LiteralPath (Join-Path $targetEspRoot 'EFI\Microsoft\Boot\BCD') `
            -PathType Leaf)) {
        throw 'コピー先ESPにBCDストアがありません。'
    }

    $stage = 'complete'
    $summary = [ordered]@{
        schemaVersion = 1
        result = 'PASS'
        mode = 'resume-after-clone'
        elevated = $isElevated
        sourceDiskNumber = [int]$source.Number
        targetDiskNumber = [int]$target.Number
        sourceDiskBytes = [UInt64]$source.Size
        targetDiskBytes = [UInt64]$target.Size
        sourcePartitionCount = $sourcePartitions.Count
        targetPartitionCount = $targetPartitions.Count
        diskGuidRegenerated = $true
        partitionGuidsRegenerated = $true
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
        mode = 'resume-after-clone'
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
            # Preserve the authoritative test result if temporary cleanup fails.
        }
    }
}
