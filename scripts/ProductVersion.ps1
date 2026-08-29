Set-StrictMode -Version Latest

function Assert-YtecRegularVersionFile {
    param(
        [Parameter(Mandatory)]
        [string]$Path,
        [Parameter(Mandatory)]
        [string]$Description,
        [long]$MaximumLength = 4096
    )

    $fullPath = [IO.Path]::GetFullPath($Path)
    if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
        throw "$Description が見つかりません: $fullPath"
    }
    $item = Get-Item -LiteralPath $fullPath -Force
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "$Description のreparse pointは使用しません: $fullPath"
    }
    if ($item.Length -le 0 -or $item.Length -gt $MaximumLength) {
        throw "$Description のサイズが許可範囲外です: $($item.Length)"
    }
    return $item
}

function Read-YtecProductVersion {
    param(
        [Parameter(Mandatory)]
        [string]$Path
    )

    $item = Assert-YtecRegularVersionFile `
        -Path $Path `
        -Description '製品版正本 version.json'
    $bytes = [IO.File]::ReadAllBytes($item.FullName)
    if ($bytes.Length -ge 3 -and
        $bytes[0] -eq 0xEF -and
        $bytes[1] -eq 0xBB -and
        $bytes[2] -eq 0xBF) {
        throw 'version.json はUTF-8 BOMなしで保存してください。'
    }
    $utf8 = [Text.UTF8Encoding]::new($false, $true)
    try {
        $text = $utf8.GetString($bytes)
    } catch {
        throw "version.json は厳格なUTF-8ではありません: $($_.Exception.Message)"
    }

    $memberMatches = [regex]::Matches(
        $text,
        '"(?<name>(?:\\.|[^"\\])*)"\s*:')
    $expectedMembers = @('numeric', 'display', 'file', 'channel')
    if ($memberMatches.Count -ne $expectedMembers.Count) {
        throw 'version.json は必須4項目だけを一度ずつ含める必要があります。'
    }
    $seenMembers = @{}
    foreach ($memberMatch in $memberMatches) {
        $memberName = [string]$memberMatch.Groups['name'].Value
        if ($expectedMembers -cnotcontains $memberName -or
            $seenMembers.ContainsKey($memberName)) {
            throw "version.json に未知または重複した項目があります: $memberName"
        }
        $seenMembers.Add($memberName, $true)
    }

    try {
        $manifest = $text | ConvertFrom-Json
    } catch {
        throw "version.json を解析できません: $($_.Exception.Message)"
    }
    if ($manifest -isnot [pscustomobject]) {
        throw 'version.json のルートはJSON objectである必要があります。'
    }
    $properties = @($manifest.PSObject.Properties)
    if ($properties.Count -ne $expectedMembers.Count) {
        throw 'version.json の項目数が不正です。'
    }
    foreach ($expectedMember in $expectedMembers) {
        $property = @($properties | Where-Object {
                $_.Name.Equals(
                    $expectedMember,
                    [StringComparison]::Ordinal)
            })
        if ($property.Count -ne 1 -or $property[0].Value -isnot [string]) {
            throw "version.json の必須文字列項目が不正です: $expectedMember"
        }
    }

    $numeric = [string]$manifest.numeric
    $display = [string]$manifest.display
    $fileVersion = [string]$manifest.file
    $channel = [string]$manifest.channel
    $numericPattern =
        '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$'
    if ($numeric -cnotmatch $numericPattern) {
        throw 'version.json numeric は先頭ゼロのない3要素の数値版である必要があります。'
    }
    if ($channel -cnotmatch '^[a-z0-9]+(?:-[a-z0-9]+)*$') {
        throw 'version.json channel は小文字英数字と単一ハイフンだけを使用してください。'
    }
    if ($display -cne "$numeric-$channel") {
        throw 'version.json display は numeric-channel と完全一致する必要があります。'
    }
    if ($fileVersion -cne "$numeric.0") {
        throw 'version.json file は numeric.0 と完全一致する必要があります。'
    }

    return [pscustomobject][ordered]@{
        numeric = $numeric
        display = $display
        file = $fileVersion
        channel = $channel
        path = $item.FullName
    }
}

function Assert-YtecExecutableVersionInfo {
    param(
        [Parameter(Mandatory)]
        [string]$Path,
        [Parameter(Mandatory)]
        [pscustomobject]$Version,
        [Parameter(Mandatory)]
        [string]$OriginalFilename,
        [Parameter(Mandatory)]
        [string]$FileDescription
    )

    $item = Assert-YtecRegularVersionFile `
        -Path $Path `
        -Description "製品実行ファイル $OriginalFilename" `
        -MaximumLength 512MB
    $info = $item.VersionInfo
    $expected = [ordered]@{
        FileVersion = $Version.file
        ProductVersion = $Version.display
        OriginalFilename = $OriginalFilename
        FileDescription = $FileDescription
        CompanyName = 'Y-TEC'
        ProductName = 'Y-TEC Tsumugi Drive'
    }
    foreach ($field in $expected.Keys) {
        if ([string]$info.$field -cne [string]$expected[$field]) {
            throw "Win32 VERSIONINFO が正本と一致しません: $OriginalFilename / $field（実際: $([string]$info.$field)）"
        }
    }

    return [pscustomobject][ordered]@{
        path = $item.FullName
        fileVersion = [string]$info.FileVersion
        productVersion = [string]$info.ProductVersion
        originalFilename = [string]$info.OriginalFilename
        fileDescription = [string]$info.FileDescription
        companyName = [string]$info.CompanyName
        productName = [string]$info.ProductName
    }
}

function Assert-YtecSbomProductVersion {
    param(
        [Parameter(Mandatory)]
        [string]$Path,
        [Parameter(Mandatory)]
        [pscustomobject]$Version,
        [Parameter(Mandatory)]
        [string]$ExpectedPackageName,
        [Parameter(Mandatory)]
        [string]$ExpectedNamespaceBase
    )

    $item = Assert-YtecRegularVersionFile `
        -Path $Path `
        -Description 'SPDX SBOM' `
        -MaximumLength 16MB
    try {
        $sbom = Get-Content -LiteralPath $item.FullName -Raw |
            ConvertFrom-Json
    } catch {
        throw "SPDX SBOMを解析できません: $($_.Exception.Message)"
    }
    $expectedDocumentName = "$ExpectedPackageName-$($Version.display)"
    $expectedNamespace =
        $ExpectedNamespaceBase.TrimEnd('/') + '/' + $Version.display
    $rootPackages = @($sbom.packages | Where-Object {
            $_.SPDXID -ceq 'SPDXRef-Package-ytec-disk-clone'
        })
    if ([string]$sbom.name -cne $expectedDocumentName -or
        [string]$sbom.documentNamespace -cne $expectedNamespace -or
        $rootPackages.Count -ne 1 -or
        [string]$rootPackages[0].name -cne $ExpectedPackageName -or
        [string]$rootPackages[0].versionInfo -cne $Version.display) {
        throw 'SBOMの製品版、document name、namespaceが version.json と一致しません。'
    }

    return [pscustomobject][ordered]@{
        path = $item.FullName
        documentName = [string]$sbom.name
        documentNamespace = [string]$sbom.documentNamespace
        packageName = [string]$rootPackages[0].name
        packageVersion = [string]$rootPackages[0].versionInfo
    }
}
