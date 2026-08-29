$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$helperPath = Join-Path $PSScriptRoot 'ProductVersion.ps1'
. $helperPath

$version = Read-YtecProductVersion -Path (Join-Path $repoRoot 'version.json')
$sbomReport = Assert-YtecSbomProductVersion `
    -Path (Join-Path $repoRoot 'SBOM.spdx.json') `
    -Version $version `
    -ExpectedPackageName 'ytec-disk-clone' `
    -ExpectedNamespaceBase `
        'https://github.com/ytec-commits/ytec-disk-clone/sbom'

function Write-ExactTemporaryVersionManifest {
    param(
        [Parameter(Mandatory)]
        [string]$Text
    )

    $path = Join-Path $env:TEMP `
        ('Y-TEC-Tsumugi-Drive-version-test-' +
            [guid]::NewGuid().ToString('N') + '.json')
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes($Text)
    $stream = [IO.File]::Open(
        $path,
        [IO.FileMode]::CreateNew,
        [IO.FileAccess]::Write,
        [IO.FileShare]::None)
    try {
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush($true)
    } finally {
        $stream.Dispose()
    }
    return $path
}

function Remove-ExactTemporaryVersionManifest {
    param(
        [Parameter(Mandatory)]
        [string]$Path
    )

    $temporaryRoot = [IO.Path]::GetFullPath($env:TEMP).TrimEnd('\')
    $candidate = [IO.Path]::GetFullPath($Path)
    $parent = [IO.Path]::GetDirectoryName($candidate).TrimEnd('\')
    $leaf = [IO.Path]::GetFileName($candidate)
    if (-not $parent.Equals(
            $temporaryRoot,
            [StringComparison]::OrdinalIgnoreCase) -or
        $leaf -notmatch
            '^Y-TEC-Tsumugi-Drive-version-test-[0-9a-f]{32}\.json$') {
        throw "一時version manifestの削除対象が固定境界外です: $candidate"
    }
    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
        $item = Get-Item -LiteralPath $candidate -Force
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "一時version manifestのreparse pointは削除しません: $candidate"
        }
        Remove-Item -LiteralPath $candidate -Force
    }
}

function Assert-VersionManifestRejected {
    param(
        [Parameter(Mandatory)]
        [string]$Text
    )

    $path = Write-ExactTemporaryVersionManifest -Text $Text
    try {
        try {
            Read-YtecProductVersion -Path $path | Out-Null
            throw '不正なversion manifestが許可されました。'
        } catch {
            if ($_.Exception.Message -ceq
                '不正なversion manifestが許可されました。') {
                throw
            }
        }
    } finally {
        Remove-ExactTemporaryVersionManifest -Path $path
    }
}

Assert-VersionManifestRejected -Text `
    '{"numeric":"2.3.4","display":"2.3.4-preview","file":"2.3.4.0"}'
Assert-VersionManifestRejected -Text `
    '{"numeric":"2.3.4","display":"2.3.4-preview","file":"2.3.4.0","channel":"preview","extra":"x"}'
Assert-VersionManifestRejected -Text `
    '{"numeric":2.3,"display":"2.3.4-preview","file":"2.3.4.0","channel":"preview"}'
Assert-VersionManifestRejected -Text `
    '{"numeric":"2.3.4","numeric":"9.9.9","display":"2.3.4-preview","file":"2.3.4.0","channel":"preview"}'
Assert-VersionManifestRejected -Text `
    '{"numeric":"2.3.4","display":"2.3.4-beta","file":"2.3.4.0","channel":"preview"}'
Assert-VersionManifestRejected -Text '{not-json}'

$report = [ordered]@{
    schemaVersion = 1
    numeric = $version.numeric
    display = $version.display
    file = $version.file
    channel = $version.channel
    sbom = $sbomReport
    rejectedInvalidManifestCount = 6
}
Write-Output ('Product version tests: PASS ' +
    ($report | ConvertTo-Json -Depth 4 -Compress))
