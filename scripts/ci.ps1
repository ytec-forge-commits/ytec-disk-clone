param(
    [switch]$SkipAnalysis,
    [switch]$SkipAsan
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repoRoot

. (Join-Path $PSScriptRoot 'Enter-MsvcEnvironment.ps1')

function Invoke-NativeCommand {
    param(
        [Parameter(Mandatory)]
        [string]$Command,
        [Parameter(ValueFromRemainingArguments)]
        [string[]]$Arguments
    )

    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Command が終了コード $LASTEXITCODE で失敗しました。"
    }
}

function Test-PowerShellScriptEncoding {
    $scripts = Get-ChildItem -LiteralPath $PSScriptRoot -Filter '*.ps1' -File
    foreach ($script in $scripts) {
        $bytes = [IO.File]::ReadAllBytes($script.FullName)
        $hasUtf8Bom = $bytes.Length -ge 3 -and
            $bytes[0] -eq 0xEF -and
            $bytes[1] -eq 0xBB -and
            $bytes[2] -eq 0xBF
        if (-not $hasUtf8Bom) {
            throw "PowerShell script must use UTF-8 BOM for Windows PowerShell 5.1: $($script.Name)"
        }
    }
    Write-Output 'PowerShell UTF-8 BOM check: PASS'
}

Test-PowerShellScriptEncoding
& (Join-Path $PSScriptRoot 'Test-ProductVersion.ps1')
Invoke-NativeCommand cmake --preset msvc-x64
Invoke-NativeCommand cmake --build --preset msvc-x64 --clean-first
Invoke-NativeCommand ctest --preset msvc-x64

# WinPE media uses the static MSVC runtime.  Build and test this tree on every
# CI run so media validation cannot accidentally inspect a stale executable.
Invoke-NativeCommand cmake --preset msvc-x64-vm
Invoke-NativeCommand cmake --build --preset msvc-x64-vm --clean-first
Invoke-NativeCommand ctest --preset msvc-x64-vm

& (Join-Path $PSScriptRoot 'check-licenses.ps1')
& (Join-Path $PSScriptRoot 'check-safety-boundary.ps1')
& (Join-Path $PSScriptRoot 'generate-sbom.ps1') -Check
& (Join-Path $PSScriptRoot 'Test-WinPEAppMediaBoundary.ps1')
& (Join-Path $PSScriptRoot 'Test-WinPEProductBootMatrixBoundary.ps1')
& (Join-Path $PSScriptRoot 'Test-PortablePackageBoundary.ps1')
& (Join-Path $PSScriptRoot 'Test-Phase3WinPEMediaBoundary.ps1')
& (Join-Path $PSScriptRoot 'Test-Phase4WinPEMediaBoundary.ps1')

if (-not $SkipAnalysis) {
    Invoke-NativeCommand cmake --preset msvc-x64-analysis
    Invoke-NativeCommand cmake --build --preset msvc-x64-analysis --clean-first
}

if (-not $SkipAsan) {
    Invoke-NativeCommand cmake --preset msvc-x64-asan
    Invoke-NativeCommand cmake --build --preset msvc-x64-asan --clean-first
    Invoke-NativeCommand ctest --preset msvc-x64-asan
}

Write-Output 'Y-TEC Tsumugi Drive CI checks: PASS'
