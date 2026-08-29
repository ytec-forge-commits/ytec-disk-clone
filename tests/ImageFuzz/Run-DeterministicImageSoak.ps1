[CmdletBinding()]
param(
    [ValidateSet('msvc-x64', 'msvc-x64-analysis', 'msvc-x64-asan')]
    [string]$Preset = 'msvc-x64-asan',

    [ValidateRange(1, 100000000)]
    [UInt64]$IterationsPerBatch = 250000,

    [ValidateRange(1, 16)]
    [int]$BatchCount = 8
)

$ErrorActionPreference = 'Stop'

$repositoryRoot = [IO.Path]::GetFullPath(
    (Join-Path $PSScriptRoot '..\..'))
. (Join-Path $repositoryRoot 'scripts\Enter-MsvcEnvironment.ps1')

$executable = Join-Path $repositoryRoot (
    'out\build\{0}\tests\ytec-image-fuzz-tests.exe' -f $Preset)
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "ImageFuzz executable not found. Build preset '$Preset' first: $executable"
}

# Fixed independent campaigns make a failure replayable from the emitted seed.
# This runner is intentionally not described as coverage-guided fuzzing.
$campaignSeeds = @(
    '0x5954454346555A5A',
    '0x243F6A8885A308D3',
    '0x13198A2E03707344',
    '0xA4093822299F31D0',
    '0x082EFA98EC4E6C89',
    '0x452821E638D01377',
    '0xBE5466CF34E90C6C',
    '0xC0AC29B7C97C50DD',
    '0x3F84D5B5B5470917',
    '0x9216D5D98979FB1B',
    '0xD1310BA698DFB5AC',
    '0x2FFD72DBD01ADFB7',
    '0xB8E1AFED6A267E96',
    '0xBA7C9045F12C7F99',
    '0x24A19947B3916CF7',
    '0x0801F2E2858EFC16'
)

$iterationText = $IterationsPerBatch.ToString(
    [Globalization.CultureInfo]::InvariantCulture)
Write-Host (
    'START deterministic-image-soak coverage_guided=false preset={0} batches={1} iterations_per_batch={2}' -f
        $Preset, $BatchCount, $iterationText)

for ($index = 0; $index -lt $BatchCount; ++$index) {
    $seed = $campaignSeeds[$index]
    Write-Host (
        'BATCH {0}/{1} coverage_guided=false seed={2}' -f
            ($index + 1), $BatchCount, $seed)
    & $executable --deterministic-soak $seed $iterationText
    if ($LASTEXITCODE -ne 0) {
        throw "Deterministic ImageFuzz soak failed for seed $seed (exit $LASTEXITCODE)."
    }
}

Write-Host 'PASS deterministic-image-soak coverage_guided=false'
