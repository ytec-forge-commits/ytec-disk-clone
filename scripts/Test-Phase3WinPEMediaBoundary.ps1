param(
    [string]$BaseProductMediaRoot
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$builder = Join-Path $PSScriptRoot 'New-Phase3WinPEValidationMedia.ps1'
$wrapper = Join-Path $PSScriptRoot 'Invoke-Phase3WinPEMediaElevated.ps1'

foreach ($file in @($builder, $wrapper)) {
    $tokens = $null
    $parseErrors = $null
    [Management.Automation.Language.Parser]::ParseFile(
        $file,
        [ref]$tokens,
        [ref]$parseErrors) | Out-Null
    if ($parseErrors.Count -ne 0) {
        throw "Phase 3 WinPE media script parse failed: $($parseErrors[0].Message)"
    }
}

$scriptText = Get-Content -LiteralPath $builder -Raw
$requiredMarkers = @(
    'YTEC-VM-ONLY-PHASE3-MBR-CLONE',
    'YTEC-VM-ONLY-PHASE3-BIOS-BCDBOOT',
    '--source 0 --target 1 --legacy-bios-test',
    'YDC_PHASE3_AUTOMATION_PASS',
    'type "%LOG%">COM1',
    'productWriteServiceConnected = $false',
    'repositoryContainsMicrosoftPayload = $false'
)
foreach ($marker in $requiredMarkers) {
    if (-not $scriptText.Contains($marker)) {
        throw "Phase 3 WinPE media scriptの固定安全境界がありません: $marker"
    }
}
foreach ($forbidden in @(
        '/allowFullOS', 'mbr2gpt.exe', 'copype.cmd', 'findstr')) {
    if ($scriptText.Contains($forbidden)) {
        throw "Phase 3 WinPE media scriptに禁止境界があります: $forbidden"
    }
}

function Assert-RejectedOutput {
    param(
        [Parameter(Mandatory)][string]$OutputRoot,
        [Parameter(Mandatory)][string]$ExpectedMessage
    )
    try {
        & $builder `
            -BaseProductMediaRoot $env:TEMP `
            -OutputRoot $OutputRoot
        throw "拒否されるべき出力先が許可されました: $OutputRoot"
    } catch {
        if ($_.Exception.Message -notlike "*$ExpectedMessage*") {
            throw "想定外の拒否理由です: $($_.Exception.Message)"
        }
    }
}

Assert-RejectedOutput `
    -OutputRoot (Join-Path $repoRoot 'out\forbidden-phase3-winpe') `
    -ExpectedMessage 'リポジトリ外'
Assert-RejectedOutput `
    -OutputRoot ([IO.Path]::GetPathRoot($repoRoot)) `
    -ExpectedMessage 'ドライブ直下'
Assert-RejectedOutput `
    -OutputRoot $env:TEMP `
    -ExpectedMessage '既存の出力先'

if (-not [string]::IsNullOrWhiteSpace($BaseProductMediaRoot)) {
    $preflightOutput = Join-Path $env:LOCALAPPDATA (
        'YTEC\ytec-disk-clone\phase3-preflight-only\' +
        [guid]::NewGuid().ToString('N'))
    $result = & $builder `
        -BaseProductMediaRoot $BaseProductMediaRoot `
        -OutputRoot $preflightOutput
    if ($result -notlike 'PHASE3_WINPE_MEDIA_PREFLIGHT_PASS=*') {
        throw "Phase 3事前検証の成功マーカーがありません: $result"
    }
    if (Test-Path -LiteralPath $preflightOutput) {
        throw 'Phase 3事前検証だけで出力先が作成されました。'
    }
}

Write-Output 'Phase 3 WinPE media boundary tests: PASS'
