param(
    [string]$BaseProductMediaRoot
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$builder = Join-Path $PSScriptRoot 'New-Phase4WinPEValidationMedia.ps1'
$wrapper = Join-Path $PSScriptRoot 'Invoke-Phase4WinPEMediaElevated.ps1'

foreach ($file in @($builder, $wrapper)) {
    $tokens = $null
    $parseErrors = $null
    [Management.Automation.Language.Parser]::ParseFile(
        $file,
        [ref]$tokens,
        [ref]$parseErrors) | Out-Null
    if ($parseErrors.Count -ne 0) {
        throw "Phase 4 WinPE media script parse failed: $($parseErrors[0].Message)"
    }
}

$scriptText = Get-Content -LiteralPath $builder -Raw
foreach ($marker in @(
        'YTEC-VM-ONLY-PHASE4-MBR2GPT',
        'YTEC-VM-ONLY-PHASE4-UEFI-BCDBOOT',
        '--target 0 --windows-root',
        '> "%ROOT%\mount-esp.txt" echo select disk 0',
        '>>"%ROOT%\mount-esp.txt" echo select partition 2',
        'diskpart.exe /s "%ROOT%\mount-esp.txt"',
        'diskpart.exe /s "%ROOT%\unmount-esp.txt"',
        'conversionSkipped=true reason=explicit-bcdboot-resume-profile',
        'YDC_PHASE4_AUTOMATION_PASS',
        'YDC_PHASE4_BCDBOOT_RESUME_PASS',
        'type "%LOG%">COM1',
        'productWriteServiceConnected = $false',
        'repositoryContainsMicrosoftPayload = $false')) {
    if (-not $scriptText.Contains($marker)) {
        throw "Phase 4 WinPE media scriptの固定安全境界がありません: $marker"
    }
}
foreach ($forbidden in @(
        '/allowFullOS', '/map:', 'findstr', 'mountvol.exe S: /S',
        '--source 0 --target 1', 'clean', 'format')) {
    if ($scriptText.Contains($forbidden)) {
        throw "Phase 4 WinPE media scriptに禁止境界があります: $forbidden"
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
    -OutputRoot (Join-Path $repoRoot 'out\forbidden-phase4-winpe') `
    -ExpectedMessage 'リポジトリ外'
Assert-RejectedOutput `
    -OutputRoot ([IO.Path]::GetPathRoot($repoRoot)) `
    -ExpectedMessage 'ドライブ直下'
Assert-RejectedOutput `
    -OutputRoot $env:TEMP `
    -ExpectedMessage '既存の出力先'

if (-not [string]::IsNullOrWhiteSpace($BaseProductMediaRoot)) {
    $preflightOutput = Join-Path $env:LOCALAPPDATA (
        'YTEC\ytec-disk-clone\phase4-preflight-only\' +
        [guid]::NewGuid().ToString('N'))
    $result = & $builder `
        -BaseProductMediaRoot $BaseProductMediaRoot `
        -OutputRoot $preflightOutput
    if ($result -notlike 'PHASE4_WINPE_MEDIA_PREFLIGHT_PASS=*') {
        throw "Phase 4事前検証の成功マーカーがありません: $result"
    }
    if (Test-Path -LiteralPath $preflightOutput) {
        throw 'Phase 4事前検証だけで出力先が作成されました。'
    }
}

Write-Output 'Phase 4 WinPE media boundary tests: PASS'
