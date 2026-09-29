param(
    [Parameter(Mandatory)]
    [string]$BaseProductMediaRoot,

    [Parameter(Mandatory)]
    [string]$OutputRoot,

    [Parameter(Mandatory)]
    [string]$LogPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$OutputEncoding = [Text.UTF8Encoding]::new($false)

$builder = Join-Path $PSScriptRoot 'New-Phase3WinPEValidationMedia.ps1'
$logParent = Split-Path -Parent ([IO.Path]::GetFullPath($LogPath))
if (-not (Test-Path -LiteralPath $logParent -PathType Container)) {
    throw "ログ親フォルダーがありません: $logParent"
}

$exitCode = 0
Start-Transcript -LiteralPath $LogPath -Force | Out-Null
try {
    & $builder `
        -BaseProductMediaRoot $BaseProductMediaRoot `
        -OutputRoot $OutputRoot `
        -BuildMedia
} catch {
    $exitCode = 1
    Write-Error ($_ | Format-List * -Force | Out-String)
} finally {
    Stop-Transcript | Out-Null
}
exit $exitCode
