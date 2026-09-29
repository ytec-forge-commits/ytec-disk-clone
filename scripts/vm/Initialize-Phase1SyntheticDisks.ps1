[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $ResultPath,
    [Parameter(Mandatory)]
    [string] $DonePath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

trap {
    $errorRecord = $_
    $failure = [ordered]@{
        schemaVersion = 1
        result = 'FAIL'
        message = $errorRecord.Exception.Message
        fullyQualifiedErrorId = $errorRecord.FullyQualifiedErrorId
        category = $errorRecord.CategoryInfo.Category.ToString()
        position = $errorRecord.InvocationInfo.PositionMessage
        scriptStackTrace = $errorRecord.ScriptStackTrace
    } | ConvertTo-Json -Compress
    [IO.File]::WriteAllText($ResultPath, $failure, [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText($DonePath, 'FAIL', [Text.UTF8Encoding]::new($false))
    Write-Error $errorRecord
    exit 1
}

$principal = [Security.Principal.WindowsPrincipal]::new(
    [Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw '合成物理ディスクの初期化には管理者権限が必要です。'
}

$sourceBytes = 2GB
$targetBytes = 3GB
$sizeTolerance = 1MB
$efiType = '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}'
$msrType = '{e3c9e316-0b5c-4db8-817d-f92df00215ae}'
$basicType = '{ebd0a0a2-b9e5-4433-87c0-68b6b72699c7}'
$recoveryType = '{de94bba4-06d1-4d40-a16a-bfd50179d6ac}'

function Get-UniqueTestDisk {
    param(
        [Parameter(Mandatory)]
        [UInt64] $ExpectedBytes,
        [Parameter(Mandatory)]
        [string] $Role
    )

    $matches = @(Get-Disk | Where-Object {
        [Math]::Abs([double]$_.Size - [double]$ExpectedBytes) -le $sizeTolerance
    })
    if ($matches.Count -ne 1) {
        throw "$Role 用の専用仮想ディスクを一意に特定できません。"
    }
    $disk = $matches[0]
    if ($disk.Number -eq 0 -or $disk.IsSystem -or $disk.IsBoot -or
        $disk.FriendlyName -notlike '*VBOX*') {
        throw "$Role がVirtualBox専用データディスクではありません。"
    }
    return $disk
}

function Get-VolumeRoot {
    param(
        [Parameter(Mandatory)]
        [Microsoft.Management.Infrastructure.CimInstance] $Partition
    )

    $volume = Get-Volume -Partition $Partition
    $path = @($volume.Path | Where-Object { $_ -like '\\?\Volume{*}\' }) |
        Select-Object -First 1
    if ([string]::IsNullOrWhiteSpace($path)) {
        throw "Volume GUIDパスを取得できません: partition $($Partition.PartitionNumber)"
    }
    return $path
}

function Remove-AutomaticMbrToGptReservedPartition {
    param(
        [Parameter(Mandatory)]
        [int] $DiskNumber,
        [switch] $RequireKnownFivePartitionLayout
    )

    $partitions = @(Get-Partition -DiskNumber $DiskNumber | Sort-Object Offset)
    $reserved = @($partitions | Where-Object { $_.GptType -eq $msrType })
    $automatic = @($reserved | Where-Object {
        $_.Offset -lt 1MB -and $_.Size -gt 15MB -and $_.Size -le 16MB
    })
    if ($automatic.Count -ne 1) {
        throw 'Initialize-Diskが自動作成した先頭MSRを一意に特定できません。'
    }

    if ($RequireKnownFivePartitionLayout) {
        $efi = @($partitions | Where-Object {
            $_.GptType -eq $efiType -and $_.Size -eq 128MB
        })
        $explicitReserved = @($reserved | Where-Object {
            $_.Offset -ge 1MB -and $_.Size -eq 16MB
        })
        $data = @($partitions | Where-Object {
            $_.GptType -eq $basicType -and $_.Size -eq 1400MB
        })
        $recovery = @($partitions | Where-Object {
            $_.GptType -eq $recoveryType
        })
        if ($partitions.Count -ne 5 -or
            $reserved.Count -ne 2 -or
            $efi.Count -ne 1 -or
            $explicitReserved.Count -ne 1 -or
            $data.Count -ne 1 -or
            $recovery.Count -ne 1) {
            throw '既存GPTは既知の5区画合成テストレイアウトではありません。補正しません。'
        }

        $knownVolumes = @(
            @{ Partition = $efi[0]; Label = 'YDC-ESP' }
            @{ Partition = $data[0]; Label = 'YDC-DATA' }
            @{ Partition = $recovery[0]; Label = 'YDC-RECOVERY' }
        )
        foreach ($knownVolume in $knownVolumes) {
            $volume = Get-Volume -Partition $knownVolume.Partition
            if ($volume.FileSystemLabel -ne $knownVolume.Label) {
                throw "既存GPTの合成ラベルが一致しません: $($knownVolume.Label)"
            }
        }
    }

    Remove-Partition `
        -DiskNumber $DiskNumber `
        -PartitionNumber $automatic[0].PartitionNumber `
        -Confirm:$false
}

$source = Get-UniqueTestDisk -ExpectedBytes $sourceBytes -Role 'コピー元'
$target = Get-UniqueTestDisk -ExpectedBytes $targetBytes -Role 'コピー先'
if ($source.Number -eq $target.Number) {
    throw 'コピー元とコピー先が同じディスクです。'
}
if ($target.PartitionStyle -ne 'RAW') {
    throw 'コピー先は未初期化RAWでなければなりません。既存結果は上書きしません。'
}

if ($source.PartitionStyle -eq 'RAW') {
    $null = Set-Disk -Number $source.Number -IsOffline $false
    $null = Set-Disk -Number $source.Number -IsReadOnly $false
    $null = Initialize-Disk -Number $source.Number -PartitionStyle GPT

    # Initialize-Disk creates an MSR before we can place the ESP. Remove only
    # that narrowly identified automatic partition, then create the intended
    # ESP/MSR/data/recovery test layout explicitly.
    Remove-AutomaticMbrToGptReservedPartition -DiskNumber $source.Number

    $esp = New-Partition `
        -DiskNumber $source.Number `
        -Size 128MB `
        -GptType $efiType
    $null = Format-Volume `
        -Partition $esp `
        -FileSystem FAT32 `
        -NewFileSystemLabel 'YDC-ESP' `
        -Confirm:$false

    $null = New-Partition `
        -DiskNumber $source.Number `
        -Size 16MB `
        -GptType $msrType

    $data = New-Partition `
        -DiskNumber $source.Number `
        -Size 1400MB `
        -GptType $basicType
    $null = Format-Volume `
        -Partition $data `
        -FileSystem NTFS `
        -NewFileSystemLabel 'YDC-DATA' `
        -AllocationUnitSize 4096 `
        -Confirm:$false

    $recovery = New-Partition `
        -DiskNumber $source.Number `
        -UseMaximumSize `
        -GptType $recoveryType
    $null = Format-Volume `
        -Partition $recovery `
        -FileSystem NTFS `
        -NewFileSystemLabel 'YDC-RECOVERY' `
        -AllocationUnitSize 4096 `
        -Confirm:$false

    $espRoot = Get-VolumeRoot -Partition $esp
    $dataRoot = Get-VolumeRoot -Partition $data
    $recoveryRoot = Get-VolumeRoot -Partition $recovery
    [IO.File]::WriteAllText(
        ([IO.Path]::Combine($espRoot, 'ydc-efi-marker.txt')),
        'YDC EFI RAW COPY MARKER V1',
        [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText(
        ([IO.Path]::Combine($dataRoot, 'ydc-ntfs-marker.txt')),
        'YDC NTFS USED CLUSTER MARKER V1',
        [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText(
        ([IO.Path]::Combine($recoveryRoot, 'ydc-recovery-marker.txt')),
        'YDC RECOVERY RAW COPY MARKER V1',
        [Text.UTF8Encoding]::new($false))

    $payloadPath = [IO.Path]::Combine($dataRoot, 'ydc-payload.bin')
    $payload = [byte[]]::new(1MB)
    for ($index = 0; $index -lt $payload.Length; $index++) {
        $payload[$index] = [byte](($index * 31 + 17) % 251)
    }
    $stream = [IO.File]::Open(
        $payloadPath,
        [IO.FileMode]::CreateNew,
        [IO.FileAccess]::Write,
        [IO.FileShare]::None)
    try {
        foreach ($block in 1..8) {
            $stream.Write($payload, 0, $payload.Length)
        }
        $stream.Flush($true)
    }
    finally {
        $stream.Dispose()
    }
    $source = Get-Disk -Number $source.Number
}
elseif ($source.PartitionStyle -ne 'GPT') {
    throw 'コピー元がGPTでもRAWでもありません。'
}
else {
    $existingPartitions = @(Get-Partition -DiskNumber $source.Number)
    if ($existingPartitions.Count -eq 5) {
        Remove-AutomaticMbrToGptReservedPartition `
            -DiskNumber $source.Number `
            -RequireKnownFivePartitionLayout
    }
}

$partitions = @(Get-Partition -DiskNumber $source.Number)
if ($partitions.Count -ne 4) {
    throw "合成コピー元のパーティション数が4ではありません: $($partitions.Count)"
}

$result = [ordered]@{
    schemaVersion = 1
    result = 'READY'
    sourceDiskNumber = [int]$source.Number
    sourceBytes = [UInt64]$source.Size
    targetDiskNumber = [int]$target.Number
    targetBytes = [UInt64]$target.Size
    sourcePartitionCount = $partitions.Count
} | ConvertTo-Json -Compress
[IO.File]::WriteAllText($ResultPath, $result, [Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText($DonePath, 'PASS', [Text.UTF8Encoding]::new($false))
$result
