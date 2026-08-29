[CmdletBinding()]
param(
    [ValidateRange(1, 604800)]
    [UInt64]$MaxTotalTimeSeconds = 3600,

    [ValidateRange(1, 65536)]
    [int]$MaximumInputBytes = 65536,

    [ValidateRange(0, 2)]
    [int]$FuzzerVerbosity = 1
)

$ErrorActionPreference = 'Stop'

function Convert-HexFixture {
    param(
        [Parameter(Mandatory)]
        [string]$Path
    )

    $encoded = [IO.File]::ReadAllText($Path)
    $compact = [Text.RegularExpressions.Regex]::Replace($encoded, '\s', '')
    if (($compact.Length % 2) -ne 0 -or $compact -notmatch '^[0-9A-Fa-f]*$') {
        throw "Golden fixture is not canonical hexadecimal text: $Path"
    }
    $bytes = [byte[]]::new($compact.Length / 2)
    for ($index = 0; $index -lt $bytes.Length; ++$index) {
        $bytes[$index] = [Convert]::ToByte(
            $compact.Substring($index * 2, 2), 16)
    }
    return $bytes
}

function Get-LowerSha256 {
    param(
        [Parameter(Mandatory)]
        [byte[]]$Bytes
    )

    $sha256 = [Security.Cryptography.SHA256]::Create()
    try {
        return [BitConverter]::ToString(
            $sha256.ComputeHash($Bytes)).Replace('-', '').ToLowerInvariant()
    }
    finally {
        $sha256.Dispose()
    }
}

function Write-FixedSeed {
    param(
        [Parameter(Mandatory)]
        [string]$Path,

        [Parameter(Mandatory)]
        [AllowEmptyCollection()]
        [byte[]]$Bytes
    )

    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        $existing = [IO.File]::ReadAllBytes($Path)
        if ($existing.Length -ne $Bytes.Length -or
            (Get-LowerSha256 -Bytes $existing) -cne
                (Get-LowerSha256 -Bytes $Bytes)) {
            throw "Fixed seed already exists with different bytes: $Path"
        }
        return
    }
    [IO.File]::WriteAllBytes($Path, $Bytes)
}

$repositoryRoot = [IO.Path]::GetFullPath(
    (Join-Path $PSScriptRoot '..\..'))
. (Join-Path $repositoryRoot 'scripts\Enter-MsvcEnvironment.ps1')

$buildDirectory = Join-Path $repositoryRoot 'out\build\msvc-x64-libfuzzer'
& cmake --preset msvc-x64 -B $buildDirectory `
    -DYTEC_BUILD_IMAGE_LIBFUZZER=ON
if ($LASTEXITCODE -ne 0) {
    throw "Coverage-guided ImageFuzz configure failed (exit $LASTEXITCODE)."
}
& cmake --build $buildDirectory --target ytec-image-libfuzzer -- -j 1
if ($LASTEXITCODE -ne 0) {
    throw "Coverage-guided ImageFuzz build failed (exit $LASTEXITCODE)."
}

$fuzzRoot = Join-Path $repositoryRoot 'out\fuzz\image-v1'
$corpusDirectory = Join-Path $fuzzRoot 'corpus'
$runId = 'run-{0}-{1}' -f `
    [DateTime]::UtcNow.ToString(
        'yyyyMMdd-HHmmssfff',
        [Globalization.CultureInfo]::InvariantCulture), `
    $PID
$artifactDirectory = Join-Path (Join-Path $fuzzRoot 'artifacts') $runId
[void](New-Item -ItemType Directory -Path $corpusDirectory -Force)
[void](New-Item -ItemType Directory -Path $artifactDirectory)

$goldenDirectory = Join-Path $repositoryRoot 'tests\ImageGolden'
$manifestPath = Join-Path $goldenDirectory 'CORPUS-MANIFEST.txt'
$fixtureCount = 0
$encryptedGoldenSeedPath = $null
foreach ($line in [IO.File]::ReadAllLines($manifestPath)) {
    if ([string]::IsNullOrWhiteSpace($line) -or $line.StartsWith('#')) {
        continue
    }
    if ($line -notmatch '^(?<hash>[0-9a-f]{64})\s+(?<stem>\S+)\s+(?<length>[0-9]+)$') {
        throw "Invalid Golden corpus manifest entry: $line"
    }
    $fixturePath = Join-Path $goldenDirectory ($matches.stem + '.hex')
    $bytes = Convert-HexFixture -Path $fixturePath
    if ($bytes.Length -ne [UInt64]$matches.length -or
        (Get-LowerSha256 -Bytes $bytes) -cne $matches.hash) {
        throw "Golden seed does not match sealed hash/length: $fixturePath"
    }
    if ($bytes.Length -gt $MaximumInputBytes) {
        throw "Golden seed exceeds the configured input bound: $fixturePath"
    }
    $seedPath = Join-Path $corpusDirectory ('golden-' + $matches.stem)
    Write-FixedSeed -Path $seedPath -Bytes $bytes
    if ($matches.stem -ceq 'container-encrypted-exact-mbr-v1') {
        $encryptedGoldenSeedPath = $seedPath
    }
    ++$fixtureCount
}
if ($fixtureCount -ne 17 -or
    [string]::IsNullOrWhiteSpace($encryptedGoldenSeedPath)) {
    throw "Expected 17 sealed Golden seeds, found $fixtureCount."
}

$emptySeedPath = Join-Path $corpusDirectory 'boundary-empty'
if (Test-Path -LiteralPath $emptySeedPath -PathType Leaf) {
    if ((Get-Item -LiteralPath $emptySeedPath).Length -ne 0) {
        throw "Fixed empty seed already exists with different bytes: $emptySeedPath"
    }
}
else {
    [IO.File]::WriteAllBytes($emptySeedPath, [byte[]]::new(0))
}
Write-FixedSeed `
    -Path (Join-Path $corpusDirectory 'boundary-zero-1') `
    -Bytes ([byte[]]@(0))
Write-FixedSeed `
    -Path (Join-Path $corpusDirectory 'boundary-zero-511') `
    -Bytes ([byte[]]::new(511))
$allOnes = [byte[]]::new(512)
for ($index = 0; $index -lt $allOnes.Length; ++$index) {
    $allOnes[$index] = 0xFF
}
Write-FixedSeed `
    -Path (Join-Path $corpusDirectory 'boundary-ff-512') `
    -Bytes $allOnes
Write-FixedSeed `
    -Path (Join-Path $corpusDirectory 'boundary-zero-maximum') `
    -Bytes ([byte[]]::new($MaximumInputBytes))

$executable = Join-Path $buildDirectory 'tests\ytec-image-libfuzzer.exe'
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "Coverage-guided ImageFuzz executable not found: $executable"
}
$artifactPrefix = $artifactDirectory + [IO.Path]::DirectorySeparatorChar
$secondsText = $MaxTotalTimeSeconds.ToString(
    [Globalization.CultureInfo]::InvariantCulture)

# Exercise the exact encrypted success vector once under ASan. The main
# coverage-guided process deliberately omits the password branch so repeated
# Argon2id working sets cannot masquerade as a long-run parser leak.
try {
    $env:YTEC_IMAGE_FUZZ_ENABLE_ENCRYPTED_GOLDEN = '1'
    & $executable `
        $encryptedGoldenSeedPath `
        '-runs=1' `
        '-verbosity=0' `
        '-print_final_stats=1' `
        '-rss_limit_mb=2048'
    if ($LASTEXITCODE -ne 0) {
        throw "Encrypted Golden ASan preflight failed (exit $LASTEXITCODE)."
    }
}
finally {
    Remove-Item Env:YTEC_IMAGE_FUZZ_ENABLE_ENCRYPTED_GOLDEN `
        -ErrorAction SilentlyContinue
}

Write-Host (
    'START coverage-guided-image-fuzz coverage_guided=true golden_seeds={0} max_input_bytes={1} max_total_time_seconds={2}' -f
        $fixtureCount, $MaximumInputBytes, $secondsText)
& $executable `
    $corpusDirectory `
    "-artifact_prefix=$artifactPrefix" `
    "-max_len=$MaximumInputBytes" `
    '-timeout=10' `
    "-max_total_time=$secondsText" `
    "-verbosity=$FuzzerVerbosity" `
    '-print_final_stats=1'
if ($LASTEXITCODE -ne 0) {
    throw "Coverage-guided ImageFuzz failed (exit $LASTEXITCODE). Artifacts: $artifactDirectory"
}
Write-Host (
    'PASS coverage-guided-image-fuzz coverage_guided=true corpus={0} artifacts={1}' -f
        $corpusDirectory, $artifactDirectory)
