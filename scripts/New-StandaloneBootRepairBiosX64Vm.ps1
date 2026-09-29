[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $IsoPath,

    [Parameter(Mandatory)]
    [ValidatePattern('^[0-9A-Fa-f]{64}$')]
    [string] $ExpectedSha256,

    [string] $VmRoot = (
        Join-Path (
            [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
        ) '.validation\vms'
    ),

    [string] $CredentialRoot = (
        Join-Path (
            [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
        ) '.validation\vm-secrets'
    ),

    [string] $EvidenceRoot = (
        Join-Path (
            [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
        ) '.validation\evidence\standalone-bios-x64-install'
    )
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$PSNativeCommandUseErrorActionPreference = $false

$vmName = 'YDC-Standalone-BootRepair-BIOS-x64'
$vboxManage = 'C:\Program Files\Oracle\VirtualBox\VBoxManage.exe'
$guestAdditionsIso = 'C:\Program Files\Oracle\VirtualBox\VBoxGuestAdditions.iso'
$isoExpectedBytes = 6023215104
$isoExpectedImage = 'Windows 10 Pro ('
$imageIndex = 3
$diskMb = 57344

function Invoke-VBox {
    param(
        [Parameter(Mandatory)]
        [string[]] $Arguments,

        [switch] $SuppressOutput
    )

    if ($SuppressOutput) {
        $null = & $vboxManage @Arguments 2>&1
    }
    else {
        & $vboxManage @Arguments
    }
    if ($LASTEXITCODE -ne 0) {
        throw (
            'VirtualBoxコマンドが失敗しました。' +
            " command=$($Arguments[0]) exit=$LASTEXITCODE"
        )
    }
}

if (
    -not (Test-Path -LiteralPath $vboxManage -PathType Leaf) -or
    -not (Test-Path -LiteralPath $guestAdditionsIso -PathType Leaf)
) {
    throw 'VirtualBoxとGuest Additions ISOが必要です。'
}

$runningVms = @(& $vboxManage list runningvms)
if ($LASTEXITCODE -ne 0) {
    throw '稼働中VMを確認できません。'
}
if ($runningVms.Count -ne 0) {
    throw '別のVMが稼働中です。VMラボは直列で使用してください。'
}

$isoPathValue = [IO.Path]::GetFullPath($IsoPath)
if (-not (Test-Path -LiteralPath $isoPathValue -PathType Leaf)) {
    throw "Windows ISOがありません: $isoPathValue"
}
$isoItem = Get-Item -LiteralPath $isoPathValue
if ($isoItem.Length -ne $isoExpectedBytes) {
    throw 'Windows ISOの固定サイズが一致しません。'
}
$actualIsoHash = (
    Get-FileHash -LiteralPath $isoPathValue -Algorithm SHA256
).Hash
if ($actualIsoHash -ine $ExpectedSha256) {
    throw 'Windows ISOのSHA-256が承認済み棚卸し値と一致しません。'
}

$detection = & $vboxManage unattended detect (
    "--iso=$isoPathValue"
) --machine-readable
if ($LASTEXITCODE -ne 0) {
    throw 'VirtualBoxがWindows ISOを認識できません。'
}
$imageLine = $detection |
    Where-Object { $_ -like "ImageIndex$imageIndex=*" } |
    Select-Object -First 1
if (
    $detection -notcontains 'IsInstallSupported="on"' -or
    $imageLine -notmatch '^ImageIndex\d+="(.+)"$' -or
    -not $Matches[1].StartsWith(
        $isoExpectedImage,
        [StringComparison]::Ordinal
    )
) {
    throw 'Windows 10 Pro x64の無人インストール情報を確認できません。'
}
$imageName = $Matches[1]

$registeredNames = @(
    & $vboxManage list vms |
        ForEach-Object {
            if ($_ -match '^"(.+)" \{[0-9a-f-]+\}$') {
                $Matches[1]
            }
        }
)
if ($registeredNames -contains $vmName) {
    throw "同名VMが既に登録されています: $vmName"
}

$vmRootPath = [IO.Path]::GetFullPath($VmRoot)
$credentialRootPath = [IO.Path]::GetFullPath($CredentialRoot)
$evidenceRootPath = [IO.Path]::GetFullPath($EvidenceRoot)
if (
    [IO.Path]::GetFileName($vmRootPath) -cne 'vms' -or
    [IO.Path]::GetFileName($credentialRootPath) -cne 'vm-secrets' -or
    [IO.Path]::GetFileName($evidenceRootPath) -cne
        'standalone-bios-x64-install'
) {
    throw 'VM、資格情報、または証跡フォルダーの安全な範囲を確認できません。'
}
$vmDirectory = Join-Path $vmRootPath $vmName
if (Test-Path -LiteralPath $vmDirectory) {
    throw "同名VMフォルダーが既にあります: $vmDirectory"
}

New-Item -ItemType Directory -Path $vmRootPath -Force | Out-Null
New-Item -ItemType Directory -Path $credentialRootPath -Force | Out-Null
$currentSid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
& icacls.exe $credentialRootPath /inheritance:r /grant:r (
    "*$($currentSid):(OI)(CI)F"
) '*S-1-5-18:(OI)(CI)F' | Out-Null
if ($LASTEXITCODE -ne 0) {
    throw 'VM資格情報フォルダーのACL設定に失敗しました。'
}

$randomBytes = [Security.Cryptography.RandomNumberGenerator]::GetBytes(24)
try {
    $randomText = [Convert]::ToBase64String($randomBytes)
    $randomText = $randomText.Replace('/', '7').Replace('+', '9').TrimEnd('=')
    $vmPassword = "Ydc!$randomText"
    $passwordFile = Join-Path $credentialRootPath "$vmName.password.txt"
    [IO.File]::WriteAllText(
        $passwordFile,
        $vmPassword,
        [Text.UTF8Encoding]::new($false)
    )
}
finally {
    [Array]::Clear($randomBytes, 0, $randomBytes.Length)
}
& icacls.exe $passwordFile /inheritance:r /grant:r (
    "*$($currentSid):F"
) '*S-1-5-18:F' | Out-Null
if ($LASTEXITCODE -ne 0) {
    throw 'VM資格情報ファイルのACL設定に失敗しました。'
}

Invoke-VBox @(
    'createvm',
    '--name', $vmName,
    '--ostype', 'Windows10_64',
    '--basefolder', $vmRootPath,
    '--register'
)
Invoke-VBox @(
    'modifyvm',
    $vmName,
    '--memory', '6144',
    '--cpus', '4',
    '--cpu-execution-cap', '100',
    '--firmware', 'bios',
    '--chipset', 'ich9',
    '--ioapic', 'on',
    '--x86-hpet', 'on',
    '--paravirt-provider', 'default',
    '--nested-paging', 'on',
    '--graphicscontroller', 'vboxsvga',
    '--vram', '128',
    '--accelerate-3d', 'off',
    '--mouse', 'usbtablet',
    '--keyboard', 'usb',
    '--nic1', 'none',
    '--clipboard-mode', 'disabled',
    '--drag-and-drop', 'disabled',
    '--audio-enabled', 'off',
    '--boot1', 'disk',
    '--boot2', 'dvd',
    '--boot3', 'none',
    '--boot4', 'none',
    '--rtc-use-utc', 'off'
)

$diskPath = Join-Path $vmDirectory "$vmName-56GiB.vdi"
Invoke-VBox @(
    'createmedium',
    'disk',
    '--filename', $diskPath,
    '--size', $diskMb,
    '--format', 'VDI',
    '--variant', 'Standard'
)
Invoke-VBox @(
    'storagectl',
    $vmName,
    '--name', 'SATA',
    '--add', 'sata',
    '--controller', 'IntelAhci',
    '--portcount', '4',
    '--hostiocache', 'on',
    '--bootable', 'on'
)
Invoke-VBox @(
    'storageattach',
    $vmName,
    '--storagectl', 'SATA',
    '--port', '0',
    '--device', '0',
    '--type', 'hdd',
    '--medium', $diskPath,
    '--nonrotational', 'on',
    '--discard', 'off'
)

$unattendedArguments = @(
    'unattended',
    'install',
    $vmName,
    "--iso=$isoPathValue",
    '--user=YdcTest',
    "--user-password-file=$passwordFile",
    "--admin-password-file=$passwordFile",
    '--full-user-name=Y-TEC 単独起動修復VM検証',
    '--locale=ja_JP',
    '--country=JP',
    '--language=ja-JP',
    '--hostname=ydc-bios-repair.test',
    "--image-index=$imageIndex",
    '--install-additions',
    "--additions-iso=$guestAdditionsIso",
    "--auxiliary-base-path=$(Join-Path $vmDirectory 'Unattended')",
    '--start-vm=none'
)
Invoke-VBox -SuppressOutput -Arguments $unattendedArguments
Invoke-VBox @('startvm', $vmName, '--type', 'headless')

foreach ($attempt in 1..5) {
    Start-Sleep -Milliseconds 1500
    $null = & $vboxManage controlvm $vmName keyboardputscancode 1c 9c 2>&1
}

$evidenceDirectory = Join-Path (
    $evidenceRootPath
) (Get-Date -Format 'yyyyMMdd-HHmmss')
New-Item -ItemType Directory -Path $evidenceDirectory -Force | Out-Null
$resultPath = Join-Path $evidenceDirectory 'setup.json'
$result = [ordered]@{
    schemaVersion = 1
    vmName = $vmName
    purpose = 'Standalone boot repair Windows 10 x64 Legacy BIOS VM'
    firmware = 'Legacy BIOS'
    network = 'none'
    diskBytes = 56GB
    isoPath = $isoPathValue
    isoLength = $isoItem.Length
    isoSha256 = $actualIsoHash
    imageIndex = $imageIndex
    imageName = $imageName
    productKeySupplied = $false
    activationBypassUsed = $false
    credentialStoredOutsideVm = $true
    credentialValueLogged = $false
    installationStarted = $true
    started = (Get-Date).ToString('o')
} | ConvertTo-Json -Depth 4
[IO.File]::WriteAllText(
    $resultPath,
    $result,
    [Text.UTF8Encoding]::new($false))

[PSCustomObject]@{
    VmName = $vmName
    Firmware = 'Legacy BIOS'
    Network = 'none'
    DiskPath = $diskPath
    DiskBytes = 56GB
    Evidence = $resultPath
    InstallationStarted = $true
} | Format-List
