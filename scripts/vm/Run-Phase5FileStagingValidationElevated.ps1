[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidatePattern('^[A-Fa-f0-9]{64}$')]
    [string] $ExpectedHarnessSha256,
    [switch] $ReusePreparedDestination
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$workRoot = [IO.Path]::GetFullPath($PSScriptRoot)
$harness = Join-Path $workRoot 'ytec-phase5-file-staging-vm.exe'
$summaryPath = Join-Path $workRoot 'summary.json'
$donePath = Join-Path $workRoot 'done.txt'
$harnessStdoutPath = Join-Path $workRoot 'harness-stdout.txt'
$harnessStderrPath = Join-Path $workRoot 'harness-stderr.txt'
$finalPath = 'T:\YDC-Phase5-File-Staging\synthetic.dcimg'
$partialPath = "$finalPath.partial"
$authorization = 'YDC_PHASE5_FILE_STAGING_VM_ONLY'
$sourceBytes = [UInt64](128MB)
$destinationBytes = [UInt64](512MB)
$utf8 = [Text.UTF8Encoding]::new($false)
$stage = 'startup'
$harnessStarted = $false
$harnessExitCode = $null

function Write-Utf8Text {
    param(
        [Parameter(Mandatory)]
        [string] $Path,
        [Parameter(Mandatory)]
        [AllowEmptyString()]
        [string] $Value
    )

    [IO.File]::WriteAllText($Path, $Value, $utf8)
}

function Test-Administrator {
    $principal = [Security.Principal.WindowsPrincipal]::new(
        [Security.Principal.WindowsIdentity]::GetCurrent())
    return $principal.IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Test-VirtualBoxGuest {
    $system = Get-CimInstance `
        -ClassName Win32_ComputerSystem `
        -ErrorAction Stop
    $identity = "$($system.Manufacturer) $($system.Model)"
    return $identity -match '(?i)(VirtualBox|innotek)'
}

try {
    if (-not (Test-Administrator)) {
        throw 'UACで昇格した管理者トークンではありません。'
    }
    if (-not (Test-VirtualBoxGuest)) {
        throw 'このランナーはVirtualBox専用VM以外では実行できません。'
    }

    $activeAdapters = @(Get-CimInstance `
        -ClassName Win32_NetworkAdapter `
        -Filter 'NetEnabled = True' `
        -ErrorAction Stop)
    if ($activeAdapters.Count -ne 0) {
        throw 'NICが有効なため実ファイル検証を開始しません。'
    }
    if ($env:SystemDrive -cne 'C:') {
        throw '固定C:がシステムドライブではありません。'
    }
    if (-not [IO.File]::Exists($harness)) {
        throw "実ファイル検証ハーネスがありません: $harness"
    }
    $actualHarnessSha256 = (
        Get-FileHash -LiteralPath $harness -Algorithm SHA256).Hash
    if ($actualHarnessSha256 -cne $ExpectedHarnessSha256.ToUpperInvariant()) {
        throw '実ファイル検証ハーネスのSHA-256がホスト側期待値と一致しません。'
    }

    $stage = 'disk-gate'
    $disks = @(Get-Disk -ErrorAction Stop)
    if ($disks.Count -ne 3) {
        throw '固定3台構成ではないため、合成保存先を初期化しません。'
    }
    $systemDisks = @($disks | Where-Object {
        $_.IsSystem -or $_.IsBoot
    })
    $sources = @($disks | Where-Object {
        -not $_.IsSystem -and
        -not $_.IsBoot -and
        [UInt64]$_.Size -eq $sourceBytes -and
        [string]$_.PartitionStyle -ceq 'RAW' -and
        [int]$_.NumberOfPartitions -eq 0
    })
    if ($ReusePreparedDestination) {
        $destinations = @($disks | Where-Object {
            -not $_.IsSystem -and
            -not $_.IsBoot -and
            [UInt64]$_.Size -eq $destinationBytes -and
            [string]$_.PartitionStyle -ceq 'MBR' -and
            [int]$_.NumberOfPartitions -eq 1 -and
            -not $_.IsReadOnly -and
            -not $_.IsOffline
        })
    }
    else {
        $destinations = @($disks | Where-Object {
            -not $_.IsSystem -and
            -not $_.IsBoot -and
            [UInt64]$_.Size -eq $destinationBytes -and
            [string]$_.PartitionStyle -ceq 'RAW' -and
            [int]$_.NumberOfPartitions -eq 0 -and
            -not $_.IsReadOnly -and
            -not $_.IsOffline
        })
    }
    if ($systemDisks.Count -ne 1 -or
        $sources.Count -ne 1 -or
        $destinations.Count -ne 1) {
        throw 'システム/128MiB RAWソース/512MiB RAW保存先の固定構成が一致しません。'
    }
    if ($sources[0].Number -eq $destinations[0].Number) {
        throw '固定合成ソースと保存先のディスク番号が同一です。'
    }
    $destination = $destinations[0]
    if (-not $ReusePreparedDestination) {
        if (Get-Volume -DriveLetter T -ErrorAction SilentlyContinue) {
            throw '初期化前からT:が存在するため、保存先を変更しません。'
        }

        $stage = 'destination-format'
        $initialized = $destination |
            Initialize-Disk -PartitionStyle MBR -PassThru -ErrorAction Stop
        $partition = $initialized |
            New-Partition `
                -UseMaximumSize `
                -DriveLetter T `
                -ErrorAction Stop
        $null = $partition |
            Format-Volume `
                -FileSystem NTFS `
                -NewFileSystemLabel 'YDC_PHASE5_STAGE' `
                -Confirm:$false `
                -Force `
                -ErrorAction Stop
    }

    $stage = 'destination-volume-gate'
    $mappedDisk = Get-Partition -DriveLetter T -ErrorAction Stop |
        Get-Disk -ErrorAction Stop
    if ([UInt64]$mappedDisk.Size -ne $destinationBytes -or
        $mappedDisk.Number -ne $destination.Number -or
        $mappedDisk.IsSystem -or
        $mappedDisk.IsBoot) {
        throw '初期化後T:が固定512MiB合成保存先へ対応していません。'
    }
    $volume = Get-Volume -DriveLetter T -ErrorAction Stop
    if ([string]$volume.FileSystem -cne 'NTFS' -or
        [string]$volume.FileSystemLabel -cne 'YDC_PHASE5_STAGE') {
        throw '固定T:のNTFS形式またはラベルが一致しません。'
    }

    $stage = 'output-preflight'
    $outputRoot = Split-Path -Parent $finalPath
    [IO.Directory]::CreateDirectory($outputRoot) | Out-Null
    if ([IO.File]::Exists($finalPath) -or
        [IO.File]::Exists($partialPath)) {
        throw '新規保存先に完成または未完了dcimgが既にあります。'
    }

    $stage = 'harness'
    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $harness
    $startInfo.Arguments = "--authorize $authorization"
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.StandardOutputEncoding = [Text.Encoding]::UTF8
    $startInfo.StandardErrorEncoding = [Text.Encoding]::UTF8
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    if (-not $process.Start()) {
        throw '実ファイル検証ハーネスを開始できませんでした。'
    }
    $harnessStarted = $true
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit(120000)) {
        try {
            $process.Kill()
        }
        catch {
            # Timeoutを正本とし、停止試行失敗で置き換えない。
        }
        throw '実ファイル検証ハーネスが2分以内に完了しませんでした。'
    }
    $harnessExitCode = $process.ExitCode
    $stdout = $stdoutTask.GetAwaiter().GetResult()
    $stderr = $stderrTask.GetAwaiter().GetResult()
    Write-Utf8Text -Path $harnessStdoutPath -Value $stdout
    Write-Utf8Text -Path $harnessStderrPath -Value $stderr
    if ($harnessExitCode -ne 0) {
        throw "実ファイル検証ハーネスが終了コード${harnessExitCode}で失敗しました。"
    }
    if ($stdout -notmatch 'YDC_PHASE5_FILE_STAGING_VM_PASS') {
        throw '実ファイル検証ハーネスのPASSマーカーがありません。'
    }

    $stage = 'final-verification'
    if (-not [IO.File]::Exists($finalPath)) {
        throw '確定済みdcimgがありません。'
    }
    if ([IO.File]::Exists($partialPath)) {
        throw '確定後に未完了partialが残っています。'
    }
    $finalInfo = Get-Item -LiteralPath $finalPath -ErrorAction Stop
    if ([UInt64]$finalInfo.Length -le 1MB -or
        [UInt64]$finalInfo.Length -gt 4MB) {
        throw '確定済みdcimgが固定1MiB超から4MiB以下の範囲外です。'
    }
    $finalSha256 = (
        Get-FileHash -LiteralPath $finalPath -Algorithm SHA256).Hash

    $stage = 'complete'
    $summary = [ordered]@{
        schemaVersion = 1
        result = 'PASS'
        stage = $stage
        virtualBox = $true
        nicEnabledCount = 0
        systemDrive = 'C:'
        sourceDiskNumber = [int]$sources[0].Number
        sourceDiskBytes = $sourceBytes
        destinationDiskNumber = [int]$destination.Number
        destinationDiskBytes = $destinationBytes
        destinationDrive = 'T:'
        destinationFileSystem = 'NTFS'
        harnessSha256 = $actualHarnessSha256
        harnessStarted = $harnessStarted
        harnessExitCode = $harnessExitCode
        finalPath = $finalPath
        finalBytes = [UInt64]$finalInfo.Length
        finalSha256 = $finalSha256
        partialExists = $false
        passMarker = $true
        completedUtc = [DateTimeOffset]::UtcNow
    }
    Write-Utf8Text `
        -Path $summaryPath `
        -Value ($summary | ConvertTo-Json -Depth 5)
    Write-Utf8Text -Path $donePath -Value 'PASS'
    exit 0
}
catch {
    $failure = $_.Exception.Message
    $summary = [ordered]@{
        schemaVersion = 1
        result = 'FAIL'
        stage = $stage
        message = $failure
        harnessStarted = $harnessStarted
        harnessExitCode = $harnessExitCode
        finalExists = [IO.File]::Exists($finalPath)
        partialExists = [IO.File]::Exists($partialPath)
        completedUtc = [DateTimeOffset]::UtcNow
    }
    Write-Utf8Text `
        -Path $summaryPath `
        -Value ($summary | ConvertTo-Json -Depth 5)
    Write-Utf8Text -Path $donePath -Value 'FAIL'
    exit 1
}
