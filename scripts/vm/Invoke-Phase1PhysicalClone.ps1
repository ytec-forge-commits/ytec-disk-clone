[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateSet('Plan', 'Execute')]
    [string] $Mode,
    [Parameter(Mandatory)]
    [string] $HarnessPath,
    [Parameter(Mandatory)]
    [ValidateRange(1, 128)]
    [int] $SourceDiskNumber,
    [Parameter(Mandatory)]
    [ValidateRange(1, 128)]
    [int] $TargetDiskNumber,
    [switch] $BootTest,
    [string] $Confirmation,
    [Parameter(Mandatory)]
    [string] $ResultPath,
    [Parameter(Mandatory)]
    [string] $DonePath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

try {
    $arguments = @(
        if ($Mode -eq 'Plan') { '--plan' } else { '--execute' }
        '--source'
        $SourceDiskNumber
        '--target'
        $TargetDiskNumber
    )
    if ($BootTest) {
        $arguments += '--boot-test'
    }
    if ($Mode -eq 'Execute') {
        if ([string]::IsNullOrWhiteSpace($Confirmation)) {
            throw '実行モードには確認文字列が必要です。'
        }
        $arguments += @(
            '--authorization'
            $(if ($BootTest) {
                'YTEC-VM-ONLY-PHASE1-BOOT-CLONE'
            }
            else {
                'YTEC-VM-ONLY-PHASE1-DESTRUCTIVE'
            })
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
