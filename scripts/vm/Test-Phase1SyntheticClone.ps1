[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateRange(1, 128)]
    [int] $SourceDiskNumber,
    [Parameter(Mandatory)]
    [ValidateRange(1, 128)]
    [int] $TargetDiskNumber,
    [Parameter(Mandatory)]
    [string] $ResultPath,
    [Parameter(Mandatory)]
    [string] $DonePath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

trap {
    $failure = [ordered]@{
        schemaVersion = 1
        result = 'FAIL'
        message = $_.Exception.Message
    } | ConvertTo-Json -Compress
    [IO.File]::WriteAllText($ResultPath, $failure, [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText($DonePath, 'FAIL', [Text.UTF8Encoding]::new($false))
    Write-Error $_
    exit 1
}

function Get-SinglePartitionByType {
    param(
        [Parameter(Mandatory)]
        [int] $DiskNumber,
        [Parameter(Mandatory)]
        [string] $GptType
    )

    $matches = @(Get-Partition -DiskNumber $DiskNumber | Where-Object {
        $_.GptType -eq $GptType
    })
    if ($matches.Count -ne 1) {
        throw "GPT種別を一意に取得できません: disk=$DiskNumber type=$GptType"
    }
    return $matches[0]
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

function Compare-FileHash {
    param(
        [Parameter(Mandatory)]
        [string] $SourcePath,
        [Parameter(Mandatory)]
        [string] $TargetPath
    )

    if (-not [IO.File]::Exists($SourcePath) -or
        -not [IO.File]::Exists($TargetPath)) {
        throw "比較対象ファイルがありません: $SourcePath / $TargetPath"
    }
    $sha256 = [Security.Cryptography.SHA256]::Create()
    try {
        $sourceStream = [IO.File]::OpenRead($SourcePath)
        try {
            $sourceHash = [BitConverter]::ToString(
                $sha256.ComputeHash($sourceStream)).Replace('-', '')
        }
        finally {
            $sourceStream.Dispose()
        }
        $targetStream = [IO.File]::OpenRead($TargetPath)
        try {
            $targetHash = [BitConverter]::ToString(
                $sha256.ComputeHash($targetStream)).Replace('-', '')
        }
        finally {
            $targetStream.Dispose()
        }
    }
    finally {
        $sha256.Dispose()
    }
    if ($sourceHash -cne $targetHash) {
        throw "ファイルハッシュが一致しません: $SourcePath / $TargetPath"
    }
    return $sourceHash
}

$null = Update-HostStorageCache
$deadline = (Get-Date).AddSeconds(30)
do {
    $targetDisk = Get-Disk -Number $TargetDiskNumber
    if ($targetDisk.PartitionStyle -eq 'GPT' -and -not $targetDisk.IsOffline) {
        break
    }
    Start-Sleep -Seconds 1
    $null = Update-HostStorageCache
} while ((Get-Date) -lt $deadline)

$sourceDisk = Get-Disk -Number $SourceDiskNumber
$targetDisk = Get-Disk -Number $TargetDiskNumber
if ($sourceDisk.IsSystem -or $sourceDisk.IsBoot -or
    $targetDisk.IsSystem -or $targetDisk.IsBoot) {
    throw '合成データディスク以外が検証対象になっています。'
}
if ($sourceDisk.PartitionStyle -ne 'GPT' -or
    $targetDisk.PartitionStyle -ne 'GPT' -or
    $targetDisk.IsOffline) {
    throw 'コピー元/コピー先のGPTまたはonline状態が不正です。'
}
if ($sourceDisk.Guid -eq $targetDisk.Guid) {
    throw 'コピー先Disk GUIDが再生成されていません。'
}

$efiType = '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}'
$msrType = '{e3c9e316-0b5c-4db8-817d-f92df00215ae}'
$basicType = '{ebd0a0a2-b9e5-4433-87c0-68b6b72699c7}'
$recoveryType = '{de94bba4-06d1-4d40-a16a-bfd50179d6ac}'

$sourceEfi = Get-SinglePartitionByType -DiskNumber $SourceDiskNumber -GptType $efiType
$targetEfi = Get-SinglePartitionByType -DiskNumber $TargetDiskNumber -GptType $efiType
$sourceMsr = Get-SinglePartitionByType -DiskNumber $SourceDiskNumber -GptType $msrType
$targetMsr = Get-SinglePartitionByType -DiskNumber $TargetDiskNumber -GptType $msrType
$sourceData = Get-SinglePartitionByType -DiskNumber $SourceDiskNumber -GptType $basicType
$targetData = Get-SinglePartitionByType -DiskNumber $TargetDiskNumber -GptType $basicType
$sourceRecovery = Get-SinglePartitionByType -DiskNumber $SourceDiskNumber -GptType $recoveryType
$targetRecovery = Get-SinglePartitionByType -DiskNumber $TargetDiskNumber -GptType $recoveryType

$sourcePartitions = @($sourceEfi, $sourceMsr, $sourceData, $sourceRecovery)
$targetPartitions = @($targetEfi, $targetMsr, $targetData, $targetRecovery)
foreach ($sourcePartition in $sourcePartitions) {
    if ($targetPartitions.Guid -contains $sourcePartition.Guid) {
        throw "コピー先Partition GUIDが再生成されていません: $($sourcePartition.Guid)"
    }
}
for ($index = 0; $index -lt $sourcePartitions.Count; $index++) {
    if ($sourcePartitions[$index].Offset -ne $targetPartitions[$index].Offset -or
        $sourcePartitions[$index].Size -ne $targetPartitions[$index].Size) {
        throw "パーティション位置またはサイズが一致しません: index=$index"
    }
}

$sourceEfiRoot = Get-VolumeRoot -Partition $sourceEfi
$targetEfiRoot = Get-VolumeRoot -Partition $targetEfi
$sourceDataRoot = Get-VolumeRoot -Partition $sourceData
$targetDataRoot = Get-VolumeRoot -Partition $targetData
$sourceRecoveryRoot = Get-VolumeRoot -Partition $sourceRecovery
$targetRecoveryRoot = Get-VolumeRoot -Partition $targetRecovery

$efiHash = Compare-FileHash `
    -SourcePath ([IO.Path]::Combine($sourceEfiRoot, 'ydc-efi-marker.txt')) `
    -TargetPath ([IO.Path]::Combine($targetEfiRoot, 'ydc-efi-marker.txt'))
$dataMarkerHash = Compare-FileHash `
    -SourcePath ([IO.Path]::Combine($sourceDataRoot, 'ydc-ntfs-marker.txt')) `
    -TargetPath ([IO.Path]::Combine($targetDataRoot, 'ydc-ntfs-marker.txt'))
$payloadHash = Compare-FileHash `
    -SourcePath ([IO.Path]::Combine($sourceDataRoot, 'ydc-payload.bin')) `
    -TargetPath ([IO.Path]::Combine($targetDataRoot, 'ydc-payload.bin'))
$recoveryHash = Compare-FileHash `
    -SourcePath ([IO.Path]::Combine($sourceRecoveryRoot, 'ydc-recovery-marker.txt')) `
    -TargetPath ([IO.Path]::Combine($targetRecoveryRoot, 'ydc-recovery-marker.txt'))

$result = [ordered]@{
    schemaVersion = 1
    result = 'PASS'
    sourceDiskNumber = $SourceDiskNumber
    targetDiskNumber = $TargetDiskNumber
    diskGuidRegenerated = $true
    partitionGuidsRegenerated = $true
    partitionCount = $targetPartitions.Count
    efiMarkerSha256 = $efiHash
    ntfsMarkerSha256 = $dataMarkerHash
    ntfsPayloadSha256 = $payloadHash
    recoveryMarkerSha256 = $recoveryHash
} | ConvertTo-Json -Compress
[IO.File]::WriteAllText($ResultPath, $result, [Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText($DonePath, 'PASS', [Text.UTF8Encoding]::new($false))
$result
