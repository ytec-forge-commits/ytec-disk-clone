param(
    [Parameter(Mandatory)]
    [string]$OutputRoot,

    [ValidateSet('2011CA', '2023CA')]
    [string]$CertificateGeneration = '2023CA',

    [ValidateSet('Inventory')]
    [string]$ValidationScenario = 'Inventory',

    [string]$DiagnosticPath = '',

    [string]$WinPEAppPath = '',

    [string]$WinPEGuiPath = '',

    [string]$FinalIsoPath = '',

    [ValidatePattern('^$|^[A-Za-z]:$')]
    [string]$TargetUsbDrive = '',

    [int]$ExpectedUsbDiskNumber = -1,

    [UInt64]$ExpectedUsbSizeBytes = 0,

    [string]$ExpectedUsbSerialSuffix = '',

    [string]$ExpectedUsbDeviceInstanceId = '',

    [ValidatePattern('^$|^[0-9A-Fa-f]{64}$')]
    [string]$ExpectedUsbCanonicalLayoutSha256 = '',

    [ValidateSet('', 'Initialize', 'Refresh')]
    [string]$UsbOperation = '',

    [ValidateSet('', 'NTFS', 'exFAT')]
    [string]$UsbDataFileSystem = '',

    [switch]$BuildUsb,

    [switch]$BuildMedia
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$OutputEncoding = [Text.UTF8Encoding]::new($false)

$script:RescueUsbMinimumBytes = [UInt64](8GB)
$script:RescueUsbBootPartitionBytes = [UInt64](4GB)
$script:MaximumBootFileCount = 65536
$script:MaximumBootPathCharacters = [UInt64](8MB)
$script:MaximumBootLogicalBytes = [UInt64](4GB)
$script:MaximumDataFileCount = 262144
$script:MaximumDataPathCharacters = [UInt64](64MB)
$script:MaximumDataLogicalBytes = [UInt64](2TB)
$script:RescueUsbOwnershipPurpose =
    'Y-TEC Tsumugi Drive rescue USB ownership'
$script:RescueUsbMarkerRelativePath =
    'YtecDiskClone\rescue-media-id.txt'
$script:RescueUsbManifestRelativePath =
    'YtecDiskClone\rescue-media-manifest.json'
$script:RescueUsbTransactionRelativePath =
    '.ytec-rescue-transaction'
$script:MaximumOwnershipManifestBytes = [UInt64](64MB)

function Assert-ExternalNewOutputPath {
    param(
        [Parameter(Mandatory)]
        [string]$RepositoryRoot,
        [Parameter(Mandatory)]
        [string]$CandidatePath
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
        [Parameter(Mandatory)]
        [string]$Path,
        [Parameter(Mandatory)]
        [string]$Description
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Description が見つかりません: $Path"
    }
    $item = Get-Item -LiteralPath $Path -Force
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "$Description のreparse pointは使用しません: $Path"
    }
}

function Assert-MicrosoftSignature {
    param(
        [Parameter(Mandatory)]
        [string]$Path,
        [Parameter(Mandatory)]
        [string]$Description
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

function Copy-VerifiedMountedWimEfiFile {
    param(
        [Parameter(Mandatory)]
        [string]$MountRoot,

        [Parameter(Mandatory)]
        [string]$SourcePath,

        [Parameter(Mandatory)]
        [string]$DestinationPath,

        [Parameter(Mandatory)]
        [string]$FsutilPath,

        [Parameter(Mandatory)]
        [string]$Description
    )

    $mountFullPath = [IO.Path]::GetFullPath($MountRoot).TrimEnd('\')
    $mountPrefix = $mountFullPath + '\'
    $sourceFullPath = [IO.Path]::GetFullPath($SourcePath)
    if (-not $sourceFullPath.StartsWith(
            $mountPrefix,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "$Description が固定WIMマウント先の外側です: $sourceFullPath"
    }
    if (-not (Test-Path -LiteralPath $sourceFullPath -PathType Leaf)) {
        throw "$Description が見つかりません: $sourceFullPath"
    }

    $ancestor = Split-Path -Parent $sourceFullPath
    while ($ancestor.StartsWith(
            $mountPrefix,
            [StringComparison]::OrdinalIgnoreCase) -or
        $ancestor.Equals(
            $mountFullPath,
            [StringComparison]::OrdinalIgnoreCase)) {
        $ancestorItem = Get-Item -LiteralPath $ancestor -Force
        if (($ancestorItem.Attributes -band
                [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "$Description の親フォルダーにreparse pointがあります: $ancestor"
        }
        if ($ancestor.Equals(
                $mountFullPath,
                [StringComparison]::OrdinalIgnoreCase)) {
            break
        }
        $ancestor = Split-Path -Parent $ancestor
    }

    $sourceItem = Get-Item -LiteralPath $sourceFullPath -Force
    $sourceIsWimProjection = ($sourceItem.Attributes -band
            [IO.FileAttributes]::ReparsePoint) -ne 0
    if ($sourceIsWimProjection) {
        # DISMでマウントしたWIMは通常ファイルにもIO_REPARSE_TAG_WIMを付ける。
        # 任意のリンクは許可せず、Windows SDK定義のWIMタグだけを許可する。
        $reparseOutput = @(
            & $FsutilPath reparsepoint query $sourceFullPath 2>&1
        )
        $reparseExitCode = $LASTEXITCODE
        $reparseText = $reparseOutput -join [Environment]::NewLine
        if ($reparseExitCode -ne 0 -or
            $reparseText -notmatch '(?i)\b0x80000008\b') {
            throw "$Description が許可済みWIM投影ではありません: $sourceFullPath"
        }
    }

    if ($sourceItem.Length -lt 64KB -or $sourceItem.Length -gt 16MB) {
        throw "$Description のサイズが許可範囲外です: $($sourceItem.Length)"
    }
    if (Test-Path -LiteralPath $DestinationPath) {
        throw "$Description の検証用コピー先を上書きしません: $DestinationPath"
    }

    $sourceHashBefore = (Get-FileHash -LiteralPath $sourceFullPath `
        -Algorithm SHA256).Hash
    Copy-Item -LiteralPath $sourceFullPath -Destination $DestinationPath
    Assert-RegularNonReparseFile `
        -Path $DestinationPath `
        -Description "$Description の検証用コピー"

    $destinationItem = Get-Item -LiteralPath $DestinationPath -Force
    $bytes = [IO.File]::ReadAllBytes($destinationItem.FullName)
    if ($bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) {
        throw "$Description にDOS MZ署名がありません。"
    }
    $peOffset = [BitConverter]::ToInt32($bytes, 0x3C)
    if ($peOffset -lt 0x40 -or $peOffset -gt ($bytes.Length - 94) -or
        $bytes[$peOffset] -ne 0x50 -or
        $bytes[$peOffset + 1] -ne 0x45 -or
        $bytes[$peOffset + 2] -ne 0 -or
        $bytes[$peOffset + 3] -ne 0) {
        throw "$Description のPEヘッダーが不正です。"
    }
    $machine = [BitConverter]::ToUInt16($bytes, $peOffset + 4)
    $optionalHeaderMagic = [BitConverter]::ToUInt16($bytes, $peOffset + 24)
    $subsystem = [BitConverter]::ToUInt16($bytes, $peOffset + 92)
    if ($machine -ne 0x8664 -or
        $optionalHeaderMagic -ne 0x020B -or
        $subsystem -ne 10) {
        throw ("$Description はAMD64 PE32+ EFI Applicationではありません: " +
            ('machine=0x{0:X4}, optionalMagic=0x{1:X4}, subsystem={2}' -f
                $machine, $optionalHeaderMagic, $subsystem))
    }
    Assert-MicrosoftSignature `
        -Path $destinationItem.FullName `
        -Description "$Description の検証用コピー"

    $sourceHashAfter = (Get-FileHash -LiteralPath $sourceFullPath `
        -Algorithm SHA256).Hash
    $destinationHash = (Get-FileHash -LiteralPath $destinationItem.FullName `
        -Algorithm SHA256).Hash
    if ($sourceHashBefore -ne $sourceHashAfter -or
        $sourceHashBefore -ne $destinationHash) {
        throw "$Description のコピー中にSHA-256が変化しました。"
    }

    return [ordered]@{
        sourceRelativePath = $sourceFullPath.Substring(
            $mountFullPath.Length).TrimStart('\')
        length = $destinationItem.Length
        sha256 = $destinationHash
        sourceWasWimProjection = $sourceIsWimProjection
        reparseTag = if ($sourceIsWimProjection) { '0x80000008' } else { '' }
        machine = 'AMD64'
        optionalHeader = 'PE32+'
        subsystem = 'EFI Application'
        microsoftSignatureVerified = $true
        trustAnchor = 'SHA-256-recorded ADK WinPE WIM plus materialized file verification'
    }
}

function Get-WinPEAppPeReport {
    param(
        [Parameter(Mandatory)]
        [string]$Path,

        [Parameter(Mandatory)]
        [string[]]$AllowedDependencies,

        [Parameter(Mandatory)]
        [string]$Description
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
    if ($peOffset -lt 0x40 -or $peOffset -gt ($bytes.Length - 26)) {
        throw "$Description のPEヘッダー位置が不正です。"
    }
    if ($bytes[$peOffset] -ne 0x50 -or
        $bytes[$peOffset + 1] -ne 0x45 -or
        $bytes[$peOffset + 2] -ne 0 -or
        $bytes[$peOffset + 3] -ne 0) {
        throw "$Description にPE署名がありません。"
    }

    $machine = [BitConverter]::ToUInt16($bytes, $peOffset + 4)
    $optionalHeaderMagic = [BitConverter]::ToUInt16($bytes, $peOffset + 24)
    if ($machine -ne 0x8664 -or $optionalHeaderMagic -ne 0x020B) {
        throw ("$Description はAMD64 PE32+でなければなりません: " +
            ('machine=0x{0:X4}, optionalMagic=0x{1:X4}' -f
                $machine, $optionalHeaderMagic))
    }

    $ascii = [Text.Encoding]::ASCII.GetString($bytes)
    $dependencies = @(
        [regex]::Matches($ascii, '(?i)[A-Za-z0-9._-]+\.dll') |
            ForEach-Object Value |
            Sort-Object -Unique
    )
    $unexpected = @(
        $dependencies |
            Where-Object { $AllowedDependencies -notcontains $_ }
    )
    $missing = @(
        $AllowedDependencies |
            Where-Object { $dependencies -notcontains $_ }
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
        [Parameter(Mandatory)]
        [string]$Command,
        [Parameter(Mandatory)]
        [string[]]$Arguments,
        [Parameter(Mandatory)]
        [string]$Operation
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

function Write-MediaProgress {
    param(
        [Parameter(Mandatory)]
        [ValidateRange(0, 100)]
        [int]$Percent,

        [Parameter(Mandatory)]
        [ValidatePattern('^[a-z0-9-]+$')]
        [string]$Stage
    )

    Write-Output ("TSUMUGI_MEDIA_PROGRESS={0}|{1}" -f $Percent, $Stage)
}

function Get-NormalizedSerialSuffix {
    param([AllowNull()][string]$SerialNumber)

    if ([string]::IsNullOrWhiteSpace($SerialNumber)) {
        return ''
    }
    $printable = -join @(
        $SerialNumber.ToCharArray() |
            Where-Object {
                [int]$_ -ge 0x20 -and [int]$_ -le 0x7E
            }
    )
    $trimmed = $printable.Trim()
    if ($trimmed.Length -le 8) {
        return $trimmed
    }
    return $trimmed.Substring($trimmed.Length - 8)
}

function Get-VerifiedUsbDisk {
    param(
        [Parameter(Mandatory)]
        [ValidateRange(0, [int]::MaxValue)]
        [int]$DiskNumber,
        [Parameter(Mandatory)]
        [UInt64]$SizeBytes,
        [AllowEmptyString()]
        [string]$SerialSuffix,
        [Parameter(Mandatory)]
        [string]$DeviceInstanceId
    )

    $disk = Get-Disk -Number $DiskNumber -ErrorAction Stop
    if ([UInt64]$disk.Size -ne $SizeBytes) {
        throw 'USBの容量が確認時から変化しました。'
    }
    if ([string]$disk.BusType -ne 'USB' -or
        [bool]$disk.IsSystem -or [bool]$disk.IsBoot -or
        [bool]$disk.IsReadOnly -or [bool]$disk.IsOffline) {
        throw '選択先がオンライン・書込み可能・非システムのUSBではありません。'
    }
    $operationalStatuses = @(
        $disk.OperationalStatus | ForEach-Object { [string]$_ }
    )
    if ($operationalStatuses -notcontains 'Online') {
        throw '選択USBがオンライン状態ではありません。'
    }

    $cimDisks = @(
        Get-CimInstance -ClassName Win32_DiskDrive -ErrorAction Stop |
            Where-Object { [int]$_.Index -eq $DiskNumber }
    )
    if ($cimDisks.Count -ne 1) {
        throw '選択USBのデバイス識別情報を一意に再取得できません。'
    }
    $observedDeviceId = [string]$cimDisks[0].PNPDeviceID
    if (-not $observedDeviceId.Equals(
            $DeviceInstanceId,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw 'USBのデバイス識別情報が確認時から変化しました。'
    }
    $observedSuffix = Get-NormalizedSerialSuffix `
        -SerialNumber ([string]$cimDisks[0].SerialNumber)
    if (-not [string]::IsNullOrEmpty($SerialSuffix) -and
        $observedSuffix -ne $SerialSuffix) {
        throw 'USBのシリアル末尾が確認時から変化しました。'
    }

    return [ordered]@{
        disk = $disk
        diskNumber = $DiskNumber
        sizeBytes = $SizeBytes
        serialSuffix = $SerialSuffix
        deviceInstanceId = $observedDeviceId
        partitionStyle = [string]$disk.PartitionStyle
    }
}

function Get-UsbPartitionsAllowEmpty {
    param(
        [Parameter(Mandatory)]
        [ValidateRange(0, [int]::MaxValue)]
        [int]$DiskNumber,
        [Parameter(Mandatory)]
        [UInt64]$SizeBytes,
        [AllowEmptyString()]
        [string]$SerialSuffix,
        [Parameter(Mandatory)]
        [string]$DeviceInstanceId
    )

    try {
        return @(Get-Partition -DiskNumber $DiskNumber -ErrorAction Stop)
    } catch {
        if ([string]$_.FullyQualifiedErrorId -notlike
            'CmdletizationQuery_NotFound_DiskNumber,Get-Partition*') {
            throw
        }

        # Some Storage providers report an empty initialized disk as an
        # ObjectNotFound error instead of returning an empty partition list.
        # Re-verify the stable USB identity before treating that result as empty.
        Get-VerifiedUsbDisk `
            -DiskNumber $DiskNumber `
            -SizeBytes $SizeBytes `
            -SerialSuffix $SerialSuffix `
            -DeviceInstanceId $DeviceInstanceId | Out-Null
        return @()
    }
}

function Test-UsbDriveLetterAvailable {
    param(
        [Parameter(Mandatory)]
        [ValidatePattern('^[A-Za-z]:$')]
        [string]$Drive
    )

    $driveLetter = $Drive.Substring(0, 1).ToUpperInvariant()
    $root = "$driveLetter`:\"
    if ($null -ne (Get-PSDrive `
            -Name $driveLetter `
            -PSProvider FileSystem `
            -ErrorAction SilentlyContinue) -or
        (Test-Path -LiteralPath $root)) {
        return $false
    }
    $volumes = @(
        Get-Volume -ErrorAction Stop | Where-Object {
            [string]$_.DriveLetter -eq $driveLetter
        }
    )
    return $volumes.Count -eq 0
}

function Select-UsbDriveLetter {
    param(
        [Parameter(Mandatory)]
        [ValidatePattern('^[A-Za-z]:$')]
        [string]$PreferredDrive
    )

    $preferredLetter = $PreferredDrive.Substring(0, 1).ToUpperInvariant()
    $candidates = @($preferredLetter)
    foreach ($code in [int][char]'D'..[int][char]'Z') {
        $candidate = [char]$code
        if ([string]$candidate -ne $preferredLetter) {
            $candidates += [string]$candidate
        }
    }
    foreach ($candidate in $candidates) {
        $drive = "$candidate`:"
        if (Test-UsbDriveLetterAvailable -Drive $drive) {
            return $drive
        }
    }
    throw 'レスキューUSBへ割り当て可能な未使用ドライブ文字がありません。'
}

function Get-CanonicalUsbLayout {
    param(
        [Parameter(Mandatory)]
        [ValidateRange(0, [int]::MaxValue)]
        [int]$DiskNumber,
        [Parameter(Mandatory)]
        [UInt64]$SizeBytes,
        [AllowEmptyString()]
        [string]$SerialSuffix,
        [Parameter(Mandatory)]
        [string]$DeviceInstanceId
    )

    $verifiedDisk = Get-VerifiedUsbDisk `
        -DiskNumber $DiskNumber `
        -SizeBytes $SizeBytes `
        -SerialSuffix $SerialSuffix `
        -DeviceInstanceId $DeviceInstanceId
    $partitions = @(
        Get-UsbPartitionsAllowEmpty `
            -DiskNumber $DiskNumber `
            -SizeBytes $SizeBytes `
            -SerialSuffix $SerialSuffix `
            -DeviceInstanceId $DeviceInstanceId |
            Sort-Object -Property PartitionNumber
    )
    $entries = @(
        foreach ($partition in $partitions) {
            $type = if ($null -ne $partition.PSObject.Properties['MbrType']) {
                [string]$partition.MbrType
            } elseif ($null -ne $partition.PSObject.Properties['GptType']) {
                [string]$partition.GptType
            } elseif ($null -ne $partition.PSObject.Properties['Type']) {
                [string]$partition.Type
            } else {
                ''
            }
            [ordered]@{
                number = [int]$partition.PartitionNumber
                style = ([string]$verifiedDisk.partitionStyle).ToUpperInvariant()
                type = $type.ToUpperInvariant()
                offsetBytes = [UInt64]$partition.Offset
                sizeBytes = [UInt64]$partition.Size
                bootable = [bool]$partition.IsActive
            }
        }
    )
    $value = [ordered]@{
        diskStyle = ([string]$verifiedDisk.partitionStyle).ToUpperInvariant()
        partitions = $entries
    }
    return [ordered]@{
        disk = $verifiedDisk.disk
        partitions = $partitions
        value = $value
        canonical = ($value | ConvertTo-Json -Depth 8 -Compress)
    }
}

function Get-CanonicalUsbLayoutDigest {
    param([Parameter(Mandatory)]$LayoutValue)

    $stream = [IO.MemoryStream]::new()
    $writer = [IO.BinaryWriter]::new(
        $stream,
        [Text.Encoding]::UTF8,
        $true)
    try {
        $domain = [Text.Encoding]::UTF8.GetBytes(
            'ytec-rescue-usb-canonical-layout-v1')
        $writer.Write([UInt64]$domain.Length)
        $writer.Write($domain)
        $styleValue = switch (
            ([string]$LayoutValue.diskStyle).ToUpperInvariant()) {
            'RAW' { [byte]0; break }
            'MBR' { [byte]1; break }
            'GPT' { [byte]2; break }
            default { [byte]3; break }
        }
        $writer.Write([byte]$styleValue)
        # Windows PowerShell 5.1 sorts OrderedDictionary property-name keys
        # differently from PowerShell 7.  Explicit typed expressions keep the
        # wire order identical to the C++ canonical-layout contract.
        $partitions = @($LayoutValue.partitions |
            Sort-Object -Property `
                @{ Expression = { [UInt32]$_.number }; Ascending = $true },
                @{ Expression = { [string]$_.style }; Ascending = $true },
                @{ Expression = { [string]$_.type }; Ascending = $true },
                @{ Expression = { [UInt64]$_.offsetBytes }; Ascending = $true },
                @{ Expression = { [UInt64]$_.sizeBytes }; Ascending = $true },
                @{ Expression = { [bool]$_.bootable }; Ascending = $true })
        $writer.Write([UInt64]$partitions.Count)
        foreach ($partition in $partitions) {
            $writer.Write([UInt32]$partition.number)
            $partitionStyleValue = switch (
                ([string]$partition.style).ToUpperInvariant()) {
                'RAW' { [byte]0; break }
                'MBR' { [byte]1; break }
                'GPT' { [byte]2; break }
                default { [byte]3; break }
            }
            $writer.Write([byte]$partitionStyleValue)
            $type = ([string]$partition.type).ToUpperInvariant()
            $writer.Write([UInt64]$type.Length)
            foreach ($character in $type.ToCharArray()) {
                $writer.Write([UInt32][char]$character)
            }
            $writer.Write([UInt64]$partition.offsetBytes)
            $writer.Write([UInt64]$partition.sizeBytes)
            $bootableValue = if ([bool]$partition.bootable) {
                [byte]1
            } else {
                [byte]0
            }
            $writer.Write([byte]$bootableValue)
        }
        $writer.Flush()
        $hasher = [Security.Cryptography.SHA256]::Create()
        try {
            return ([BitConverter]::ToString(
                $hasher.ComputeHash($stream.ToArray()))).Replace('-', '')
        } finally {
            $hasher.Dispose()
        }
    } finally {
        $writer.Dispose()
        $stream.Dispose()
    }
}

function Assert-UsbIdentityAndLayout {
    param(
        [Parameter(Mandatory)]
        [ValidateRange(0, [int]::MaxValue)]
        [int]$DiskNumber,
        [Parameter(Mandatory)]
        [UInt64]$SizeBytes,
        [AllowEmptyString()]
        [string]$SerialSuffix,
        [Parameter(Mandatory)]
        [string]$DeviceInstanceId,
        [Parameter(Mandatory)]
        [string]$ExpectedCanonicalLayout,
        [Parameter(Mandatory)]
        [string]$Boundary
    )

    $observed = Get-CanonicalUsbLayout `
        -DiskNumber $DiskNumber `
        -SizeBytes $SizeBytes `
        -SerialSuffix $SerialSuffix `
        -DeviceInstanceId $DeviceInstanceId
    if ($observed.canonical -cne $ExpectedCanonicalLayout) {
        throw "$Boundary の直前にUSBの完全パーティションレイアウトが変化しました。"
    }
    return $observed
}

function Assert-SafeMediaRelativePath {
    param(
        [Parameter(Mandatory)]
        [string]$RelativePath
    )

    if ([string]::IsNullOrWhiteSpace($RelativePath) -or
        $RelativePath.Length -ge 32768 -or
        [IO.Path]::IsPathRooted($RelativePath) -or
        $RelativePath.Contains('/') -or
        $RelativePath.StartsWith('\') -or
        $RelativePath.EndsWith('\')) {
        throw "媒体内の相対パスが不正です: $RelativePath"
    }
    foreach ($component in $RelativePath.Split('\')) {
        if ([string]::IsNullOrWhiteSpace($component) -or
            $component -eq '.' -or $component -eq '..' -or
            $component.EndsWith('.') -or $component.EndsWith(' ') -or
            $component -match '[\x00-\x1F<>:"|?*]' -or
            $component -match
                '(?i)^(CON|PRN|AUX|NUL|CLOCK\$|CONIN\$|CONOUT\$|COM[1-9]|LPT[1-9])(?:\.|$)') {
            throw "媒体内にWindowsで安全に扱えない名前があります: $RelativePath"
        }
    }
}

function Get-BoundedMediaTreeManifest {
    param(
        [Parameter(Mandatory)]
        [string]$Root,
        [string[]]$ExcludedRelativePaths = @(),
        [Parameter(Mandatory)]
        [ValidateRange(1, [int]::MaxValue)]
        [int]$MaximumFileCount,
        [Parameter(Mandatory)]
        [UInt64]$MaximumPathCharacters,
        [Parameter(Mandatory)]
        [UInt64]$MaximumLogicalBytes,
        [Parameter(Mandatory)]
        [ValidateSet('FAT32', 'NTFS', 'exFAT')]
        [string]$FileSystem,
        [string]$FsutilPath = '',
        [string[]]$ExcludedRelativePathPrefixes = @(),
        [switch]$PrivacyPreservingSummary,
        [switch]$RedactPaths
    )

    $rootFullPath = [IO.Path]::GetFullPath($Root).TrimEnd('\')
    $rootItem = Get-Item -LiteralPath $rootFullPath -Force
    if (-not $rootItem.PSIsContainer -or
        ($rootItem.Attributes -band
            [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "媒体ツリーのルートが通常フォルダーではありません: $rootFullPath"
    }
    $excluded = @{}
    foreach ($relativePath in $ExcludedRelativePaths) {
        Assert-SafeMediaRelativePath -RelativePath $relativePath
        $excluded[$relativePath.ToUpperInvariant()] = $true
    }
    $excludedPrefixes = @(
        foreach ($relativePath in $ExcludedRelativePathPrefixes) {
            Assert-SafeMediaRelativePath -RelativePath $relativePath
            $relativePath.ToUpperInvariant().TrimEnd('\') + '\'
        }
    )

    $files = [Collections.Generic.List[object]]::new()
    $directories = [Collections.Generic.List[string]]::new()
    $pending = [Collections.Generic.Stack[string]]::new()
    $pending.Push($rootFullPath)
    [UInt64]$totalPathCharacters = 0
    [UInt64]$totalLogicalBytes = 0
    [UInt64]$entryCount = 0
    while ($pending.Count -ne 0) {
        $directory = $pending.Pop()
        foreach ($item in @(Get-ChildItem -LiteralPath $directory -Force)) {
            $relativePath = $item.FullName.Substring(
                $rootFullPath.Length).TrimStart('\')
            try {
                Assert-SafeMediaRelativePath -RelativePath $relativePath
            } catch {
                if ($RedactPaths) {
                    throw '媒体ツリーにWindowsで安全に扱えない名前があります。'
                }
                throw
            }
            $displayPath = if ($RedactPaths) {
                '[redacted]'
            } else {
                $relativePath
            }
            $normalizedPath = $relativePath.ToUpperInvariant()
            if ($excluded.ContainsKey($normalizedPath) -or
                @($excludedPrefixes | Where-Object {
                    $normalizedPath.StartsWith(
                        $_,
                        [StringComparison]::OrdinalIgnoreCase)
                }).Count -ne 0) {
                continue
            }
            if ($entryCount -ge [UInt64]$MaximumFileCount) {
                throw '媒体ツリーの列挙件数が安全上限を超えています。'
            }
            [UInt64]$pathCharacters = $relativePath.Length
            if ($pathCharacters -gt
                    ($MaximumPathCharacters - $totalPathCharacters)) {
                throw '媒体ツリーの総パス長が安全上限を超えています。'
            }
            $entryCount++
            $totalPathCharacters += $pathCharacters
            if (($item.Attributes -band
                    [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "媒体ツリーにreparse pointがあります: $displayPath"
            }
            if ($item.PSIsContainer) {
                $directories.Add($normalizedPath)
                $pending.Push($item.FullName)
                continue
            }
            $linkTypeProperty = $item.PSObject.Properties['LinkType']
            if ($null -ne $linkTypeProperty -and
                [string]$linkTypeProperty.Value -eq 'HardLink') {
                throw "媒体ツリーにhardlinkがあります: $displayPath"
            }
            if ($FileSystem -eq 'NTFS') {
                if ([string]::IsNullOrWhiteSpace($FsutilPath)) {
                    throw 'NTFSファイルのhardlink確認に署名済みFsutilが必要です。'
                }
                $hardLinks = @(
                    & $FsutilPath hardlink list $item.FullName 2>&1
                )
                if ($LASTEXITCODE -ne 0) {
                    throw "NTFSファイルのhardlink数を確認できません: $displayPath"
                }
                $hardLinkPaths = @(
                    $hardLinks | Where-Object {
                        ([string]$_).TrimStart().StartsWith('\')
                    }
                )
                if ($hardLinkPaths.Count -ne 1) {
                    throw "媒体ツリーにhardlinkまたは曖昧なリンク情報があります: $displayPath"
                }
            }
            [UInt64]$length = $item.Length
            if ($length -gt
                    ($MaximumLogicalBytes - $totalLogicalBytes)) {
                throw '媒体ツリーの総論理バイト数が安全上限を超えています。'
            }
            $totalLogicalBytes += $length
            $files.Add([ordered]@{
                relativePath = $normalizedPath
                length = $length
                sha256 = (Get-FileHash -LiteralPath $item.FullName `
                    -Algorithm SHA256).Hash
            })
        }
    }

    $sortedFiles = @($files | Sort-Object -Property relativePath)
    $sortedDirectories = @($directories | Sort-Object -Unique)
    if (@($sortedFiles.relativePath | Select-Object -Unique).Count -ne
        $sortedFiles.Count) {
        throw '媒体ツリーに大小文字違いを含む重複パスがあります。'
    }
    $canonicalLines = [Collections.Generic.List[string]]::new()
    $canonicalLines.Add('YTEC-RESCUE-MEDIA-TREE-V1')
    $canonicalLines.Add(
        "C:${entryCount}:${totalPathCharacters}:${totalLogicalBytes}")
    foreach ($relativePath in $sortedDirectories) {
        $canonicalLines.Add("D:$($relativePath.Length):$relativePath")
    }
    foreach ($file in $sortedFiles) {
        $canonicalLines.Add(
            "F:$($file.relativePath.Length):$($file.relativePath):$($file.length):$($file.sha256)")
    }
    $canonicalBytes = [Text.Encoding]::UTF8.GetBytes(
        ($canonicalLines -join "`n"))
    $hasher = [Security.Cryptography.SHA256]::Create()
    try {
        $rootDigest = ([BitConverter]::ToString(
            $hasher.ComputeHash($canonicalBytes))).Replace('-', '')
    } finally {
        $hasher.Dispose()
    }
    $summary = [ordered]@{
        entryCount = $entryCount
        fileCount = $sortedFiles.Count
        totalPathCharacters = $totalPathCharacters
        totalLogicalBytes = $totalLogicalBytes
        rootDigest = $rootDigest
    }
    if ($PrivacyPreservingSummary) {
        return $summary
    }
    return [ordered]@{
        entryCount = $summary.entryCount
        fileCount = $summary.fileCount
        totalPathCharacters = $summary.totalPathCharacters
        totalLogicalBytes = $summary.totalLogicalBytes
        rootDigest = $summary.rootDigest
        files = $sortedFiles
        directories = $sortedDirectories
    }
}

function Assert-MediaTreeManifestEqual {
    param(
        [Parameter(Mandatory)]$Expected,
        [Parameter(Mandatory)]$Observed,
        [Parameter(Mandatory)][string]$Description
    )

    $expectedJson = $Expected | ConvertTo-Json -Depth 8 -Compress
    $observedJson = $Observed | ConvertTo-Json -Depth 8 -Compress
    if ($expectedJson -cne $observedJson) {
        throw "$Description のパス、長さ、SHA-256、またはディレクトリ構成が変化しました。"
    }
}

function Get-VerifiedUsbTarget {
    param(
        [Parameter(Mandatory)]
        [ValidatePattern('^[A-Za-z]:$')]
        [string]$Drive,
        [Parameter(Mandatory)]
        [ValidateRange(0, [int]::MaxValue)]
        [int]$DiskNumber,
        [Parameter(Mandatory)]
        [UInt64]$SizeBytes,
        [AllowEmptyString()]
        [string]$SerialSuffix,
        [Parameter(Mandatory)]
        [string]$DeviceInstanceId,
        [Parameter(Mandatory)]
        [ValidateSet('Initialize', 'Refresh')]
        [string]$Operation,
        [Parameter(Mandatory)]
        [ValidateSet('NTFS', 'exFAT')]
        [string]$DataFileSystem
    )

    if ($SizeBytes -lt $script:RescueUsbMinimumBytes) {
        throw 'レスキューUSBは8GiB以上が必要です。'
    }
    $verifiedDisk = Get-VerifiedUsbDisk `
        -DiskNumber $DiskNumber `
        -SizeBytes $SizeBytes `
        -SerialSuffix $SerialSuffix `
        -DeviceInstanceId $DeviceInstanceId
    if ([UInt64]$verifiedDisk.disk.LogicalSectorSize -eq 0 -or
        ($SizeBytes / [UInt64]$verifiedDisk.disk.LogicalSectorSize) -gt
            [UInt64][UInt32]::MaxValue) {
        throw '選択USBはBIOS互換MBRで安全に表現できる容量ではありません。'
    }
    $allowedStyles = if ($Operation -eq 'Refresh') {
        @('MBR')
    } else {
        @('RAW', 'MBR', 'GPT')
    }
    if ($allowedStyles -notcontains $verifiedDisk.partitionStyle) {
        throw '選択USBは初期化可能なRAW／GPT／MBR構成ではありません。'
    }

    $driveLetter = $Drive.Substring(0, 1).ToUpperInvariant()
    $partitions = @(
        Get-UsbPartitionsAllowEmpty `
            -DiskNumber $DiskNumber `
            -SizeBytes $SizeBytes `
            -SerialSuffix $SerialSuffix `
            -DeviceInstanceId $DeviceInstanceId
    )
    if ($verifiedDisk.partitionStyle -eq 'RAW' -and
        $partitions.Count -ne 0) {
        throw 'RAWとして列挙されたUSBにパーティションがあるため停止しました。'
    }
    $layout = Get-CanonicalUsbLayout `
        -DiskNumber $DiskNumber `
        -SizeBytes $SizeBytes `
        -SerialSuffix $SerialSuffix `
        -DeviceInstanceId $DeviceInstanceId
    if ($Operation -eq 'Initialize') {
        $selectedDrive = Select-UsbDriveLetter -PreferredDrive $Drive
        return [ordered]@{
            disk = $verifiedDisk.disk
            diskNumber = $DiskNumber
            drive = $selectedDrive
            root = $null
            dataDrive = $null
            dataRoot = $null
            sizeBytes = $SizeBytes
            serialSuffix = $SerialSuffix
            deviceInstanceId = $verifiedDisk.deviceInstanceId
            partitionStyle = $verifiedDisk.partitionStyle
            partitionNumber = 0
            partitions = $partitions
            canonicalLayout = $layout.canonical
            canonicalLayoutValue = $layout.value
            bootFileSystem = $null
            dataFileSystem = $DataFileSystem
        }
    }

    if ($partitions.Count -ne 2) {
        throw '保持更新は検証済みMBR・2領域のY-TEC媒体だけに限定します。'
    }
    $bootPartition = $partitions[0]
    $dataPartition = $partitions[1]
    if ([int]$bootPartition.PartitionNumber -ne 1 -or
        [int]$dataPartition.PartitionNumber -ne 2 -or
        [UInt64]$bootPartition.Size -ne
            $script:RescueUsbBootPartitionBytes -or
        -not [bool]$bootPartition.IsActive -or
        [bool]$dataPartition.IsActive -or
        [UInt64]$bootPartition.Offset -eq 0 -or
        [UInt64]$dataPartition.Offset -ne
            ([UInt64]$bootPartition.Offset + [UInt64]$bootPartition.Size) -or
        [UInt64]$dataPartition.Size -eq 0 -or
        [UInt64]$dataPartition.Offset -gt $SizeBytes -or
        [UInt64]$dataPartition.Size -ne
            ($SizeBytes - [UInt64]$dataPartition.Offset)) {
        throw '4GiB FAT32起動領域＋残容量データ領域の完全レイアウトではありません。'
    }
    $bootVolumes = @($bootPartition | Get-Volume -ErrorAction Stop)
    $dataVolumes = @($dataPartition | Get-Volume -ErrorAction Stop)
    if ($bootVolumes.Count -ne 1 -or $dataVolumes.Count -ne 1 -or
        [string]::IsNullOrWhiteSpace([string]$bootVolumes[0].DriveLetter) -or
        [string]::IsNullOrWhiteSpace([string]$dataVolumes[0].DriveLetter) -or
        [string]$bootVolumes[0].DriveLetter -ne $driveLetter -or
        [string]$bootVolumes[0].FileSystem -ine 'FAT32' -or
        [string]$dataVolumes[0].FileSystem -ine $DataFileSystem -or
        [string]$bootVolumes[0].DriveLetter -ieq
            [string]$dataVolumes[0].DriveLetter) {
        throw '起動／データ領域のドライブ文字またはファイルシステムが一致しません。'
    }
    $root = "$driveLetter`:\"
    $dataDriveLetter =
        ([string]$dataVolumes[0].DriveLetter).ToUpperInvariant()
    $dataRoot = "$dataDriveLetter`:\"
    foreach ($volumeRoot in @($root, $dataRoot)) {
        if (-not (Test-Path -LiteralPath $volumeRoot -PathType Container) -or
            ((Get-Item -LiteralPath $volumeRoot -Force).Attributes -band
                [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw '選択USBの通常ボリュームルートを再確認できません。'
        }
    }
    return [ordered]@{
        disk = $verifiedDisk.disk
        diskNumber = $DiskNumber
        drive = "$driveLetter`:"
        root = $root
        sizeBytes = $SizeBytes
        serialSuffix = $SerialSuffix
        deviceInstanceId = $verifiedDisk.deviceInstanceId
        partitionStyle = $verifiedDisk.partitionStyle
        partitionNumber = 1
        dataDrive = "$dataDriveLetter`:"
        dataRoot = $dataRoot
        dataPartitionNumber = 2
        partitions = $partitions
        canonicalLayout = $layout.canonical
        canonicalLayoutValue = $layout.value
        bootFileSystem = 'FAT32'
        dataFileSystem = $DataFileSystem
    }
}

function New-RescueUsbPartition {
    param(
        [Parameter(Mandatory)]$Disk,
        [Parameter(Mandatory)][char]$DriveLetter,
        [UInt64]$SizeBytes = 0,
        [switch]$UseMaximumSize,
        [switch]$Active
    )

    $parameters = @{
        InputObject = $Disk
        DriveLetter = $DriveLetter
        ErrorAction = 'Stop'
    }
    if ($UseMaximumSize) {
        $parameters.UseMaximumSize = $true
    } else {
        if ($SizeBytes -eq 0) {
            throw '作成するUSBパーティションサイズがありません。'
        }
        $parameters.Size = $SizeBytes
    }
    if ($Active) {
        $parameters.IsActive = $true
    }
    return New-Partition @parameters
}

function Format-RescueUsbPartition {
    param(
        [Parameter(Mandatory)]$Partition,
        [Parameter(Mandatory)]
        [ValidateSet('FAT32', 'NTFS', 'exFAT')]
        [string]$FileSystem,
        [Parameter(Mandatory)][string]$Label
    )

    Format-Volume `
        -Partition $Partition `
        -FileSystem $FileSystem `
        -NewFileSystemLabel $Label `
        -Force `
        -Confirm:$false `
        -ErrorAction Stop | Out-Null
}

function Initialize-VerifiedUsbTarget {
    param(
        [Parameter(Mandatory)]
        [ValidatePattern('^[A-Za-z]:$')]
        [string]$Drive,
        [Parameter(Mandatory)]
        [ValidateRange(0, [int]::MaxValue)]
        [int]$DiskNumber,
        [Parameter(Mandatory)]
        [UInt64]$SizeBytes,
        [AllowEmptyString()]
        [string]$SerialSuffix,
        [Parameter(Mandatory)]
        [string]$DeviceInstanceId,
        [Parameter(Mandatory)]
        [ValidateSet('NTFS', 'exFAT')]
        [string]$DataFileSystem
    )

    $before = Get-VerifiedUsbTarget `
        -Drive $Drive `
        -DiskNumber $DiskNumber `
        -SizeBytes $SizeBytes `
        -SerialSuffix $SerialSuffix `
        -DeviceInstanceId $DeviceInstanceId `
        -Operation Initialize `
        -DataFileSystem $DataFileSystem
    if ($before.partitions.Count -ne 0) {
        Assert-UsbIdentityAndLayout `
            -DiskNumber $DiskNumber `
            -SizeBytes $SizeBytes `
            -SerialSuffix $SerialSuffix `
            -DeviceInstanceId $DeviceInstanceId `
            -ExpectedCanonicalLayout $before.canonicalLayout `
            -Boundary 'Clear-Disk' | Out-Null
        Clear-Disk `
            -InputObject $before.disk `
            -RemoveData `
            -RemoveOEM `
            -Confirm:$false `
            -ErrorAction Stop
        Update-Disk -Number $DiskNumber -ErrorAction Stop | Out-Null
    }
    $cleared = Get-CanonicalUsbLayout `
        -DiskNumber $DiskNumber `
        -SizeBytes $SizeBytes `
        -SerialSuffix $SerialSuffix `
        -DeviceInstanceId $DeviceInstanceId
    if ($cleared.partitions.Count -ne 0) {
        throw '選択USBの消去後もパーティションが残っているため停止しました。'
    }

    $clearedStyle = [string]$cleared.disk.PartitionStyle
    Assert-UsbIdentityAndLayout `
        -DiskNumber $DiskNumber `
        -SizeBytes $SizeBytes `
        -SerialSuffix $SerialSuffix `
        -DeviceInstanceId $DeviceInstanceId `
        -ExpectedCanonicalLayout $cleared.canonical `
        -Boundary 'MBR初期化' | Out-Null
    if ($clearedStyle -eq 'RAW') {
        Initialize-Disk `
            -InputObject $cleared.disk `
            -PartitionStyle MBR `
            -ErrorAction Stop | Out-Null
    } elseif ($clearedStyle -eq 'GPT') {
        Set-Disk `
            -InputObject $cleared.disk `
            -PartitionStyle MBR `
            -ErrorAction Stop | Out-Null
    } elseif ($clearedStyle -ne 'MBR') {
        throw '選択USBの消去後のパーティション形式が不明なため停止しました。'
    }
    Update-Disk -Number $DiskNumber -ErrorAction Stop | Out-Null
    $initialized = Get-CanonicalUsbLayout `
        -DiskNumber $DiskNumber `
        -SizeBytes $SizeBytes `
        -SerialSuffix $SerialSuffix `
        -DeviceInstanceId $DeviceInstanceId
    if ([string]$initialized.disk.PartitionStyle -ne 'MBR' -or
        $initialized.partitions.Count -ne 0) {
        throw '選択USBを空のMBRディスクとして確認できません。'
    }
    $selectedDrive = Select-UsbDriveLetter `
        -PreferredDrive $before.drive
    $bootDriveLetter = [char]$selectedDrive.Substring(0, 1)
    Assert-UsbIdentityAndLayout `
        -DiskNumber $DiskNumber `
        -SizeBytes $SizeBytes `
        -SerialSuffix $SerialSuffix `
        -DeviceInstanceId $DeviceInstanceId `
        -ExpectedCanonicalLayout $initialized.canonical `
        -Boundary '4GiB起動領域作成' | Out-Null
    $bootPartition = New-RescueUsbPartition `
        -Disk $initialized.disk `
        -DriveLetter $bootDriveLetter `
        -SizeBytes $script:RescueUsbBootPartitionBytes `
        -Active
    $bootCreated = Get-CanonicalUsbLayout `
        -DiskNumber $DiskNumber `
        -SizeBytes $SizeBytes `
        -SerialSuffix $SerialSuffix `
        -DeviceInstanceId $DeviceInstanceId
    if ($bootCreated.partitions.Count -ne 1 -or
        [UInt64]$bootCreated.partitions[0].Size -ne
            $script:RescueUsbBootPartitionBytes -or
        -not [bool]$bootCreated.partitions[0].IsActive) {
        throw '正確な4GiBのactive起動領域を作成できませんでした。'
    }
    Assert-UsbIdentityAndLayout `
        -DiskNumber $DiskNumber `
        -SizeBytes $SizeBytes `
        -SerialSuffix $SerialSuffix `
        -DeviceInstanceId $DeviceInstanceId `
        -ExpectedCanonicalLayout $bootCreated.canonical `
        -Boundary 'FAT32起動領域format' | Out-Null
    Format-RescueUsbPartition `
        -Partition $bootPartition `
        -FileSystem FAT32 `
        -Label 'TSUMUGI_BOOT'

    $bootFormatted = Get-CanonicalUsbLayout `
        -DiskNumber $DiskNumber `
        -SizeBytes $SizeBytes `
        -SerialSuffix $SerialSuffix `
        -DeviceInstanceId $DeviceInstanceId
    $dataDrive = Select-UsbDriveLetter -PreferredDrive $selectedDrive
    $dataDriveLetter = [char]$dataDrive.Substring(0, 1)
    Assert-UsbIdentityAndLayout `
        -DiskNumber $DiskNumber `
        -SizeBytes $SizeBytes `
        -SerialSuffix $SerialSuffix `
        -DeviceInstanceId $DeviceInstanceId `
        -ExpectedCanonicalLayout $bootFormatted.canonical `
        -Boundary '残容量データ領域作成' | Out-Null
    $dataPartition = New-RescueUsbPartition `
        -Disk $bootFormatted.disk `
        -DriveLetter $dataDriveLetter `
        -UseMaximumSize
    $dataCreated = Get-CanonicalUsbLayout `
        -DiskNumber $DiskNumber `
        -SizeBytes $SizeBytes `
        -SerialSuffix $SerialSuffix `
        -DeviceInstanceId $DeviceInstanceId
    if ($dataCreated.partitions.Count -ne 2) {
        throw '残容量データ領域を一意に作成できませんでした。'
    }
    Assert-UsbIdentityAndLayout `
        -DiskNumber $DiskNumber `
        -SizeBytes $SizeBytes `
        -SerialSuffix $SerialSuffix `
        -DeviceInstanceId $DeviceInstanceId `
        -ExpectedCanonicalLayout $dataCreated.canonical `
        -Boundary "$DataFileSystem データ領域format" | Out-Null
    Format-RescueUsbPartition `
        -Partition $dataPartition `
        -FileSystem $DataFileSystem `
        -Label 'TSUMUGI_DATA'

    return Get-VerifiedUsbTarget `
        -Drive $selectedDrive `
        -DiskNumber $DiskNumber `
        -SizeBytes $SizeBytes `
        -SerialSuffix $SerialSuffix `
        -DeviceInstanceId $DeviceInstanceId `
        -Operation Refresh `
        -DataFileSystem $DataFileSystem
}

function Assert-ExactObjectProperties {
    param(
        [Parameter(Mandatory)]$Value,
        [Parameter(Mandatory)][string[]]$ExpectedNames,
        [Parameter(Mandatory)][string]$Description
    )

    $observedNames = @($Value.PSObject.Properties.Name)
    if (($observedNames -join "`n") -cne ($ExpectedNames -join "`n")) {
        throw "$Description の項目が既知のschemaと一致しません。"
    }
}

function ConvertTo-ValidatedOwnedBootTree {
    param([Parameter(Mandatory)]$Value)

    Assert-ExactObjectProperties `
        -Value $Value `
        -ExpectedNames @(
            'entryCount',
            'fileCount',
            'totalPathCharacters',
            'totalLogicalBytes',
            'rootDigest',
            'files',
            'directories') `
        -Description '所有manifestの起動ツリー'
    [UInt64]$entryCount = $Value.entryCount
    [UInt64]$fileCount = $Value.fileCount
    [UInt64]$totalPathCharacters = $Value.totalPathCharacters
    [UInt64]$totalLogicalBytes = $Value.totalLogicalBytes
    $files = @($Value.files)
    $directories = @($Value.directories)
    if ($entryCount -gt [UInt64]$script:MaximumBootFileCount -or
        $fileCount -gt $entryCount -or
        $files.Count -ne $fileCount -or
        ($files.Count + $directories.Count) -ne $entryCount -or
        $totalPathCharacters -gt $script:MaximumBootPathCharacters -or
        $totalLogicalBytes -gt $script:MaximumBootLogicalBytes -or
        [string]$Value.rootDigest -notmatch '^[0-9A-F]{64}$') {
        throw '所有manifestの起動ツリーが安全上限またはdigest形式に違反しています。'
    }
    $normalizedFiles = @(
        foreach ($file in $files) {
            Assert-ExactObjectProperties `
                -Value $file `
                -ExpectedNames @('relativePath', 'length', 'sha256') `
                -Description '所有manifestの起動ファイル'
            Assert-SafeMediaRelativePath `
                -RelativePath ([string]$file.relativePath)
            if ([UInt64]$file.length -gt $script:MaximumBootLogicalBytes -or
                [string]$file.sha256 -notmatch '^[0-9A-F]{64}$') {
                throw '所有manifestの起動ファイル長またはSHA-256が不正です。'
            }
            [ordered]@{
                relativePath = ([string]$file.relativePath).ToUpperInvariant()
                length = [UInt64]$file.length
                sha256 = ([string]$file.sha256).ToUpperInvariant()
            }
        }
    )
    $normalizedDirectories = @(
        foreach ($directory in $directories) {
            Assert-SafeMediaRelativePath -RelativePath ([string]$directory)
            ([string]$directory).ToUpperInvariant()
        }
    )
    $normalizedFiles = @($normalizedFiles | Sort-Object -Property relativePath)
    $normalizedDirectories = @($normalizedDirectories | Sort-Object -Unique)
    $allPaths = @(
        $normalizedFiles.relativePath
        $normalizedDirectories
    )
    if (@($allPaths | Sort-Object -Unique).Count -ne $allPaths.Count -or
        $normalizedDirectories.Count -ne $directories.Count) {
        throw '所有manifestに大小文字違いを含む重複パスがあります。'
    }
    return [ordered]@{
        entryCount = $entryCount
        fileCount = $fileCount
        totalPathCharacters = $totalPathCharacters
        totalLogicalBytes = $totalLogicalBytes
        rootDigest = ([string]$Value.rootDigest).ToUpperInvariant()
        files = $normalizedFiles
        directories = $normalizedDirectories
    }
}

function Get-VerifiedOwnedUsbMedia {
    param(
        [Parameter(Mandatory)]$Target,
        [Parameter(Mandatory)]
        [ValidateSet('NTFS', 'exFAT')]
        [string]$DataFileSystem
    )

    $manifestPath = Join-Path `
        $Target.root $script:RescueUsbManifestRelativePath
    $markerPath = Join-Path `
        $Target.root $script:RescueUsbMarkerRelativePath
    Assert-RegularNonReparseFile `
        -Path $manifestPath `
        -Description 'レスキューUSB所有manifest'
    Assert-RegularNonReparseFile `
        -Path $markerPath `
        -Description 'レスキューUSB媒体ID'
    $manifestItem = Get-Item -LiteralPath $manifestPath -Force
    if ([UInt64]$manifestItem.Length -eq 0 -or
        [UInt64]$manifestItem.Length -gt
            $script:MaximumOwnershipManifestBytes) {
        throw 'レスキューUSB所有manifestのサイズが安全上限外です。'
    }
    $manifest = [IO.File]::ReadAllText(
        $manifestPath,
        [Text.UTF8Encoding]::new($false)) | ConvertFrom-Json
    Assert-ExactObjectProperties `
        -Value $manifest `
        -ExpectedNames @(
            'schemaVersion',
            'purpose',
            'mediaId',
            'bootFileSystem',
            'dataFileSystem',
            'bootPartitionBytes',
            'canonicalLayout',
            'ownedBootTree') `
        -Description 'レスキューUSB所有manifest'
    $mediaId = [string]$manifest.mediaId
    $markerBytes = [IO.File]::ReadAllBytes($markerPath)
    if ([UInt32]$manifest.schemaVersion -ne 2 -or
        [string]$manifest.purpose -cne
            $script:RescueUsbOwnershipPurpose -or
        $mediaId -notmatch
            '^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$' -or
        $markerBytes.Length -ne 36 -or
        [Text.Encoding]::ASCII.GetString($markerBytes) -cne $mediaId -or
        [string]$manifest.bootFileSystem -cne 'FAT32' -or
        [string]$manifest.dataFileSystem -cne $DataFileSystem -or
        [UInt64]$manifest.bootPartitionBytes -ne
            $script:RescueUsbBootPartitionBytes) {
        throw 'Y-TEC所有manifest、媒体ID、またはファイルシステム契約が一致しません。'
    }
    $manifestLayoutJson = $manifest.canonicalLayout |
        ConvertTo-Json -Depth 8 -Compress
    if ($manifestLayoutJson -cne $Target.canonicalLayout) {
        throw '所有manifestの完全パーティションレイアウトが現在値と一致しません。'
    }
    $ownedBootTree = ConvertTo-ValidatedOwnedBootTree `
        -Value $manifest.ownedBootTree
    $observedBootTree = Get-BoundedMediaTreeManifest `
        -Root $Target.root `
        -ExcludedRelativePaths @($script:RescueUsbManifestRelativePath) `
        -MaximumFileCount $script:MaximumBootFileCount `
        -MaximumPathCharacters $script:MaximumBootPathCharacters `
        -MaximumLogicalBytes $script:MaximumBootLogicalBytes `
        -FileSystem FAT32
    Assert-MediaTreeManifestEqual `
        -Expected $ownedBootTree `
        -Observed $observedBootTree `
        -Description '所有済み起動領域'
    foreach ($requiredRelativePath in @(
            'SOURCES\BOOT.WIM',
            'BOOTMGR',
            'EFI\BOOT\BOOTX64.EFI',
            $script:RescueUsbMarkerRelativePath.ToUpperInvariant())) {
        if ($ownedBootTree.files.relativePath -cnotcontains
            $requiredRelativePath) {
            throw '所有manifestに必須起動ファイルがありません。'
        }
    }
    $transactionPath =
        $script:RescueUsbTransactionRelativePath.ToUpperInvariant()
    if ($ownedBootTree.directories -cnotcontains $transactionPath -or
        @($ownedBootTree.files | Where-Object {
            $_.relativePath.StartsWith(
                $transactionPath + '\',
                [StringComparison]::OrdinalIgnoreCase)
        }).Count -ne 0 -or
        @($ownedBootTree.directories | Where-Object {
            $_ -ne $transactionPath -and $_.StartsWith(
                $transactionPath + '\',
                [StringComparison]::OrdinalIgnoreCase)
        }).Count -ne 0) {
        throw '所有manifestの予約transaction領域が空ではありません。'
    }
    return [ordered]@{
        mediaId = $mediaId
        manifest = $manifest
        manifestPath = $manifestPath
        ownedBootTree = $ownedBootTree
        observedBootTree = $observedBootTree
    }
}

function New-RescueUsbOwnershipManifest {
    param(
        [Parameter(Mandatory)][string]$MediaRoot,
        [Parameter(Mandatory)]$Target,
        [Parameter(Mandatory)][string]$MediaId,
        [Parameter(Mandatory)]
        [ValidateSet('NTFS', 'exFAT')]
        [string]$DataFileSystem,
        [Parameter(Mandatory)][string]$FsutilPath
    )

    $transactionRoot = Join-Path `
        $MediaRoot $script:RescueUsbTransactionRelativePath
    $manifestPath = Join-Path `
        $MediaRoot $script:RescueUsbManifestRelativePath
    if (Test-Path -LiteralPath $transactionRoot) {
        throw '作成元媒体に予約transaction領域が既に存在します。'
    }
    if (Test-Path -LiteralPath $manifestPath) {
        throw '作成元媒体に所有manifestが既に存在します。'
    }
    New-Item -ItemType Directory -Path $transactionRoot | Out-Null
    $ownedBootTree = Get-BoundedMediaTreeManifest `
        -Root $MediaRoot `
        -ExcludedRelativePaths @($script:RescueUsbManifestRelativePath) `
        -MaximumFileCount $script:MaximumBootFileCount `
        -MaximumPathCharacters $script:MaximumBootPathCharacters `
        -MaximumLogicalBytes $script:MaximumBootLogicalBytes `
        -FileSystem NTFS `
        -FsutilPath $FsutilPath
    $manifest = [ordered]@{
        schemaVersion = 2
        purpose = $script:RescueUsbOwnershipPurpose
        mediaId = $MediaId
        bootFileSystem = 'FAT32'
        dataFileSystem = $DataFileSystem
        bootPartitionBytes = $script:RescueUsbBootPartitionBytes
        canonicalLayout = $Target.canonicalLayoutValue
        ownedBootTree = $ownedBootTree
    }
    [IO.File]::WriteAllText(
        $manifestPath,
        ($manifest | ConvertTo-Json -Depth 10),
        [Text.UTF8Encoding]::new($false))
    Assert-RegularNonReparseFile `
        -Path $manifestPath `
        -Description 'staging済み所有manifest'
    $reobserved = Get-BoundedMediaTreeManifest `
        -Root $MediaRoot `
        -ExcludedRelativePaths @($script:RescueUsbManifestRelativePath) `
        -MaximumFileCount $script:MaximumBootFileCount `
        -MaximumPathCharacters $script:MaximumBootPathCharacters `
        -MaximumLogicalBytes $script:MaximumBootLogicalBytes `
        -FileSystem NTFS `
        -FsutilPath $FsutilPath
    Assert-MediaTreeManifestEqual `
        -Expected $ownedBootTree `
        -Observed $reobserved `
        -Description '作成元起動領域'
    return [ordered]@{
        manifest = $manifest
        manifestPath = $manifestPath
        ownedBootTree = $ownedBootTree
    }
}

function Get-PrivateDataTreeSummary {
    param(
        [Parameter(Mandatory)]$Target,
        [Parameter(Mandatory)][string]$FsutilPath
    )

    try {
        return Get-BoundedMediaTreeManifest `
            -Root $Target.dataRoot `
            -ExcludedRelativePaths @(
                'System Volume Information',
                '$RECYCLE.BIN') `
            -MaximumFileCount $script:MaximumDataFileCount `
            -MaximumPathCharacters $script:MaximumDataPathCharacters `
            -MaximumLogicalBytes $script:MaximumDataLogicalBytes `
            -FileSystem $Target.dataFileSystem `
            -FsutilPath $FsutilPath `
            -PrivacyPreservingSummary `
            -RedactPaths
    } catch {
        # Do not allow provider/fsutil/Get-FileHash ErrorRecord details to
        # disclose user data basenames through transcript, stderr or reports.
        throw [IO.InvalidDataException]::new(
            '保持データの非公開tree scanに失敗しました (DATA_TREE_SCAN_FAILED)。')
    }
}

function Copy-RescueBootPayloadToStage {
    param(
        [Parameter(Mandatory)][string]$SourceRoot,
        [Parameter(Mandatory)][string]$StageRoot
    )

    foreach ($item in @(Get-ChildItem -LiteralPath $SourceRoot -Force)) {
        if ($item.Name.Equals(
                $script:RescueUsbTransactionRelativePath,
                [StringComparison]::OrdinalIgnoreCase)) {
            continue
        }
        if (($item.Attributes -band
                [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw '作成元起動領域にreparse pointがあります。'
        }
        $destination = Join-Path $StageRoot $item.Name
        if (Test-Path -LiteralPath $destination) {
            throw '非上書きstaging先に同名項目があります。'
        }
        Copy-Item `
            -LiteralPath $item.FullName `
            -Destination $destination `
            -Recurse
    }
}

function Move-RescueBootChildren {
    param(
        [Parameter(Mandatory)][string]$SourceRoot,
        [Parameter(Mandatory)][string]$DestinationRoot,
        [switch]$SkipTransactionRoot
    )

    foreach ($item in @(Get-ChildItem -LiteralPath $SourceRoot -Force)) {
        if ($SkipTransactionRoot -and $item.Name.Equals(
                $script:RescueUsbTransactionRelativePath,
                [StringComparison]::OrdinalIgnoreCase)) {
            continue
        }
        if (($item.Attributes -band
                [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw '切替対象の起動領域にreparse pointがあります。'
        }
        $destination = Join-Path $DestinationRoot $item.Name
        if (Test-Path -LiteralPath $destination) {
            throw '非上書き切替先に同名項目があります。'
        }
        Move-Item -LiteralPath $item.FullName -Destination $destination
    }
}

function Remove-RescueBootChildrenExceptTransaction {
    param([Parameter(Mandatory)][string]$Root)

    foreach ($item in @(Get-ChildItem -LiteralPath $Root -Force)) {
        if ($item.Name.Equals(
                $script:RescueUsbTransactionRelativePath,
                [StringComparison]::OrdinalIgnoreCase)) {
            continue
        }
        if (($item.Attributes -band
                [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw 'rollback対象にreparse pointがあるため自動削除を停止しました。'
        }
        Remove-Item -LiteralPath $item.FullName -Recurse -Force
    }
}

function Assert-StagedRescueBootPayload {
    param(
        [Parameter(Mandatory)][string]$SourceRoot,
        [Parameter(Mandatory)][string]$StageRoot
    )

    $expected = Get-BoundedMediaTreeManifest `
        -Root $SourceRoot `
        -ExcludedRelativePaths @(
            $script:RescueUsbManifestRelativePath,
            $script:RescueUsbTransactionRelativePath) `
        -MaximumFileCount $script:MaximumBootFileCount `
        -MaximumPathCharacters $script:MaximumBootPathCharacters `
        -MaximumLogicalBytes $script:MaximumBootLogicalBytes `
        -FileSystem FAT32
    $observed = Get-BoundedMediaTreeManifest `
        -Root $StageRoot `
        -ExcludedRelativePaths @($script:RescueUsbManifestRelativePath) `
        -MaximumFileCount $script:MaximumBootFileCount `
        -MaximumPathCharacters $script:MaximumBootPathCharacters `
        -MaximumLogicalBytes $script:MaximumBootLogicalBytes `
        -FileSystem FAT32
    Assert-MediaTreeManifestEqual `
        -Expected $expected `
        -Observed $observed `
        -Description '非上書きstaging済み起動領域'
    $sourceManifest = Join-Path `
        $SourceRoot $script:RescueUsbManifestRelativePath
    $stagedManifest = Join-Path `
        $StageRoot $script:RescueUsbManifestRelativePath
    Assert-RegularNonReparseFile `
        -Path $stagedManifest `
        -Description 'staging済み所有manifest'
    if ((Get-FileHash -LiteralPath $sourceManifest -Algorithm SHA256).Hash -cne
        (Get-FileHash -LiteralPath $stagedManifest -Algorithm SHA256).Hash) {
        throw 'staging済み所有manifestのSHA-256が一致しません。'
    }
}

function Invoke-RescueUsbBootUpdate {
    param(
        [Parameter(Mandatory)]$Target,
        [Parameter(Mandatory)][string]$SourceRoot,
        [Parameter(Mandatory)]$SourceOwnership,
        [AllowNull()]$PreviousOwnership,
        [AllowNull()]$DataBefore,
        [Parameter(Mandatory)][string]$FsutilPath,
        [Parameter(Mandatory)][string]$BootsectPath,
        [Parameter(Mandatory)]
        [ValidateSet('Initialize', 'Refresh')]
        [string]$Operation
    )

    Assert-UsbIdentityAndLayout `
        -DiskNumber $Target.diskNumber `
        -SizeBytes $Target.sizeBytes `
        -SerialSuffix $Target.serialSuffix `
        -DeviceInstanceId $Target.deviceInstanceId `
        -ExpectedCanonicalLayout $Target.canonicalLayout `
        -Boundary 'USB起動領域staging' | Out-Null
    if ($Operation -eq 'Refresh') {
        if ($null -eq $PreviousOwnership -or $null -eq $DataBefore) {
            throw '保持更新のレビュー済み所有権またはデータdigestがありません。'
        }
        $observedOwnership = Get-VerifiedOwnedUsbMedia `
            -Target $Target `
            -DataFileSystem $Target.dataFileSystem
        Assert-MediaTreeManifestEqual `
            -Expected $PreviousOwnership.ownedBootTree `
            -Observed $observedOwnership.ownedBootTree `
            -Description '書込み直前の所有済み起動領域'
        if ($observedOwnership.mediaId -cne $PreviousOwnership.mediaId) {
            throw '書込み直前にレスキューUSB媒体IDが変化しました。'
        }
        $dataImmediatelyBefore = Get-PrivateDataTreeSummary `
            -Target $Target `
            -FsutilPath $FsutilPath
        Assert-MediaTreeManifestEqual `
            -Expected $DataBefore `
            -Observed $dataImmediatelyBefore `
            -Description '保持データの書込み直前digest'
    }

    $transactionRoot = Join-Path `
        $Target.root $script:RescueUsbTransactionRelativePath
    if ($Operation -eq 'Initialize') {
        if (Test-Path -LiteralPath $transactionRoot) {
            throw '初期化直後の起動領域に予約transaction先が既にあります。'
        }
        New-Item -ItemType Directory -Path $transactionRoot | Out-Null
    } else {
        $transactionItem = Get-Item -LiteralPath $transactionRoot -Force
        if (-not $transactionItem.PSIsContainer -or
            ($transactionItem.Attributes -band
                [IO.FileAttributes]::ReparsePoint) -ne 0 -or
            @(Get-ChildItem -LiteralPath $transactionRoot -Force).Count -ne 0) {
            throw '所有済み媒体の予約transaction領域が空の通常フォルダーではありません。'
        }
    }
    $transactionId = [Guid]::NewGuid().ToString('N')
    $stageRoot = Join-Path $transactionRoot ("stage-$transactionId")
    $backupRoot = Join-Path $transactionRoot ("backup-$transactionId")
    New-Item -ItemType Directory -Path $stageRoot | Out-Null
    New-Item -ItemType Directory -Path $backupRoot | Out-Null
    $destructivePhaseStarted = $false
    $newTreeFullyVerified = $false
    try {
        Copy-RescueBootPayloadToStage `
            -SourceRoot $SourceRoot `
            -StageRoot $stageRoot
        Assert-StagedRescueBootPayload `
            -SourceRoot $SourceRoot `
            -StageRoot $stageRoot

        Assert-UsbIdentityAndLayout `
            -DiskNumber $Target.diskNumber `
            -SizeBytes $Target.sizeBytes `
            -SerialSuffix $Target.serialSuffix `
            -DeviceInstanceId $Target.deviceInstanceId `
            -ExpectedCanonicalLayout $Target.canonicalLayout `
            -Boundary 'USB起動領域切替' | Out-Null
        Assert-StagedRescueBootPayload `
            -SourceRoot $SourceRoot `
            -StageRoot $stageRoot
        if ($Operation -eq 'Refresh') {
            $oldOutsideTransaction = Get-BoundedMediaTreeManifest `
                -Root $Target.root `
                -ExcludedRelativePaths @(
                    $script:RescueUsbManifestRelativePath) `
                -ExcludedRelativePathPrefixes @(
                    $script:RescueUsbTransactionRelativePath) `
                -MaximumFileCount $script:MaximumBootFileCount `
                -MaximumPathCharacters $script:MaximumBootPathCharacters `
                -MaximumLogicalBytes $script:MaximumBootLogicalBytes `
                -FileSystem FAT32
            Assert-MediaTreeManifestEqual `
                -Expected $PreviousOwnership.ownedBootTree `
                -Observed $oldOutsideTransaction `
                -Description '切替直前の所有済み起動領域'
            $dataAtCutover = Get-PrivateDataTreeSummary `
                -Target $Target `
                -FsutilPath $FsutilPath
            Assert-MediaTreeManifestEqual `
                -Expected $DataBefore `
                -Observed $dataAtCutover `
                -Description '切替直前の保持データdigest'
            $destructivePhaseStarted = $true
            Move-RescueBootChildren `
                -SourceRoot $Target.root `
                -DestinationRoot $backupRoot `
                -SkipTransactionRoot
        } else {
            $unexpected = @(
                Get-ChildItem -LiteralPath $Target.root -Force |
                    Where-Object {
                        -not $_.Name.Equals(
                            $script:RescueUsbTransactionRelativePath,
                            [StringComparison]::OrdinalIgnoreCase)
                    }
            )
            if ($unexpected.Count -ne 0) {
                throw '初期化済み起動領域がstaging中に空でなくなりました。'
            }
            $destructivePhaseStarted = $true
        }
        Move-RescueBootChildren `
            -SourceRoot $stageRoot `
            -DestinationRoot $Target.root

        $publishedTree = Get-BoundedMediaTreeManifest `
            -Root $Target.root `
            -ExcludedRelativePaths @(
                $script:RescueUsbManifestRelativePath) `
            -ExcludedRelativePathPrefixes @(
                $script:RescueUsbTransactionRelativePath) `
            -MaximumFileCount $script:MaximumBootFileCount `
            -MaximumPathCharacters $script:MaximumBootPathCharacters `
            -MaximumLogicalBytes $script:MaximumBootLogicalBytes `
            -FileSystem FAT32
        Assert-MediaTreeManifestEqual `
            -Expected $SourceOwnership.ownedBootTree `
            -Observed $publishedTree `
            -Description '切替後の起動領域'
        $sourceManifestPath = Join-Path `
            $SourceRoot $script:RescueUsbManifestRelativePath
        $publishedManifestPath = Join-Path `
            $Target.root $script:RescueUsbManifestRelativePath
        Assert-RegularNonReparseFile `
            -Path $publishedManifestPath `
            -Description '切替後の所有manifest'
        if ((Get-FileHash -LiteralPath $sourceManifestPath `
                    -Algorithm SHA256).Hash -cne
            (Get-FileHash -LiteralPath $publishedManifestPath `
                    -Algorithm SHA256).Hash) {
            throw '切替後の所有manifestのSHA-256が一致しません。'
        }
        $newTreeFullyVerified = $true

        Assert-UsbIdentityAndLayout `
            -DiskNumber $Target.diskNumber `
            -SizeBytes $Target.sizeBytes `
            -SerialSuffix $Target.serialSuffix `
            -DeviceInstanceId $Target.deviceInstanceId `
            -ExpectedCanonicalLayout $Target.canonicalLayout `
            -Boundary 'Bootsect起動コード更新' | Out-Null
        Invoke-CheckedNative `
            -Command $BootsectPath `
            -Arguments @('/nt60', $Target.drive, '/force', '/mbr') `
            -Operation '対象限定USB起動コードの更新'
        $dataAfter = Get-PrivateDataTreeSummary `
            -Target $Target `
            -FsutilPath $FsutilPath
        if ($Operation -eq 'Refresh') {
            Assert-MediaTreeManifestEqual `
                -Expected $DataBefore `
                -Observed $dataAfter `
                -Description '保持更新前後のデータ領域digest'
        }

        Assert-UsbIdentityAndLayout `
            -DiskNumber $Target.diskNumber `
            -SizeBytes $Target.sizeBytes `
            -SerialSuffix $Target.serialSuffix `
            -DeviceInstanceId $Target.deviceInstanceId `
            -ExpectedCanonicalLayout $Target.canonicalLayout `
            -Boundary '検証済みtransaction cleanup' | Out-Null
        Remove-Item -LiteralPath $backupRoot -Recurse -Force
        Remove-Item -LiteralPath $stageRoot -Recurse -Force
        $verifiedOwnership = Get-VerifiedOwnedUsbMedia `
            -Target $Target `
            -DataFileSystem $Target.dataFileSystem
        return [ordered]@{
            ownership = $verifiedOwnership
            dataAfter = $dataAfter
        }
    } catch {
        $originalError = $_
        if ($Operation -eq 'Refresh' -and
            $destructivePhaseStarted -and $newTreeFullyVerified -and
            (Test-Path -LiteralPath $backupRoot -PathType Container)) {
            Assert-UsbIdentityAndLayout `
                -DiskNumber $Target.diskNumber `
                -SizeBytes $Target.sizeBytes `
                -SerialSuffix $Target.serialSuffix `
                -DeviceInstanceId $Target.deviceInstanceId `
                -ExpectedCanonicalLayout $Target.canonicalLayout `
                -Boundary '検証済みrollback' | Out-Null
            Remove-RescueBootChildrenExceptTransaction -Root $Target.root
            Move-RescueBootChildren `
                -SourceRoot $backupRoot `
                -DestinationRoot $Target.root
            if (Test-Path -LiteralPath $stageRoot) {
                Remove-Item -LiteralPath $stageRoot -Recurse -Force
            }
            if (Test-Path -LiteralPath $backupRoot) {
                Remove-Item -LiteralPath $backupRoot -Recurse -Force
            }
            Get-VerifiedOwnedUsbMedia `
                -Target $Target `
                -DataFileSystem $Target.dataFileSystem | Out-Null
        }
        throw $originalError
    }
}

$repoRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$outputFullPath = Assert-ExternalNewOutputPath `
    -RepositoryRoot $repoRoot `
    -CandidatePath $OutputRoot

$finalIsoFullPath = ''
$finalManifestFullPath = ''
if (-not [string]::IsNullOrWhiteSpace($FinalIsoPath)) {
    $finalIsoFullPath = Assert-ExternalNewOutputPath `
        -RepositoryRoot $repoRoot `
        -CandidatePath $FinalIsoPath
    if (-not $finalIsoFullPath.EndsWith(
            '.iso',
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "完成メディアの拡張子は.isoでなければなりません: $finalIsoFullPath"
    }
    $finalParent = Split-Path -Parent $finalIsoFullPath
    if (-not (Test-Path -LiteralPath $finalParent -PathType Container)) {
        throw "完成ISOの親フォルダーがありません: $finalParent"
    }
    $outputPrefix = $outputFullPath.TrimEnd('\') + '\'
    if ($finalIsoFullPath.StartsWith(
            $outputPrefix,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw '完成ISOは一時作業フォルダーの外側に指定してください。'
    }
    $finalManifestFullPath = Assert-ExternalNewOutputPath `
        -RepositoryRoot $repoRoot `
        -CandidatePath ($finalIsoFullPath + '.manifest.json')
}
if ($BuildUsb) {
    if (-not [string]::IsNullOrWhiteSpace($FinalIsoPath)) {
        throw 'USB作成時はISO保存先を同時に指定できません。'
    }
    if ([string]::IsNullOrWhiteSpace($TargetUsbDrive) -or
        $ExpectedUsbDiskNumber -lt 0 -or
        $ExpectedUsbSizeBytes -eq 0 -or
        [string]::IsNullOrWhiteSpace($ExpectedUsbDeviceInstanceId) -or
        [string]::IsNullOrWhiteSpace(
            $ExpectedUsbCanonicalLayoutSha256) -or
        [string]::IsNullOrWhiteSpace($UsbOperation) -or
        [string]::IsNullOrWhiteSpace($UsbDataFileSystem)) {
        throw 'USB作成にはドライブ文字、安定識別情報、初期化／保持更新、データ形式の明示が必要です。'
    }
    if ($ExpectedUsbSizeBytes -lt $script:RescueUsbMinimumBytes) {
        throw 'レスキューUSBは8GiB以上が必要です。16GiB以上を推奨します。'
    }
} elseif (
    -not [string]::IsNullOrWhiteSpace($TargetUsbDrive) -or
    $ExpectedUsbDiskNumber -ne -1 -or
    $ExpectedUsbSizeBytes -ne 0 -or
    -not [string]::IsNullOrWhiteSpace($ExpectedUsbSerialSuffix) -or
    -not [string]::IsNullOrWhiteSpace($ExpectedUsbDeviceInstanceId) -or
    -not [string]::IsNullOrWhiteSpace(
        $ExpectedUsbCanonicalLayoutSha256) -or
    -not [string]::IsNullOrWhiteSpace($UsbOperation) -or
    -not [string]::IsNullOrWhiteSpace($UsbDataFileSystem)) {
    throw 'USB対象情報は-BuildUsbを指定した場合だけ使用できます。'
}

$diagnostic = if ([string]::IsNullOrWhiteSpace($DiagnosticPath)) {
    Join-Path $repoRoot `
        'out\build\msvc-x64\src\MediaBuilder\ytec-winpe-environment.exe'
} else {
    [IO.Path]::GetFullPath($DiagnosticPath)
}
Assert-RegularNonReparseFile `
    -Path $diagnostic `
    -Description 'WinPE環境診断CLI'

$diagnosticText = (& $diagnostic --json | Out-String)
$diagnosticExit = $LASTEXITCODE
if ($diagnosticExit -ne 0) {
    throw "WinPE環境診断が終了コード $diagnosticExit で作成を拒否しました。"
}
$diagnosticReport = $diagnosticText | ConvertFrom-Json
if (-not $diagnosticReport.mediaCreationPermitted -or
    $null -eq $diagnosticReport.selectedCandidateIndex) {
    throw 'WinPE環境診断の作成許可ゲートを通過していません。'
}

$candidate = $diagnosticReport.candidates[
    [int]$diagnosticReport.selectedCandidateIndex]
$adkRoot = [IO.Path]::GetFullPath([string]$candidate.root)
$winpeRoot = Join-Path $adkRoot 'Windows Preinstallation Environment'
$deploymentRoot = Join-Path $adkRoot 'Deployment Tools'
$sourceMedia = Join-Path $winpeRoot 'amd64\Media'
$sourceWim = Join-Path $winpeRoot 'amd64\en-us\winpe.wim'
$japaneseFontSupport = Join-Path $winpeRoot `
    'amd64\WinPE_OCs\WinPE-FontSupport-JA-JP.cab'
$dism = Join-Path $deploymentRoot 'amd64\DISM\dism.exe'
$oscdimgRoot = Join-Path $deploymentRoot 'amd64\Oscdimg'
$oscdimg = Join-Path $oscdimgRoot 'oscdimg.exe'
$bootsectRoot = Join-Path $deploymentRoot 'amd64\BCDBoot'
$bootsect = Join-Path $bootsectRoot 'bootsect.exe'
$makeWinPEMedia = Join-Path $winpeRoot 'MakeWinPEMedia.cmd'
$systemCmd = Join-Path $env:SystemRoot 'System32\cmd.exe'
$fsutil = Join-Path $env:SystemRoot 'System32\fsutil.exe'
$etfsboot = Join-Path $oscdimgRoot 'etfsboot.com'
$efiBootImage = if ($CertificateGeneration -eq '2023CA') {
    Join-Path $oscdimgRoot 'efisys_EX.bin'
} else {
    Join-Path $oscdimgRoot 'efisys.bin'
}

if (-not (Test-Path -LiteralPath $sourceMedia -PathType Container)) {
    throw "WinPE amd64 Mediaが見つかりません: $sourceMedia"
}
foreach ($required in @(
        $sourceWim,
        $japaneseFontSupport,
        $makeWinPEMedia,
        $etfsboot,
        $efiBootImage)) {
    Assert-RegularNonReparseFile -Path $required -Description 'ADK構成要素'
}
Assert-MicrosoftSignature -Path $dism -Description 'DISM'
Assert-MicrosoftSignature -Path $oscdimg -Description 'Oscdimg'
Assert-MicrosoftSignature -Path $bootsect -Description 'Bootsect'
Assert-MicrosoftSignature -Path $systemCmd -Description 'Windows cmd'
Assert-MicrosoftSignature -Path $fsutil -Description 'Windows Fsutil'

$winpeApp = if ([string]::IsNullOrWhiteSpace($WinPEAppPath)) {
    Join-Path $repoRoot `
        'out\build\msvc-x64-vm\src\WinPEApp\ytec-winpe-app.exe'
} else {
    [IO.Path]::GetFullPath($WinPEAppPath)
}
$winpeGui = if ([string]::IsNullOrWhiteSpace($WinPEGuiPath)) {
    Join-Path $repoRoot `
        'out\build\msvc-x64-vm\src\WinPEApp\ytec-winpe-gui.exe'
} else {
    [IO.Path]::GetFullPath($WinPEGuiPath)
}
$cliDependencies = @(
    'ADVAPI32.dll', 'bcrypt.dll', 'KERNEL32.dll', 'SETUPAPI.dll')
$guiDependencies = @(
    'ADVAPI32.dll', 'bcrypt.dll', 'COMCTL32.dll', 'COMDLG32.dll',
    'CRYPT32.dll', 'GDI32.dll', 'KERNEL32.dll', 'ole32.dll',
    'POWRPROF.dll', 'SETUPAPI.dll', 'USER32.dll', 'WINTRUST.dll')
$appReport = Get-WinPEAppPeReport `
    -Path $winpeApp `
    -AllowedDependencies $cliDependencies `
    -Description 'WinPE CLI'
$guiReport = Get-WinPEAppPeReport `
    -Path $winpeGui `
    -AllowedDependencies $guiDependencies `
    -Description 'WinPE GUI'
$guiReport['dynamicallyLoadedSystemDlls'] = @()
$projectLicense = Join-Path $repoRoot 'LICENSE'
$projectNotice = Join-Path $repoRoot 'NOTICE'
$licenseReadme = Join-Path $repoRoot 'licenses\README.md'
$lineSeedLicense = Join-Path $repoRoot `
    'licenses\LINE-Seed-JP-OFL-1.1.txt'
$zstandardLicense = Join-Path $repoRoot `
    'licenses\Zstandard-BSD-3-Clause.txt'
$argon2License = Join-Path $repoRoot `
    'licenses\Argon2-Apache-2.0.txt'
$thirdPartyNotices = Join-Path $repoRoot 'THIRD-PARTY-NOTICES.txt'
$sbom = Join-Path $repoRoot 'SBOM.spdx.json'
$thirdPartyPayloadSources = [ordered]@{
    projectLicense = $projectLicense
    projectNotice = $projectNotice
    notices = $thirdPartyNotices
    sbom = $sbom
    licenseReadme = $licenseReadme
    lineSeedLicense = $lineSeedLicense
    zstandardLicense = $zstandardLicense
    argon2License = $argon2License
}
$thirdPartyPayloadReport = [ordered]@{}
foreach ($entry in $thirdPartyPayloadSources.GetEnumerator()) {
    Assert-RegularNonReparseFile `
        -Path $entry.Value `
        -Description "第三者ライセンス資料 $($entry.Key)"
    $thirdPartyPayloadReport[$entry.Key] = [ordered]@{
        path = $entry.Value
        length = (Get-Item -LiteralPath $entry.Value).Length
        sha256 = (Get-FileHash -LiteralPath $entry.Value `
            -Algorithm SHA256).Hash
    }
}
$lineSeedLicenseReport = [ordered]@{
    name = 'LINE Seed JP'
    version = 'LINESeedJP_20241105'
    license = 'OFL-1.1'
    path = $lineSeedLicense
    length = (Get-Item -LiteralPath $lineSeedLicense).Length
    sha256 = (Get-FileHash -LiteralPath $lineSeedLicense `
        -Algorithm SHA256).Hash
}

$preflight = [ordered]@{
    schemaVersion = 1
    outputRoot = $outputFullPath
    outputExists = $false
    certificateGeneration = $CertificateGeneration
    validationScenario = $ValidationScenario
    adkVersion = [string]$candidate.deploymentToolsVersion
    dismVersion = [string]$candidate.dismFileVersion
    servicingUpdate = 'KB5101684'
    sourceWim = [ordered]@{
        path = $sourceWim
        length = (Get-Item -LiteralPath $sourceWim).Length
        sha256 = (Get-FileHash -LiteralPath $sourceWim `
            -Algorithm SHA256).Hash
    }
    japaneseFontSupport = [ordered]@{
        source = 'Installed ADK WinPE optional component'
        repositoryCopy = $false
        path = $japaneseFontSupport
        length = (Get-Item -LiteralPath $japaneseFontSupport).Length
        sha256 = (Get-FileHash -LiteralPath $japaneseFontSupport `
            -Algorithm SHA256).Hash
    }
    winpeApp = $appReport
    winpeGui = $guiReport
    lineSeedLicense = $lineSeedLicenseReport
    thirdPartyPayload = $thirdPartyPayloadReport
    finalIsoPath = $finalIsoFullPath
    finalManifestPath = $finalManifestFullPath
    buildUsbRequested = [bool]$BuildUsb
    targetUsbDrive = $TargetUsbDrive.ToUpperInvariant()
    expectedUsbDiskNumber = $ExpectedUsbDiskNumber
    expectedUsbSizeBytes = $ExpectedUsbSizeBytes
    expectedUsbSerialSuffix = $ExpectedUsbSerialSuffix
    expectedUsbCanonicalLayoutSha256 =
        $ExpectedUsbCanonicalLayoutSha256.ToUpperInvariant()
    usbOperation = $UsbOperation
    usbDataFileSystem = $UsbDataFileSystem
    buildRequested = [bool]$BuildMedia
    administrator = Test-IsAdministrator
}

if (-not $BuildMedia) {
    Write-Output ('WINPE_APP_MEDIA_PREFLIGHT_PASS=' +
        ($preflight | ConvertTo-Json -Depth 6 -Compress))
    return
}

if (-not $preflight.administrator) {
    throw '実WIMのマウント/コミットには管理者権限が必要です。UACを承認したPowerShellで-BuildMediaを実行してください。'
}

$initialUsbTarget = $null
$initialUsbOwnership = $null
$initialDataSummary = $null
if ($BuildUsb) {
    $initialUsbTarget = Get-VerifiedUsbTarget `
        -Drive $TargetUsbDrive `
        -DiskNumber $ExpectedUsbDiskNumber `
        -SizeBytes $ExpectedUsbSizeBytes `
        -SerialSuffix $ExpectedUsbSerialSuffix `
        -DeviceInstanceId $ExpectedUsbDeviceInstanceId `
        -Operation $UsbOperation `
        -DataFileSystem $UsbDataFileSystem
    $initialLayoutDigest = Get-CanonicalUsbLayoutDigest `
        -LayoutValue $initialUsbTarget.canonicalLayoutValue
    if ($initialLayoutDigest -cne
        $ExpectedUsbCanonicalLayoutSha256.ToUpperInvariant()) {
        throw 'USBの完全パーティションレイアウトがレビュー済み計画と一致しません。'
    }
    if ($UsbOperation -eq 'Refresh') {
        $initialUsbOwnership = Get-VerifiedOwnedUsbMedia `
            -Target $initialUsbTarget `
            -DataFileSystem $UsbDataFileSystem
        $initialDataSummary = Get-PrivateDataTreeSummary `
            -Target $initialUsbTarget `
            -FsutilPath $fsutil
    }
}

Write-MediaProgress -Percent 5 -Stage 'preflight'

$sourceReparse = Get-ChildItem -LiteralPath $sourceMedia -Recurse -Force |
    Where-Object {
        ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0
    } |
    Select-Object -First 1
if ($null -ne $sourceReparse) {
    throw "WinPE Media内のreparse pointはコピーしません: $($sourceReparse.FullName)"
}

$workingRoot = Join-Path $outputFullPath 'working'
$mediaRoot = Join-Path $workingRoot 'media'
$sourcesRoot = Join-Path $mediaRoot 'sources'
$mountRoot = Join-Path $workingRoot 'mount'
$bootBinsRoot = Join-Path $workingRoot 'bootbins'
$bootWim = Join-Path $sourcesRoot 'boot.wim'
$isoBaseName = 'YDC-WinPEApp'
$isoPath = Join-Path $outputFullPath `
    "$isoBaseName-amd64-$CertificateGeneration.iso"
$manifestPath = Join-Path $outputFullPath 'winpe-app-media-manifest.json'
$rescueMediaMarkerRelativePath = 'YtecDiskClone\rescue-media-id.txt'
$rescueMediaId = if ($UsbOperation -eq 'Refresh') {
    $initialUsbOwnership.mediaId
} else {
    [Guid]::NewGuid().ToString('D')
}
$rescueMediaMarkerBytes = [Text.Encoding]::ASCII.GetBytes($rescueMediaId)
if ($rescueMediaMarkerBytes.Length -ne 36 -or
    $rescueMediaId -notmatch
        '^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$') {
    throw 'レスキュー媒体の一意マーカーを有界ASCII GUID形式で生成できませんでした。'
}

New-Item -ItemType Directory -Path $outputFullPath | Out-Null
New-Item -ItemType Directory -Path $workingRoot | Out-Null
New-Item -ItemType Directory -Path $bootBinsRoot | Out-Null
Write-MediaProgress -Percent 12 -Stage 'created-working-area'
Copy-Item -LiteralPath $sourceMedia -Destination $mediaRoot -Recurse
$mediaMarkerPath = Join-Path $mediaRoot $rescueMediaMarkerRelativePath
$mediaMarkerDirectory = Split-Path -Parent $mediaMarkerPath
if (Test-Path -LiteralPath $mediaMarkerPath) {
    throw "標準WinPE媒体の予約マーカー先が既に存在します: $mediaMarkerPath"
}
if (-not (Test-Path -LiteralPath $mediaMarkerDirectory)) {
    New-Item -ItemType Directory -Path $mediaMarkerDirectory | Out-Null
}
$mediaMarkerDirectoryItem = Get-Item `
    -LiteralPath $mediaMarkerDirectory -Force
if (-not $mediaMarkerDirectoryItem.PSIsContainer -or
    ($mediaMarkerDirectoryItem.Attributes -band
        [IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw "レスキュー媒体マーカーの親は通常フォルダーである必要があります: $mediaMarkerDirectory"
}
[IO.File]::WriteAllBytes($mediaMarkerPath, $rescueMediaMarkerBytes)
Assert-RegularNonReparseFile `
    -Path $mediaMarkerPath `
    -Description '媒体ルートのレスキュー媒体マーカー'
$rescueMediaMarkerReport = [ordered]@{
    relativePath = $rescueMediaMarkerRelativePath
    length = (Get-Item -LiteralPath $mediaMarkerPath).Length
    sha256 = (Get-FileHash -LiteralPath $mediaMarkerPath `
        -Algorithm SHA256).Hash
}
$mediaPayloadRoot = $mediaMarkerDirectory
$unexpectedMediaPayload = @(
    Get-ChildItem -LiteralPath $mediaPayloadRoot -Force |
        Where-Object {
            -not $_.Name.Equals(
                'rescue-media-id.txt',
                [StringComparison]::OrdinalIgnoreCase)
        }
)
if ($unexpectedMediaPayload.Count -ne 0) {
    throw '標準WinPE媒体の製品payload予約先に未知の項目があります。'
}
$mediaPayloadApp = Join-Path $mediaPayloadRoot 'ytec-winpe-app.exe'
$mediaPayloadGui = Join-Path $mediaPayloadRoot 'ytec-winpe-gui.exe'
$mediaPayloadProjectLicense = Join-Path $mediaPayloadRoot 'LICENSE'
$mediaPayloadProjectNotice = Join-Path $mediaPayloadRoot 'NOTICE'
$mediaPayloadNotices = Join-Path `
    $mediaPayloadRoot 'THIRD-PARTY-NOTICES.txt'
$mediaPayloadSbom = Join-Path $mediaPayloadRoot 'SBOM.spdx.json'
$mediaPayloadLicenses = Join-Path $mediaPayloadRoot 'licenses'
$mediaPayloadLicenseReadme = Join-Path $mediaPayloadLicenses 'README.md'
$mediaPayloadLineSeedLicense = Join-Path $mediaPayloadLicenses `
    'LINE-Seed-JP-OFL-1.1.txt'
$mediaPayloadZstandardLicense = Join-Path $mediaPayloadLicenses `
    'Zstandard-BSD-3-Clause.txt'
$mediaPayloadArgon2License = Join-Path $mediaPayloadLicenses `
    'Argon2-Apache-2.0.txt'
$mediaPayloadData = Join-Path $mediaPayloadRoot 'data'
$mediaPayloadDataReadme = Join-Path $mediaPayloadData 'README.txt'
New-Item -ItemType Directory -Path $mediaPayloadLicenses | Out-Null
New-Item -ItemType Directory -Path $mediaPayloadData | Out-Null
Copy-Item -LiteralPath $winpeApp -Destination $mediaPayloadApp
Copy-Item -LiteralPath $winpeGui -Destination $mediaPayloadGui
Copy-Item -LiteralPath $projectLicense -Destination $mediaPayloadProjectLicense
Copy-Item -LiteralPath $projectNotice -Destination $mediaPayloadProjectNotice
Copy-Item -LiteralPath $thirdPartyNotices -Destination $mediaPayloadNotices
Copy-Item -LiteralPath $sbom -Destination $mediaPayloadSbom
Copy-Item -LiteralPath $licenseReadme -Destination $mediaPayloadLicenseReadme
Copy-Item -LiteralPath $lineSeedLicense `
    -Destination $mediaPayloadLineSeedLicense
Copy-Item -LiteralPath $zstandardLicense `
    -Destination $mediaPayloadZstandardLicense
Copy-Item -LiteralPath $argon2License `
    -Destination $mediaPayloadArgon2License
[IO.File]::WriteAllText(
    $mediaPayloadDataReadme,
    @'
Y-TEC Tsumugi Drive WinPE resume data directory.
USB media keeps active.checkpoint across reboot. ISO/CD-ROM media does not support persistent resume.
'@,
    [Text.UTF8Encoding]::new($false))
$mediaPayloadFiles = [ordered]@{
    winpeApp = $mediaPayloadApp
    winpeGui = $mediaPayloadGui
    projectLicense = $mediaPayloadProjectLicense
    projectNotice = $mediaPayloadProjectNotice
    notices = $mediaPayloadNotices
    sbom = $mediaPayloadSbom
    licenseReadme = $mediaPayloadLicenseReadme
    lineSeedLicense = $mediaPayloadLineSeedLicense
    zstandardLicense = $mediaPayloadZstandardLicense
    argon2License = $mediaPayloadArgon2License
    dataReadme = $mediaPayloadDataReadme
}
$mediaRootProductPayloadManifest = @(
    foreach ($entry in $mediaPayloadFiles.GetEnumerator()) {
        Assert-RegularNonReparseFile `
            -Path $entry.Value `
            -Description "媒体ルート製品payload $($entry.Key)"
        [ordered]@{
            name = $entry.Key
            relativePath = $entry.Value.Substring(
                $mediaRoot.Length).TrimStart('\')
            length = (Get-Item -LiteralPath $entry.Value).Length
            sha256 = (Get-FileHash -LiteralPath $entry.Value `
                -Algorithm SHA256).Hash
        }
    }
)
if ((Get-FileHash -LiteralPath $mediaPayloadApp -Algorithm SHA256).Hash -ne
        $appReport.sha256 -or
    (Get-FileHash -LiteralPath $mediaPayloadGui -Algorithm SHA256).Hash -ne
        $guiReport.sha256) {
    throw '媒体ルートへコピーしたWinPE製品EXEのSHA-256が一致しません。'
}
$mediaThirdPartyPayload = [ordered]@{
    projectLicense = $mediaPayloadProjectLicense
    projectNotice = $mediaPayloadProjectNotice
    notices = $mediaPayloadNotices
    sbom = $mediaPayloadSbom
    licenseReadme = $mediaPayloadLicenseReadme
    lineSeedLicense = $mediaPayloadLineSeedLicense
    zstandardLicense = $mediaPayloadZstandardLicense
    argon2License = $mediaPayloadArgon2License
}
foreach ($entry in $mediaThirdPartyPayload.GetEnumerator()) {
    if ((Get-FileHash -LiteralPath $entry.Value -Algorithm SHA256).Hash -ne
        $thirdPartyPayloadReport[$entry.Key].sha256) {
        throw "媒体ルートへコピーしたライセンス資料のSHA-256が一致しません: $($entry.Key)"
    }
}
$mediaPayloadDataItem = Get-Item -LiteralPath $mediaPayloadData -Force
if (-not $mediaPayloadDataItem.PSIsContainer -or
    ($mediaPayloadDataItem.Attributes -band
        [IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw '媒体ルートのEXE隣dataは通常フォルダーである必要があります。'
}
if (-not (Test-Path -LiteralPath $sourcesRoot)) {
    New-Item -ItemType Directory -Path $sourcesRoot | Out-Null
}
Copy-Item -LiteralPath $sourceWim -Destination $bootWim
New-Item -ItemType Directory -Path $mountRoot | Out-Null
Write-MediaProgress -Percent 24 -Stage 'staged-adk-media'

$stagedWimBefore = [ordered]@{
    length = (Get-Item -LiteralPath $bootWim).Length
    sha256 = (Get-FileHash -LiteralPath $bootWim -Algorithm SHA256).Hash
}

$mounted = $false
$uefiBootManagers = @()
try {
    Invoke-CheckedNative `
        -Command $dism `
        -Arguments @(
            '/Mount-Image',
            "/ImageFile:$bootWim",
            '/Index:1',
            "/MountDir:$mountRoot"
        ) `
        -Operation 'WinPE boot.wimのマウント'
    $mounted = $true
    Write-MediaProgress -Percent 38 -Stage 'mounted-wim'

    $normalBootManager = Join-Path `
        $mountRoot 'Windows\Boot\EFI\bootmgfw.efi'
    $uefiBootManagers += Copy-VerifiedMountedWimEfiFile `
        -MountRoot $mountRoot `
        -SourcePath $normalBootManager `
        -DestinationPath (Join-Path $bootBinsRoot 'bootmgfw.efi') `
        -FsutilPath $fsutil `
        -Description 'WinPE 2011 CA UEFIブートマネージャー'
    if ($CertificateGeneration -eq '2023CA') {
        $bootExManager = Join-Path `
            $mountRoot 'Windows\Boot\EFI_EX\bootmgfw_EX.efi'
        $uefiBootManagers += Copy-VerifiedMountedWimEfiFile `
            -MountRoot $mountRoot `
            -SourcePath $bootExManager `
            -DestinationPath (Join-Path $bootBinsRoot 'bootmgfw_EX.efi') `
            -FsutilPath $fsutil `
            -Description 'WinPE 2023 CA UEFIブートマネージャー'
    }

    Invoke-CheckedNative `
        -Command $dism `
        -Arguments @(
            "/Image:$mountRoot",
            '/Add-Package',
            "/PackagePath:$japaneseFontSupport"
        ) `
        -Operation 'WinPE日本語フォントサポートの追加'
    Write-MediaProgress -Percent 50 -Stage 'added-japanese-font'

    $payloadRoot = Join-Path $mountRoot 'YtecDiskClone'
    $mountedApp = Join-Path $payloadRoot 'ytec-winpe-app.exe'
    $mountedGui = Join-Path $payloadRoot 'ytec-winpe-gui.exe'
    $mountedProjectLicense = Join-Path $payloadRoot 'LICENSE'
    $mountedProjectNotice = Join-Path $payloadRoot 'NOTICE'
    $mountedNotices = Join-Path $payloadRoot 'THIRD-PARTY-NOTICES.txt'
    $mountedSbom = Join-Path $payloadRoot 'SBOM.spdx.json'
    $mountedLicenses = Join-Path $payloadRoot 'licenses'
    $mountedLicenseReadme = Join-Path $mountedLicenses 'README.md'
    $mountedLineSeedLicense = Join-Path $mountedLicenses `
        'LINE-Seed-JP-OFL-1.1.txt'
    $mountedZstandardLicense = Join-Path $mountedLicenses `
        'Zstandard-BSD-3-Clause.txt'
    $mountedArgon2License = Join-Path $mountedLicenses `
        'Argon2-Apache-2.0.txt'
    $mountedRescueMediaMarker = Join-Path $payloadRoot `
        'rescue-media-id.txt'
    $mountedData = Join-Path $payloadRoot 'data'
    $mountedDataReadme = Join-Path $mountedData 'README.txt'
    $launchScript = Join-Path $payloadRoot 'launch.cmd'
    $winpeshl = Join-Path $mountRoot 'Windows\System32\winpeshl.ini'
    foreach ($reserved in @($payloadRoot, $winpeshl)) {
        if (Test-Path -LiteralPath $reserved) {
            throw "標準WIM内の予約先が既に存在するため上書きしません: $reserved"
        }
    }

    Copy-Item `
        -LiteralPath $mediaPayloadRoot `
        -Destination $mountRoot `
        -Recurse
    $mountedDataItem = Get-Item -LiteralPath $mountedData -Force
    if (-not $mountedDataItem.PSIsContainer -or
        ($mountedDataItem.Attributes -band
            [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw 'WIM内のEXE隣dataは通常フォルダーである必要があります。'
    }
    @(
        '@echo off',
        'chcp 65001 >nul',
        'echo Y-TEC WinPE read-only disk inventory',
        '%SYSTEMDRIVE%\YtecDiskClone\ytec-winpe-app.exe --text',
        'echo.',
        'echo Read-only diagnostics completed. Use the Tsumugi GUI for verified clone/restore operations.'
    ) | Set-Content -LiteralPath $launchScript -Encoding ascii
    @(
        '[LaunchApps]',
        '%SYSTEMROOT%\System32\wpeinit.exe',
        '%SYSTEMDRIVE%\YtecDiskClone\ytec-winpe-app.exe, --launch-gui-from-media'
    ) | Set-Content -LiteralPath $winpeshl -Encoding ascii

    $mountedHash = (Get-FileHash -LiteralPath $mountedApp `
        -Algorithm SHA256).Hash
    if ($mountedHash -ne $appReport.sha256) {
        throw 'WIM内へコピーしたWinPEAppのSHA-256が元ファイルと一致しません。'
    }
    $mountedGuiHash = (Get-FileHash -LiteralPath $mountedGui `
        -Algorithm SHA256).Hash
    if ($mountedGuiHash -ne $guiReport.sha256) {
        throw 'WIM内へコピーしたWinPE GUIのSHA-256が元ファイルと一致しません。'
    }
    $mountedThirdPartyPayload = [ordered]@{
        projectLicense = $mountedProjectLicense
        projectNotice = $mountedProjectNotice
        notices = $mountedNotices
        sbom = $mountedSbom
        licenseReadme = $mountedLicenseReadme
        lineSeedLicense = $mountedLineSeedLicense
        zstandardLicense = $mountedZstandardLicense
        argon2License = $mountedArgon2License
    }
    foreach ($entry in $mountedThirdPartyPayload.GetEnumerator()) {
        Assert-RegularNonReparseFile `
            -Path $entry.Value `
            -Description "WIM内ライセンス資料 $($entry.Key)"
        if ((Get-FileHash -LiteralPath $entry.Value `
                -Algorithm SHA256).Hash -ne
            $thirdPartyPayloadReport[$entry.Key].sha256) {
            throw "WIM内へコピーしたライセンス資料のSHA-256が一致しません: $($entry.Key)"
        }
    }
    $thirdPartyPayloadManifest = @(
        foreach ($entry in $mountedThirdPartyPayload.GetEnumerator()) {
            [ordered]@{
                name = $entry.Key
                relativePath = $entry.Value.Substring(
                    $mountRoot.Length).TrimStart('\')
                length = (Get-Item -LiteralPath $entry.Value).Length
                sha256 = (Get-FileHash -LiteralPath $entry.Value `
                    -Algorithm SHA256).Hash
            }
        }
    )
    Assert-RegularNonReparseFile `
        -Path $mountedRescueMediaMarker `
        -Description 'WIM内レスキュー媒体マーカー'
    if ((Get-Item -LiteralPath $mountedRescueMediaMarker).Length -ne
            $rescueMediaMarkerReport.length -or
        (Get-FileHash -LiteralPath $mountedRescueMediaMarker `
            -Algorithm SHA256).Hash -ne
            $rescueMediaMarkerReport.sha256) {
        throw 'WIM内と媒体ルートのレスキュー媒体マーカーが一致しません。'
    }
    Write-MediaProgress -Percent 62 -Stage 'verified-product-payload'

    $payloadFiles = @(
        $mountedApp,
        $mountedGui,
        $mountedNotices,
        $mountedSbom,
        $mountedLicenseReadme,
        $mountedLineSeedLicense,
        $mountedZstandardLicense,
        $mountedArgon2License,
        $mountedDataReadme,
        $mountedRescueMediaMarker,
        $launchScript,
        $winpeshl)
    $addedFiles = @(
        foreach ($file in $payloadFiles) {
            [ordered]@{
                relativePath = $file.Substring($mountRoot.Length).TrimStart('\')
                length = (Get-Item -LiteralPath $file).Length
                sha256 = (Get-FileHash -LiteralPath $file `
                    -Algorithm SHA256).Hash
            }
        }
    )

    Invoke-CheckedNative `
        -Command $dism `
        -Arguments @(
            '/Unmount-Image',
            "/MountDir:$mountRoot",
            '/Commit',
            '/CheckIntegrity'
        ) `
        -Operation 'WinPE boot.wimのコミット'
    $mounted = $false
    Write-MediaProgress -Percent 74 -Stage 'committed-wim'
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
    throw 'WinPEApp追加後もboot.wimのSHA-256が変化していません。'
}

if ($BuildUsb) {
    $writeTarget = Get-VerifiedUsbTarget `
        -Drive $initialUsbTarget.drive `
        -DiskNumber $ExpectedUsbDiskNumber `
        -SizeBytes $ExpectedUsbSizeBytes `
        -SerialSuffix $ExpectedUsbSerialSuffix `
        -DeviceInstanceId $ExpectedUsbDeviceInstanceId `
        -Operation $UsbOperation `
        -DataFileSystem $UsbDataFileSystem
    if ($writeTarget.canonicalLayout -cne
            $initialUsbTarget.canonicalLayout -or
        -not $writeTarget.deviceInstanceId.Equals(
            $initialUsbTarget.deviceInstanceId,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw 'WIM準備中にUSBの安定識別情報または完全レイアウトが変化しました。'
    }

    Write-MediaProgress -Percent 88 -Stage 'writing-usb'
    $preparedTarget = if ($UsbOperation -eq 'Initialize') {
        Initialize-VerifiedUsbTarget `
            -Drive $writeTarget.drive `
            -DiskNumber $ExpectedUsbDiskNumber `
            -SizeBytes $ExpectedUsbSizeBytes `
            -SerialSuffix $ExpectedUsbSerialSuffix `
            -DeviceInstanceId $ExpectedUsbDeviceInstanceId `
            -DataFileSystem $UsbDataFileSystem
    } else {
        $writeTarget
    }
    if ($preparedTarget.partitionStyle -ne 'MBR' -or
        $preparedTarget.partitionNumber -ne 1 -or
        $preparedTarget.dataPartitionNumber -ne 2 -or
        $preparedTarget.bootFileSystem -ne 'FAT32' -or
        $preparedTarget.dataFileSystem -ne $UsbDataFileSystem) {
        throw '4GiB FAT32起動領域＋残容量データ領域を確認できませんでした。'
    }

    $sourceOwnership = New-RescueUsbOwnershipManifest `
        -MediaRoot $mediaRoot `
        -Target $preparedTarget `
        -MediaId $rescueMediaId `
        -DataFileSystem $UsbDataFileSystem `
        -FsutilPath $fsutil
    $updateResult = Invoke-RescueUsbBootUpdate `
        -Target $preparedTarget `
        -SourceRoot $mediaRoot `
        -SourceOwnership $sourceOwnership `
        -PreviousOwnership $initialUsbOwnership `
        -DataBefore $initialDataSummary `
        -FsutilPath $fsutil `
        -BootsectPath $bootsect `
        -Operation $UsbOperation
    $verifiedTarget = Get-VerifiedUsbTarget `
        -Drive $preparedTarget.drive `
        -DiskNumber $ExpectedUsbDiskNumber `
        -SizeBytes $ExpectedUsbSizeBytes `
        -SerialSuffix $ExpectedUsbSerialSuffix `
        -DeviceInstanceId $ExpectedUsbDeviceInstanceId `
        -Operation Refresh `
        -DataFileSystem $UsbDataFileSystem
    $sourceFiles = @(
        Get-ChildItem -LiteralPath $mediaRoot -Recurse -File -Force
    )
    if ($sourceFiles.Count -eq 0) {
        throw '検証するWinPE媒体ファイルがありません。'
    }
    $verifiedFiles = @(
        foreach ($sourceFile in $sourceFiles) {
            $relativePath = $sourceFile.FullName.Substring(
                $mediaRoot.Length).TrimStart('\')
            $usbPath = Join-Path $verifiedTarget.root $relativePath
            Assert-RegularNonReparseFile `
                -Path $usbPath `
                -Description "USB媒体ファイル $relativePath"
            $usbItem = Get-Item -LiteralPath $usbPath
            if ($usbItem.Length -ne $sourceFile.Length) {
                throw "USB媒体ファイルの長さが一致しません: $relativePath"
            }
            $sourceHash = (Get-FileHash `
                -LiteralPath $sourceFile.FullName `
                -Algorithm SHA256).Hash
            $usbHash = (Get-FileHash `
                -LiteralPath $usbItem.FullName `
                -Algorithm SHA256).Hash
            if ($usbHash -ne $sourceHash) {
                throw "USB媒体ファイルのSHA-256が一致しません: $relativePath"
            }
            [ordered]@{
                relativePath = $relativePath
                length = $usbItem.Length
                sha256 = $usbHash
            }
        }
    )
    foreach ($requiredRelativePath in @(
            'sources\boot.wim',
            'bootmgr',
            'EFI\BOOT\bootx64.efi')) {
        if ($verifiedFiles.relativePath -notcontains $requiredRelativePath) {
            throw "USBの必須起動ファイルを検証できません: $requiredRelativePath"
        }
    }

    $usbManifestPath = Join-Path `
        $outputFullPath 'usb-media-manifest.json'
    $usbBootWim = Join-Path `
        $verifiedTarget.root 'sources\boot.wim'
    $usbManifest = [ordered]@{
        schemaVersion = 1
        purpose = 'Y-TEC Tsumugi Drive WinPE rescue USB'
        generated = (Get-Date).ToString('o')
        repositoryContainsMicrosoftPayload = $false
        certificateGeneration = $CertificateGeneration
        validationScenario = $ValidationScenario
        adkVersion = [string]$candidate.deploymentToolsVersion
        dismVersion = [string]$candidate.dismFileVersion
        servicingUpdate = 'KB5101684'
        makeWinPEMedia = [ordered]@{
            path = $makeWinPEMedia
            length = (Get-Item -LiteralPath $makeWinPEMedia).Length
            sha256 = (Get-FileHash `
                -LiteralPath $makeWinPEMedia `
                -Algorithm SHA256).Hash
        }
        target = [ordered]@{
            diskNumber = $verifiedTarget.diskNumber
            drive = $verifiedTarget.drive
            dataDrive = $verifiedTarget.dataDrive
            sizeBytes = $verifiedTarget.sizeBytes
            serialSuffix = $verifiedTarget.serialSuffix
            partitionNumber = $verifiedTarget.partitionNumber
            dataPartitionNumber = $verifiedTarget.dataPartitionNumber
            canonicalLayout = $verifiedTarget.canonicalLayoutValue
        }
        storage = [ordered]@{
            operation = $UsbOperation
            bootPartitionBytes = $script:RescueUsbBootPartitionBytes
            bootFileSystem = 'FAT32'
            dataFileSystem = $UsbDataFileSystem
            dataUsesRemainingSpace = $true
        }
        dataPreservation = [ordered]@{
            preserved = ($UsbOperation -eq 'Refresh')
            fileCount = $updateResult.dataAfter.fileCount
            totalLogicalBytes = $updateResult.dataAfter.totalLogicalBytes
            beforeAfterMatch = $true
        }
        sourceWim = $preflight.sourceWim
        rescueMediaMarker = $rescueMediaMarkerReport
        mediaRootProductPayload = $mediaRootProductPayloadManifest
        thirdPartyPayload = $thirdPartyPayloadManifest
        stagedWimBefore = $stagedWimBefore
        addedFiles = $addedFiles
        stagedWimAfter = $stagedWimAfter
        winpeApp = $appReport
        verifiedFileCount = $verifiedFiles.Count
        verifiedFiles = $verifiedFiles
        bootWimSha256 = (Get-FileHash `
            -LiteralPath $usbBootWim `
            -Algorithm SHA256).Hash
        retainedWorkRoot = $outputFullPath
    }
    [IO.File]::WriteAllText(
        $usbManifestPath,
        ($usbManifest | ConvertTo-Json -Depth 10),
        [Text.UTF8Encoding]::new($false))
    Write-MediaProgress -Percent 94 -Stage 'verified-usb'
    Write-MediaProgress -Percent 100 -Stage 'completed-usb'
    Write-Output "WINPE_APP_USB_DRIVE=$($verifiedTarget.drive)"
    Write-Output "WINPE_APP_USB_PASS=$usbManifestPath"
    return
}

$bootData = '-bootdata:2#p0,e,b{0}#pEF,e,b{1}' -f `
    $etfsboot, $efiBootImage
Invoke-CheckedNative `
    -Command $oscdimg `
    -Arguments @($bootData, '-u1', '-udfver102', $mediaRoot, $isoPath) `
    -Operation 'WinPEApp検証ISOの作成'
Assert-RegularNonReparseFile -Path $isoPath -Description '生成ISO'
Write-MediaProgress -Percent 88 -Stage 'generated-iso'

$stagedIsoLength = (Get-Item -LiteralPath $isoPath).Length
$stagedIsoHash = (Get-FileHash -LiteralPath $isoPath `
    -Algorithm SHA256).Hash
$publishedIsoPath = if ([string]::IsNullOrWhiteSpace($finalIsoFullPath)) {
    $isoPath
} else {
    $finalIsoFullPath
}

$manifest = [ordered]@{
    schemaVersion = 1
    purpose = 'Y-TEC Tsumugi Drive WinPEApp validation media'
    generated = (Get-Date).ToString('o')
    repositoryContainsMicrosoftPayload = $false
    certificateGeneration = $CertificateGeneration
    validationScenario = $ValidationScenario
    adkVersion = [string]$candidate.deploymentToolsVersion
    dismVersion = [string]$candidate.dismFileVersion
    servicingUpdate = 'KB5101684'
    japaneseFontSupport = $preflight.japaneseFontSupport
    sourceWim = $preflight.sourceWim
    rescueMediaMarker = $rescueMediaMarkerReport
    mediaRootProductPayload = $mediaRootProductPayloadManifest
    thirdPartyPayload = $thirdPartyPayloadManifest
    uefiBootManagers = $uefiBootManagers
    stagedWimBefore = $stagedWimBefore
    addedFiles = $addedFiles
    stagedWimAfter = $stagedWimAfter
    winpeApp = $appReport
    iso = [ordered]@{
        path = $publishedIsoPath
        length = $stagedIsoLength
        sha256 = $stagedIsoHash
    }
    retainedWorkRoot = $outputFullPath
}
$manifestJson = $manifest | ConvertTo-Json -Depth 8
[IO.File]::WriteAllText(
    $manifestPath,
    $manifestJson,
    [Text.UTF8Encoding]::new($false))
Write-MediaProgress -Percent 94 -Stage 'verified-iso'

$publishedManifestPath = $manifestPath
if (-not [string]::IsNullOrWhiteSpace($finalIsoFullPath)) {
    [IO.File]::Move($isoPath, $finalIsoFullPath)
    Assert-RegularNonReparseFile `
        -Path $finalIsoFullPath `
        -Description '確定済みISO'
    $publishedHash = (Get-FileHash -LiteralPath $finalIsoFullPath `
        -Algorithm SHA256).Hash
    if ($publishedHash -ne $stagedIsoHash) {
        throw '完成名へ移動したISOのSHA-256がステージング時と一致しません。'
    }
    [IO.File]::Move($manifestPath, $finalManifestFullPath)
    $publishedManifestPath = $finalManifestFullPath
}

Write-MediaProgress -Percent 100 -Stage 'completed-iso'
Write-Output "WINPE_APP_MEDIA_PASS=$publishedManifestPath"
