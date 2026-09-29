$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'ProductVersion.ps1')
$versionPath = Join-Path $repoRoot 'version.json'
$productVersion = Read-YtecProductVersion -Path $versionPath
$manifestPath = Join-Path $repoRoot 'third_party\dependencies.json'
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$projectManifestPath = Join-Path $repoRoot 'sbom\project.json'
$project = Get-Content -LiteralPath $projectManifestPath -Raw | ConvertFrom-Json

$allowedLicenses = @(
    'MIT'
    'BSD-2-Clause'
    'BSD-3-Clause'
    'Apache-2.0'
    'OFL-1.1'
)
$forbiddenPattern = '(?i)(^|[^A-Za-z])(AGPL|GPL|LGPL|SSPL|MPL|EPL|Commons Clause|BSL|Business Source License|NOASSERTION|UNKNOWN)([^A-Za-z]|$)'
$failures = @()

$projectLicensePath = Join-Path $repoRoot 'LICENSE'
$projectNoticePath = Join-Path $repoRoot 'NOTICE'
$sbomPath = Join-Path $repoRoot 'SBOM.spdx.json'
$expectedApacheLicenseSha256 =
    'C71D239DF91726FC519C6EB72D318EC65820627232B2F796219E87DCF35D0AB4'

foreach ($requiredPath in @(
        $projectLicensePath,
        $projectNoticePath,
        $versionPath,
        $projectManifestPath,
        $sbomPath)) {
    if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
        $failures += "製品ライセンス監査の必須ファイルがありません: $requiredPath"
        continue
    }
    $requiredItem = Get-Item -LiteralPath $requiredPath -Force
    if (($requiredItem.Attributes -band
            [IO.FileAttributes]::ReparsePoint) -ne 0) {
        $failures += "製品ライセンス監査でreparse pointを拒否しました: $requiredPath"
    }
}

if (Test-Path -LiteralPath $projectLicensePath -PathType Leaf) {
    $actualLicenseSha256 =
        (Get-FileHash -LiteralPath $projectLicensePath -Algorithm SHA256).Hash
    if ($actualLicenseSha256 -cne $expectedApacheLicenseSha256) {
        $failures += 'ルートLICENSEが監査済みApache License 2.0本文と一致しません。'
    }
}

if ($project.license -cne 'Apache-2.0' -or
    $project.downloadLocation -cne
        'https://github.com/ytec-forge-commits/ytec-disk-clone' -or
    $project.copyright -cne 'Copyright 2026 Y-TEC' -or
    $project.documentNamespaceBase -cne
        'https://github.com/ytec-forge-commits/ytec-disk-clone/sbom') {
    $failures += '製品SBOM台帳のライセンス、公開元、著作権が不正です。'
}

if (Test-Path -LiteralPath $projectNoticePath -PathType Leaf) {
    $noticeText = Get-Content -LiteralPath $projectNoticePath -Raw
    if ($noticeText -notmatch 'Y-TEC Tsumugi Drive' -or
        $noticeText -notmatch 'Copyright 2026 Y-TEC' -or
        $noticeText -notmatch 'THIRD-PARTY-NOTICES\.txt') {
        $failures += 'NOTICEに製品帰属または第三者通知への参照がありません。'
    }
}

if (Test-Path -LiteralPath $sbomPath -PathType Leaf) {
    $sbom = Get-Content -LiteralPath $sbomPath -Raw | ConvertFrom-Json
    try {
        Assert-YtecSbomProductVersion `
            -Path $sbomPath `
            -Version $productVersion `
            -ExpectedPackageName 'ytec-disk-clone' `
            -ExpectedNamespaceBase `
                'https://github.com/ytec-forge-commits/ytec-disk-clone/sbom' |
            Out-Null
    } catch {
        $failures += $_.Exception.Message
    }
    $rootPackages = @($sbom.packages | Where-Object {
            $_.SPDXID -ceq 'SPDXRef-Package-ytec-disk-clone'
        })
    if ($rootPackages.Count -ne 1 -or
        $rootPackages[0].licenseDeclared -cne 'Apache-2.0' -or
        $rootPackages[0].licenseConcluded -cne 'Apache-2.0' -or
        $rootPackages[0].downloadLocation -cne
            'https://github.com/ytec-forge-commits/ytec-disk-clone' -or
        $rootPackages[0].copyrightText -cne 'Copyright 2026 Y-TEC') {
        $failures += 'SBOMの製品packageがApache-2.0公開台帳と一致しません。'
    }
}

foreach ($dependency in $manifest.dependencies) {
    if ([string]::IsNullOrWhiteSpace($dependency.name) -or
        [string]::IsNullOrWhiteSpace($dependency.version) -or
        [string]::IsNullOrWhiteSpace($dependency.source) -or
        [string]::IsNullOrWhiteSpace($dependency.license)) {
        $failures += '依存台帳に必須項目がないエントリがあります。'
        continue
    }

    if ($dependency.license -match $forbiddenPattern -or
        $allowedLicenses -notcontains $dependency.license) {
        $failures += "未承認または禁止ライセンス: $($dependency.name) $($dependency.license)"
    }

    if ($dependency.approved -ne $true) {
        $failures += "人間の承認記録がありません: $($dependency.name)"
    }

    if ($dependency.license -eq 'OFL-1.1' -and
        $dependency.primaryPackagePurpose -ne 'FONT') {
        $failures += "OFL依存をFONTとして記録していません: $($dependency.name)"
    }

    $licensePath = Join-Path $repoRoot $dependency.licenseFile
    if ([string]::IsNullOrWhiteSpace($dependency.licenseFile) -or
        -not (Test-Path -LiteralPath $licensePath)) {
        $failures += "ライセンス本文がありません: $($dependency.name)"
    }
}

if ($failures.Count -gt 0) {
    $failures | ForEach-Object { Write-Error $_ }
    throw 'License check: FAIL'
}

Write-Output "License check: PASS ($($manifest.dependencies.Count) external dependencies)"
