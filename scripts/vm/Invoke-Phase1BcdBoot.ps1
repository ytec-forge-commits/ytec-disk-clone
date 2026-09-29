[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateSet('Plan', 'Execute')]
    [string] $Mode,
    [Parameter(Mandatory)]
    [string] $HarnessPath,
    [Parameter(Mandatory)]
    [ValidateRange(1, 128)]
    [int] $TargetDiskNumber,
    [Parameter(Mandatory)]
    [string] $WindowsRoot,
    [Parameter(Mandatory)]
    [string] $EspRoot,
    [string] $Confirmation,
    [Parameter(Mandatory)]
    [string] $ResultPath,
    [Parameter(Mandatory)]
    [string] $DonePath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

try {
    if ($WindowsRoot -notmatch '^[A-Za-z]:\\$' -or
        $EspRoot -notmatch '^[A-Za-z]:\\$' -or
        $WindowsRoot[0] -eq $EspRoot[0]) {
        throw 'WindowsとESPには異なるドライブ文字ルートが必要です。'
    }
    $arguments = @(
        if ($Mode -eq 'Plan') { '--plan' } else { '--execute' }
        '--target'
        $TargetDiskNumber
        '--windows-root'
        $WindowsRoot
        '--esp-root'
        $EspRoot
    )
    if ($Mode -eq 'Execute') {
        if ([string]::IsNullOrWhiteSpace($Confirmation)) {
            throw '実行モードには確認文字列が必要です。'
        }
        $arguments += @(
            '--authorization'
            'YTEC-VM-ONLY-PHASE1-BCDBOOT'
            '--confirmation'
            $Confirmation
        )
    }

    $previousErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = @(& $HarnessPath @arguments 2>&1)
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }
    [IO.File]::WriteAllLines(
        $ResultPath,
        [string[]]@($output | ForEach-Object { $_.ToString() }),
        [Text.UTF8Encoding]::new($false))
    if ($exitCode -ne 0) {
        [IO.File]::WriteAllText($DonePath, 'FAIL', [Text.UTF8Encoding]::new($false))
        exit $exitCode
    }
    [IO.File]::WriteAllText($DonePath, 'PASS', [Text.UTF8Encoding]::new($false))
    exit 0
}
catch {
    [IO.File]::WriteAllText(
        $ResultPath,
        $_.Exception.Message,
        [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText($DonePath, 'FAIL', [Text.UTF8Encoding]::new($false))
    Write-Error $_
    exit 1
}
