$ErrorActionPreference = 'Stop'
$env:VSLANG = '1033'

function Set-YtecProcessPath {
    param(
        [Parameter(Mandatory)]
        [string]$Value
    )

    if ([string]::IsNullOrWhiteSpace($Value)) {
        throw 'PATH を空にすることはできません。'
    }

    # Windows APIs normally treat environment-variable names as
    # case-insensitive, but a launcher can still provide duplicate Path/PATH
    # entries in the raw process environment block. CTest/libuv can then use
    # different entries for consecutive child processes. Remove every raw
    # variant before installing the one canonical MSVC PATH value.
    for ($attempt = 0; $attempt -lt 8; ++$attempt) {
        $pathVariableNames = @(
            [Environment]::GetEnvironmentVariables(
                [EnvironmentVariableTarget]::Process).Keys |
                ForEach-Object { [string]$_ } |
                Where-Object { $_ -ieq 'PATH' })
        if ($pathVariableNames.Count -eq 0) {
            break
        }
        foreach ($pathVariableName in $pathVariableNames) {
            # PowerShell binds a literal $null to String.Empty for this .NET
            # overload, which leaves an empty variable in the process block.
            # NullString passes an actual null so the exact raw key is deleted.
            [Environment]::SetEnvironmentVariable(
                $pathVariableName,
                [NullString]::Value,
                [EnvironmentVariableTarget]::Process)
        }
    }
    $remainingPathVariableNames = @(
        [Environment]::GetEnvironmentVariables(
            [EnvironmentVariableTarget]::Process).Keys |
            ForEach-Object { [string]$_ } |
            Where-Object { $_ -ieq 'PATH' })
    if ($remainingPathVariableNames.Count -ne 0) {
        throw 'プロセス環境の重複 Path/PATH を正規化できませんでした。'
    }

    [Environment]::SetEnvironmentVariable(
        'PATH',
        $Value,
        [EnvironmentVariableTarget]::Process)

    $installedPathVariableNames = @(
        [Environment]::GetEnvironmentVariables(
            [EnvironmentVariableTarget]::Process).Keys |
            ForEach-Object { [string]$_ } |
            Where-Object { $_ -ieq 'PATH' })
    if ($installedPathVariableNames.Count -ne 1 -or
        $installedPathVariableNames[0] -cne 'PATH' -or
        [Environment]::GetEnvironmentVariable(
            'PATH',
            [EnvironmentVariableTarget]::Process) -cne $Value) {
        throw '正規化した MSVC PATH を確認できませんでした。'
    }
}

if ((Get-Command cl.exe -ErrorAction SilentlyContinue) -and
    $env:VSCMD_ARG_TGT_ARCH -eq 'x64') {
    Set-YtecProcessPath -Value $env:PATH
    return
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'vswhere.exe が見つかりません。MSVC x64 Build Tools をインストールしてください。'
}

$installationPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($installationPath)) {
    throw 'MSVC x64 Build Tools が見つかりません。'
}

$vsDevCmd = Join-Path $installationPath.Trim() 'Common7\Tools\VsDevCmd.bat'
if (-not (Test-Path -LiteralPath $vsDevCmd)) {
    throw "VsDevCmd.bat が見つかりません: $vsDevCmd"
}

$initializeCommand = '"{0}" -no_logo -arch=x64 -host_arch=x64 >nul && set' -f $vsDevCmd
$environmentLines = & $env:ComSpec /d /s /c $initializeCommand
if ($LASTEXITCODE -ne 0) {
    throw 'MSVC x64 開発環境の初期化に失敗しました。'
}

$environmentVariables = [Collections.Generic.Dictionary[string, string]]::new(
    [StringComparer]::OrdinalIgnoreCase)
foreach ($line in $environmentLines) {
    if ($line -match '^([^=]+)=(.*)$') {
        $name = $matches[1]
        $value = $matches[2]
        # A managed launcher can pass both Path and PATH in one environment
        # block. VsDevCmd updates PATH, while the stale Path entry would be
        # enumerated later and undo the compiler setup in PowerShell's
        # case-insensitive Env: drive. Prefer VsDevCmd's canonical PATH entry.
        if (-not $environmentVariables.ContainsKey($name) -or
            $name -ceq 'PATH') {
            $environmentVariables[$name] = $value
        }
    }
}
foreach ($entry in $environmentVariables.GetEnumerator()) {
    if ($entry.Key -ine 'PATH') {
        Set-Item -Path "Env:$($entry.Key)" -Value $entry.Value
    }
}
Set-YtecProcessPath -Value $environmentVariables['PATH']

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue) -or
    $env:VSCMD_ARG_TGT_ARCH -ne 'x64') {
    throw '初期化後の MSVC x64 コンパイラーを確認できませんでした。'
}
