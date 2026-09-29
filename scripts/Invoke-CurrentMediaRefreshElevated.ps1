param(
    [Parameter(Mandatory)][string] $Output2011,
    [Parameter(Mandatory)][string] $Output2023,
    [Parameter(Mandatory)][string] $LogPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$exitCode = 0
Start-Transcript -LiteralPath $LogPath -Force | Out-Null
try {
    $productBuilder = Join-Path $PSScriptRoot (
        'New-WinPEAppValidationMedia.ps1'
    )
    & $productBuilder `
        -OutputRoot $Output2011 `
        -CertificateGeneration 2011CA `
        -BuildMedia
    if ($LASTEXITCODE -ne 0) {
        throw 'The current 2011 CA product media build failed.'
    }
    & $productBuilder `
        -OutputRoot $Output2023 `
        -CertificateGeneration 2023CA `
        -BuildMedia
    if ($LASTEXITCODE -ne 0) {
        throw 'The current 2023 CA product media build failed.'
    }
}
catch {
    $exitCode = 1
    Write-Error ($_ | Format-List * -Force | Out-String)
}
finally {
    Stop-Transcript | Out-Null
}
exit $exitCode
