[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidatePattern('^[A-Fa-f0-9]{64}$')]
    [string] $ExpectedHarnessSha256
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$workRoot = [IO.Path]::GetFullPath($PSScriptRoot)
$harness = Join-Path $workRoot 'ytec-phase5-vss-live-vm.exe'
$summaryPath = Join-Path $workRoot 'summary.json'
$donePath = Join-Path $workRoot 'done.txt'
$harnessStdoutPath = Join-Path $workRoot 'harness-stdout.txt'
$harnessStderrPath = Join-Path $workRoot 'harness-stderr.txt'
$beforePath = Join-Path $workRoot 'shadows-before.json'
$afterPath = Join-Path $workRoot 'shadows-after.json'
$sentinelRoot = 'C:\YDC-VSS-Validation'
$sentinelPath = Join-Path $sentinelRoot 'sentinel.txt'
$authorization = 'YDC_PHASE5_VSS_VM_ONLY'
$expectedSentinel = "YDC_PHASE5_VM_SENTINEL_20260730`r`n"
$utf8 = [Text.UTF8Encoding]::new($false)
$stage = 'startup'
$harnessStarted = $false
$harnessExitCode = $null
$beforeCount = $null
$afterCount = $null

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

function Get-SanitizedShadowCopies {
    $copies = @(Get-CimInstance `
        -ClassName Win32_ShadowCopy `
        -ErrorAction Stop)
    return @($copies | ForEach-Object {
        [ordered]@{
            id = [string]$_.ID
            setId = [string]$_.SetID
            volumeName = [string]$_.VolumeName
            installDate = if ($null -eq $_.InstallDate) {
                $null
            }
            else {
                ([DateTimeOffset]$_.InstallDate).ToUniversalTime().
                    ToString('O')
            }
        }
    })
}

function Write-ShadowEvidence {
    param(
        [Parameter(Mandatory)]
        [string] $Path,
        [Parameter(Mandatory)]
        [AllowEmptyCollection()]
        [object[]] $Copies
    )

    $payload = [ordered]@{
        schemaVersion = 1
        count = $Copies.Count
        copies = @($Copies)
        observedUtc = [DateTimeOffset]::UtcNow
    }
    Write-Utf8Text `
        -Path $Path `
        -Value ($payload | ConvertTo-Json -Depth 6)
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
        throw 'NICが有効なためライブVSS検証を開始しません。'
    }
    if ($env:SystemDrive -cne 'C:') {
        throw '固定C:がシステムドライブではありません。'
    }
    $systemVolume = Get-CimInstance `
        -ClassName Win32_LogicalDisk `
        -Filter "DeviceID = 'C:'" `
        -ErrorAction Stop
    if ($null -eq $systemVolume -or
        [string]$systemVolume.FileSystem -cne 'NTFS') {
        throw '固定C:がNTFSではありません。'
    }
    if (-not [IO.File]::Exists($harness)) {
        throw "VSS検証ハーネスがありません: $harness"
    }
    $actualHarnessSha256 = (
        Get-FileHash -LiteralPath $harness -Algorithm SHA256).Hash
    if ($actualHarnessSha256 -cne $ExpectedHarnessSha256.ToUpperInvariant()) {
        throw 'VSS検証ハーネスのSHA-256がホスト側期待値と一致しません。'
    }

    $stage = 'sentinel'
    [IO.Directory]::CreateDirectory($sentinelRoot) | Out-Null
    $expectedSentinelBytes = [Text.Encoding]::ASCII.GetBytes(
        $expectedSentinel)
    [IO.File]::WriteAllBytes($sentinelPath, $expectedSentinelBytes)
    $actualSentinelBytes = [IO.File]::ReadAllBytes($sentinelPath)
    if (-not [Linq.Enumerable]::SequenceEqual(
            [byte[]]$expectedSentinelBytes,
            [byte[]]$actualSentinelBytes)) {
        throw '固定合成Sentinelの読戻し検証に失敗しました。'
    }

    $stage = 'shadow-before'
    $before = @(Get-SanitizedShadowCopies)
    $beforeCount = $before.Count
    Write-ShadowEvidence -Path $beforePath -Copies $before
    if ($beforeCount -ne 0) {
        throw '開始前に既存Shadow Copyがあるため、削除せず停止しました。'
    }

    $stage = 'harness'
    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $harness
    $startInfo.Arguments = "--authorize $authorization"
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    if (-not $process.Start()) {
        throw 'VSS検証ハーネスを開始できませんでした。'
    }
    $harnessStarted = $true
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit(240000)) {
        try {
            $process.Kill()
        }
        catch {
            # Timeoutが正本であり、停止試行失敗で置き換えない。
        }
        throw 'VSS検証ハーネスが4分以内に完了しませんでした。'
    }
    $harnessExitCode = $process.ExitCode
    $stdout = $stdoutTask.GetAwaiter().GetResult()
    $stderr = $stderrTask.GetAwaiter().GetResult()
    Write-Utf8Text -Path $harnessStdoutPath -Value $stdout
    Write-Utf8Text -Path $harnessStderrPath -Value $stderr
    if ($harnessExitCode -ne 0) {
        throw "VSS検証ハーネスが終了コード${harnessExitCode}で失敗しました。"
    }
    if ($stdout -notmatch 'YDC_PHASE5_VSS_VM_PASS') {
        throw 'VSS検証ハーネスのPASSマーカーがありません。'
    }

    $stage = 'shadow-after'
    $after = @(Get-SanitizedShadowCopies)
    $afterCount = $after.Count
    Write-ShadowEvidence -Path $afterPath -Copies $after
    if ($afterCount -ne 0) {
        throw '終了後にShadow Copyが残留しています。自動削除せず停止しました。'
    }

    $stage = 'complete'
    $summary = [ordered]@{
        schemaVersion = 1
        result = 'PASS'
        stage = $stage
        virtualBox = $true
        nicEnabledCount = 0
        systemDrive = 'C:'
        fileSystem = 'NTFS'
        harnessSha256 = $actualHarnessSha256
        harnessStarted = $harnessStarted
        harnessExitCode = $harnessExitCode
        shadowsBefore = $beforeCount
        shadowsAfter = $afterCount
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
    if (-not [IO.File]::Exists($afterPath)) {
        try {
            $after = @(Get-SanitizedShadowCopies)
            $afterCount = $after.Count
            Write-ShadowEvidence -Path $afterPath -Copies $after
        }
        catch {
            # 元の失敗を正本にし、確認不能はsummaryへ残す。
        }
    }
    $summary = [ordered]@{
        schemaVersion = 1
        result = 'FAIL'
        stage = $stage
        message = $failure
        harnessStarted = $harnessStarted
        harnessExitCode = $harnessExitCode
        shadowsBefore = $beforeCount
        shadowsAfter = $afterCount
        completedUtc = [DateTimeOffset]::UtcNow
    }
    Write-Utf8Text `
        -Path $summaryPath `
        -Value ($summary | ConvertTo-Json -Depth 5)
    Write-Utf8Text -Path $donePath -Value 'FAIL'
    exit 1
}
