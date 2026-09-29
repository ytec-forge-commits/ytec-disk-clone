[CmdletBinding()]
param(
    [ValidateSet(
        'YWB-Win10-22H2-x64-Clean',
        'YWB-Win11-25H2-x64-Clean'
    )]
    [string[]] $VmName = @(
        'YWB-Win10-22H2-x64-Clean',
        'YWB-Win11-25H2-x64-Clean'
    )
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$PSNativeCommandUseErrorActionPreference = $false

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$workspaceRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot '..\..'))
$labRoot = Join-Path $workspaceRoot 'business-apps\ytec-windows-backup'
$credentialRoot = Join-Path $labRoot '.validation\vm-secrets'
$vboxManage = 'C:\Program Files\Oracle\VirtualBox\VBoxManage.exe'
$guestUser = 'YbcTest'
$testRoot = Join-Path $repoRoot 'out\build\msvc-x64-vm\tests'
$testNames = @(
    'ytec-unit-tests.exe',
    'ytec-mock-tests.exe',
    'ytec-phase1-synthetic-tests.exe',
    'ytec-boot-repair-tests.exe'
)

if (-not (Test-Path -LiteralPath $vboxManage -PathType Leaf)) {
    throw 'VirtualBoxが見つかりません。'
}
foreach ($testName in $testNames) {
    if (-not (Test-Path -LiteralPath (Join-Path $testRoot $testName) -PathType Leaf)) {
        throw "VM試験バイナリがありません: $testName"
    }
}

function Get-MachineValue {
    param(
        [Parameter(Mandatory)]
        [string[]] $Information,
        [Parameter(Mandatory)]
        [string] $Name
    )

    $line = $Information |
        Where-Object { $_ -like "$Name=*" } |
        Select-Object -First 1
    if ($line -match '^[^=]+="(.*)"$') {
        return $Matches[1]
    }
    return $null
}

function Wait-GuestReady {
    param(
        [Parameter(Mandatory)]
        [string] $TargetVm,
        [Parameter(Mandatory)]
        [string] $PasswordFile
    )

    $deadline = (Get-Date).AddMinutes(10)
    while ((Get-Date) -lt $deadline) {
        $null = & $vboxManage guestcontrol $TargetVm run `
            '--exe=C:\Windows\System32\cmd.exe' `
            "--username=$guestUser" `
            "--passwordfile=$PasswordFile" `
            --quiet `
            --wait-stdout `
            -- `
            /d `
            /c `
            exit `
            0 2>&1
        if ($LASTEXITCODE -eq 0 -or $LASTEXITCODE -eq -1073740940) {
            return
        }
        Start-Sleep -Seconds 10
    }
    throw "VMのGuestControl準備を確認できませんでした: $TargetVm"
}

function Invoke-GuestControlRetry {
    param(
        [Parameter(Mandatory)]
        [string] $TargetVm,
        [Parameter(Mandatory)]
        [string[]] $Arguments,
        [Parameter(Mandatory)]
        [string] $Operation
    )

    foreach ($attempt in 1..5) {
        $output = & $vboxManage guestcontrol $TargetVm @Arguments 2>&1
        if ($LASTEXITCODE -eq 0) {
            return $output
        }
        Start-Sleep -Seconds 3
    }
    throw "VirtualBox GuestControlが失敗しました: $Operation"
}

$summaries = @()
foreach ($targetVm in $VmName) {
    $runningVms = @(& $vboxManage list runningvms)
    if ($runningVms.Count -ne 0) {
        throw '別のVMが稼働中です。VMラボの直列利用を守るため試験を開始しません。'
    }

    $passwordFile = Join-Path $credentialRoot "$targetVm.password.txt"
    if (-not (Test-Path -LiteralPath $passwordFile -PathType Leaf)) {
        throw "VM資格情報ファイルがありません: $targetVm"
    }
    $vmInformation = @(& $vboxManage showvminfo $targetVm --machinereadable)
    if ($LASTEXITCODE -ne 0) {
        throw "VM情報を取得できませんでした: $targetVm"
    }
    $nic = Get-MachineValue -Information $vmInformation -Name 'nic1'
    $firmware = Get-MachineValue -Information $vmInformation -Name 'firmware'
    $secureBoot = Get-MachineValue -Information $vmInformation -Name 'SecureBoot'
    if ($nic -ne 'none' -or $firmware -ne 'EFI64') {
        throw "VMのNICまたはUEFI設定が安全な試験条件と一致しません: $targetVm"
    }

    $runStamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $guestRoot = "C:\YtecDiskCloneValidation-$runStamp"
    $evidenceRoot = Join-Path (
        $repoRoot
    ) ".validation\evidence\phase1-vm-smoke\$targetVm\$runStamp"
    New-Item -ItemType Directory -Path $evidenceRoot -Force | Out-Null
    $started = $false
    try {
        $null = & $vboxManage startvm $targetVm --type headless 2>&1
        if ($LASTEXITCODE -ne 0) {
            throw "VMを起動できませんでした: $targetVm"
        }
        $started = $true
        Wait-GuestReady -TargetVm $targetVm -PasswordFile $passwordFile
        $null = Invoke-GuestControlRetry `
            -TargetVm $targetVm `
            -Operation '合成試験ディレクトリ作成' `
            -Arguments @(
                'mkdir',
                '--parents',
                "--username=$guestUser",
                "--passwordfile=$passwordFile",
                $guestRoot
            )

        foreach ($testName in $testNames) {
            $hostPath = Join-Path $testRoot $testName
            $guestPath = "$guestRoot\$testName"
            $null = Invoke-GuestControlRetry `
                -TargetVm $targetVm `
                -Operation "copyto $testName" `
                -Arguments @(
                    'copyto',
                    "--username=$guestUser",
                    "--passwordfile=$passwordFile",
                    $hostPath,
                    $guestPath
                )
            $testOutput = & $vboxManage guestcontrol $targetVm run `
                "--exe=$guestPath" `
                "--username=$guestUser" `
                "--passwordfile=$passwordFile" `
                --wait-stdout `
                --wait-stderr `
                -- `
                $guestPath 2>&1
            $exitCode = $LASTEXITCODE
            @($testOutput) | Set-Content `
                -LiteralPath (Join-Path $evidenceRoot "$testName.txt") `
                -Encoding utf8
            Set-Content `
                -LiteralPath (Join-Path $evidenceRoot "$testName.exit.txt") `
                -Value $exitCode `
                -Encoding ascii
            if ($exitCode -ne 0 -or ($testOutput -join "`n") -match '(?m)^FAIL ') {
                throw "VM内の合成試験が失敗しました: $targetVm / $testName"
            }
        }

        [PSCustomObject]@{
            VmName = $targetVm
            Firmware = $firmware
            SecureBoot = $secureBoot
            Nic = $nic
            Tests = $testNames
            Result = 'PASS'
            CompletedUtc = [DateTimeOffset]::UtcNow
        } | ConvertTo-Json -Depth 4 | Set-Content `
            -LiteralPath (Join-Path $evidenceRoot 'summary.json') `
            -Encoding utf8
        $summaries += [PSCustomObject]@{
            VmName = $targetVm
            SecureBoot = $secureBoot
            Result = 'PASS'
            EvidenceDirectory = $evidenceRoot
        }
    }
    finally {
        if ($started) {
            $null = & $vboxManage guestcontrol $targetVm rm `
                '--recursive' `
                "--username=$guestUser" `
                "--passwordfile=$passwordFile" `
                $guestRoot 2>&1
            $null = & $vboxManage guestcontrol $targetVm closesession --all 2>&1
            $null = & $vboxManage controlvm $targetVm savestate 2>&1
            if ($LASTEXITCODE -ne 0) {
                Write-Warning "VMを保存状態へ戻せませんでした: $targetVm"
            }
        }
    }
}

$summaries | Format-Table -AutoSize
