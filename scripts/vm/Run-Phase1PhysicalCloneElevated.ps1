[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$workRoot = $PSScriptRoot
$harness = Join-Path $workRoot 'ytec-phase1-physical-clone-vm.exe'
$initScript = Join-Path $workRoot 'Initialize-Phase1SyntheticDisks.ps1'
$cloneScript = Join-Path $workRoot 'Invoke-Phase1PhysicalClone.ps1'
$validationScript = Join-Path $workRoot 'Test-Phase1SyntheticClone.ps1'
$summaryPath = Join-Path $workRoot 'interactive-summary.json'
$donePath = Join-Path $workRoot 'interactive.done.txt'
$utf8 = [Text.UTF8Encoding]::new($false)
$stage = 'startup'
$isElevated = $false

function Write-Utf8Text {
    param(
        [Parameter(Mandatory)]
        [string] $Path,
        [Parameter(Mandatory)]
        [string] $Value
    )

    [IO.File]::WriteAllText($Path, $Value, $utf8)
}

function Invoke-Phase1Step {
    param(
        [Parameter(Mandatory)]
        [string] $Name,
        [Parameter(Mandatory)]
        [string] $ScriptPath,
        [Parameter(Mandatory)]
        [string[]] $Arguments,
        [Parameter(Mandatory)]
        [string] $ResultPath,
        [Parameter(Mandatory)]
        [string] $StepDonePath
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

try {
    $principal = [Security.Principal.WindowsPrincipal]::new(
        [Security.Principal.WindowsIdentity]::GetCurrent())
    if (-not $principal.IsInRole(
            [Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'UACで昇格した管理者トークンではありません。'
    }
    $isElevated = $true

    foreach ($requiredFile in @(
        $harness,
        $initScript,
        $cloneScript,
        $validationScript
    )) {
        if (-not [IO.File]::Exists($requiredFile)) {
            throw "必要ファイルがありません: $requiredFile"
        }
    }

    $initResult = Join-Path $workRoot 'initialize.json'
    $initDone = Join-Path $workRoot 'initialize.done.txt'
    $stage = 'initialize'
    Invoke-Phase1Step `
        -Name '合成ディスク初期化' `
        -ScriptPath $initScript `
        -Arguments @(
            '-ResultPath', $initResult,
            '-DonePath', $initDone
        ) `
        -ResultPath $initResult `
        -StepDonePath $initDone
    $ready = [IO.File]::ReadAllText($initResult) | ConvertFrom-Json
    if ($ready.result -ne 'READY') {
        throw '合成ディスク初期化結果がREADYではありません。'
    }

    $sourceNumber = [int]$ready.sourceDiskNumber
    $targetNumber = [int]$ready.targetDiskNumber
    $planResult = Join-Path $workRoot 'plan.txt'
    $planDone = Join-Path $workRoot 'plan.done.txt'
    $stage = 'plan'
    Invoke-Phase1Step `
        -Name 'VM専用クローン計画' `
        -ScriptPath $cloneScript `
        -Arguments @(
            '-Mode', 'Plan',
            '-HarnessPath', $harness,
            '-SourceDiskNumber', $sourceNumber.ToString(),
            '-TargetDiskNumber', $targetNumber.ToString(),
            '-ResultPath', $planResult,
            '-DonePath', $planDone
        ) `
        -ResultPath $planResult `
        -StepDonePath $planDone
    $confirmationLine = @([IO.File]::ReadAllLines($planResult) | Where-Object {
        $_ -like 'confirmation=*'
    }) | Select-Object -Last 1
    if ($confirmationLine -notmatch '^confirmation=(.+)$') {
        throw 'VM専用クローン計画から確認文字列を取得できません。'
    }
    $confirmation = $Matches[1]

    $cloneResult = Join-Path $workRoot 'clone.txt'
    $cloneDone = Join-Path $workRoot 'clone.done.txt'
    $stage = 'clone'
    Invoke-Phase1Step `
        -Name 'VM専用物理クローン' `
        -ScriptPath $cloneScript `
        -Arguments @(
            '-Mode', 'Execute',
            '-HarnessPath', $harness,
            '-SourceDiskNumber', $sourceNumber.ToString(),
            '-TargetDiskNumber', $targetNumber.ToString(),
            '-Confirmation', $confirmation,
            '-ResultPath', $cloneResult,
            '-DonePath', $cloneDone
        ) `
        -ResultPath $cloneResult `
        -StepDonePath $cloneDone
    if ([IO.File]::ReadAllText($cloneResult) -notmatch 'YDC_VM_CLONE_PASS') {
        throw 'VM専用物理クローン結果にPASSマーカーがありません。'
    }

    $validationResult = Join-Path $workRoot 'validation.json'
    $validationDone = Join-Path $workRoot 'validation.done.txt'
    $stage = 'validation'
    Invoke-Phase1Step `
        -Name 'VM内コピー結果検証' `
        -ScriptPath $validationScript `
        -Arguments @(
            '-SourceDiskNumber', $sourceNumber.ToString(),
            '-TargetDiskNumber', $targetNumber.ToString(),
            '-ResultPath', $validationResult,
            '-DonePath', $validationDone
        ) `
        -ResultPath $validationResult `
        -StepDonePath $validationDone
    $validation = [IO.File]::ReadAllText($validationResult) | ConvertFrom-Json
    if ($validation.result -ne 'PASS') {
        throw 'VM内コピー結果がPASSではありません。'
    }

    $stage = 'complete'
    $summary = [ordered]@{
        schemaVersion = 1
        result = 'PASS'
        elevated = $isElevated
        sourceDiskNumber = $sourceNumber
        targetDiskNumber = $targetNumber
        sourceDiskBytes = [UInt64]$ready.sourceBytes
        targetDiskBytes = [UInt64]$ready.targetBytes
        sourcePartitionCount = [int]$ready.sourcePartitionCount
        diskGuidRegenerated = [bool]$validation.diskGuidRegenerated
        partitionGuidsRegenerated = [bool]$validation.partitionGuidsRegenerated
        partitionCount = [int]$validation.partitionCount
        completedUtc = [DateTimeOffset]::UtcNow
    } | ConvertTo-Json -Depth 4
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
    } | ConvertTo-Json -Depth 4
    Write-Utf8Text -Path $summaryPath -Value $failure
    Write-Utf8Text -Path $donePath -Value 'FAIL'
    exit 1
}
