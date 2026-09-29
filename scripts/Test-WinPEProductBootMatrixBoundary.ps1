$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Get-ParsedScriptText {
    param([Parameter(Mandatory)][string] $Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "検証対象スクリプトがありません: $Path"
    }
    $tokens = $null
    $parseErrors = $null
    [Management.Automation.Language.Parser]::ParseFile(
        $Path,
        [ref]$tokens,
        [ref]$parseErrors) | Out-Null
    if ($parseErrors.Count -ne 0) {
        throw "スクリプトを解析できません: $Path : $($parseErrors[0].Message)"
    }
    return Get-Content -LiteralPath $Path -Raw
}

$matrixPath = Join-Path `
    $PSScriptRoot `
    'Invoke-WinPEProductBootMatrixVmTest.ps1'
$matrix = Get-ParsedScriptText -Path $matrixPath
foreach ($requiredMarker in @(
        "ValidatePattern('^YDC-WinPE-Boot-Matrix$')",
        "-Arguments @('list', 'runningvms')",
        'Assert-NetworkDisabled',
        'controllerNames.Count -ne 1',
        'hardDiskCount = 0',
        'physicalDiskOrUsbUsed = $false',
        "'CAPTURED_PENDING_VISUAL_INSPECTION'",
        'originalFirmware',
        'originalSecureBoot',
        'originalMedium',
        'runningVmCountAfter')) {
    if (-not $matrix.Contains($requiredMarker)) {
        throw "製品WinPE起動マトリクスの安全条件がありません: $requiredMarker"
    }
}

$builderPath = Join-Path `
    $PSScriptRoot `
    'New-WinPEAppValidationMedia.ps1'
$builder = Get-ParsedScriptText -Path $builderPath
foreach ($requiredMarker in @(
        'Assert-ExternalNewOutputPath',
        'Assert-MicrosoftSignature',
        'function Get-VerifiedUsbDisk',
        'function Initialize-VerifiedUsbTarget',
        'repositoryContainsMicrosoftPayload = $false',
        'Test-IsAdministrator',
        "'/Commit'",
        "'/CheckIntegrity'")) {
    if (-not $builder.Contains($requiredMarker)) {
        throw "製品WinPE生成の安全条件がありません: $requiredMarker"
    }
}

foreach ($source in @($matrix, $builder)) {
    foreach ($forbiddenPattern in @(
            '(?i)\bunregistervm\b',
            '(?i)\bclosemedium\b',
            '(?i)\brawdisk\b',
            '(?i)\bhostdrive\b',
            '(?i)\busbattach\b',
            '(?i)\bsnapshot\s+delete\b',
            '(?i)--job-',
            '(?i)job-manifest')) {
        if ($source -match $forbiddenPattern) {
            throw "現行WinPE検証経路に禁止された旧方式またはVM操作があります: $forbiddenPattern"
        }
    }
}

Write-Output 'WinPE product boot matrix boundary: PASS'
