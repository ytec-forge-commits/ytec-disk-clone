param(
    [Parameter(Mandatory)]
    [string]$OutputRoot,

    [Parameter(Mandatory)]
    [string]$LogPath,

    [ValidateSet('2011CA', '2023CA')]
    [string]$CertificateGeneration = '2023CA'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$OutputEncoding = [Text.UTF8Encoding]::new($false)

$builder = Join-Path $PSScriptRoot 'New-WinPEAppValidationMedia.ps1'
$logParent = Split-Path -Parent ([IO.Path]::GetFullPath($LogPath))
if (-not (Test-Path -LiteralPath $logParent -PathType Container)) {
    throw "ログ親フォルダーがありません: $logParent"
}

$exitCode = 0
Start-Transcript -LiteralPath $LogPath -Force | Out-Null
try {
    $diagnostic = Join-Path (Split-Path -Parent $PSScriptRoot) `
        'out\build\msvc-x64\src\MediaBuilder\ytec-winpe-environment.exe'
    $diagnosticCapture = $LogPath + '.adk.json'
    $diagnosticOutput = @(& $diagnostic --json)
    $diagnosticExitCode = $LASTEXITCODE
    [IO.File]::WriteAllText(
        $diagnosticCapture,
        ($diagnosticOutput -join [Environment]::NewLine),
        [Text.UTF8Encoding]::new($false))
    if ($diagnosticExitCode -ne 0) {
        throw "昇格環境のADK診断が終了コード $diagnosticExitCode で失敗しました。"
    }

    & $builder `
        -OutputRoot $OutputRoot `
        -CertificateGeneration $CertificateGeneration `
        -BuildMedia
} catch {
    $exitCode = 1
    Write-Error ($_ | Format-List * -Force | Out-String)
} finally {
    Stop-Transcript | Out-Null
}
exit $exitCode
