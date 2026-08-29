$ErrorActionPreference = 'Stop'
$PSDefaultParameterValues['Get-Content:Encoding'] = 'UTF8'
$repoRoot = Split-Path -Parent $PSScriptRoot
$sourceRoot = Join-Path $repoRoot 'src'

$forbiddenSourcePatterns = [ordered]@{
    '\bWriteFile\s*\('             = 'WriteFile'
    '\bGENERIC_WRITE\b'             = 'GENERIC_WRITE'
    '\bCREATE_ALWAYS\b'             = 'CREATE_ALWAYS'
    '\bTRUNCATE_EXISTING\b'         = 'TRUNCATE_EXISTING'
    '\bMoveFileExW\s*\('            = 'MoveFileExW'
    '\bReplaceFileW\s*\('           = 'ReplaceFileW'
    '\bDeleteFileW\s*\('            = 'DeleteFileW'
    '\bSetEndOfFile\s*\('           = 'SetEndOfFile'
    '\bSetFileInformationByHandle\s*\(' = 'SetFileInformationByHandle'
    '\bDELETE\b'                      = 'DELETE access'
    '\bIOCTL_DISK_SET_[A-Z0-9_]+\b' = 'IOCTL_DISK_SET_*'
    '\bIOCTL_DISK_CREATE_DISK\b'    = 'IOCTL_DISK_CREATE_DISK'
    '\bIOCTL_DISK_DELETE_DRIVE_LAYOUT\b' = 'IOCTL_DISK_DELETE_DRIVE_LAYOUT'
    '\bFSCTL_LOCK_VOLUME\b'         = 'FSCTL_LOCK_VOLUME'
    '\bFSCTL_DISMOUNT_VOLUME\b'     = 'FSCTL_DISMOUNT_VOLUME'
    '\bFSCTL_EXTEND_VOLUME\b'       = 'FSCTL_EXTEND_VOLUME'
    '\bIOCTL_VOLUME_OFFLINE\b'      = 'IOCTL_VOLUME_OFFLINE'
    '\bIOCTL_VOLUME_ONLINE\b'       = 'IOCTL_VOLUME_ONLINE'
}

$sourceFiles = Get-ChildItem -LiteralPath $sourceRoot -Recurse -File |
    Where-Object { $_.Extension -in @('.h', '.hpp', '.c', '.cpp', '.cxx') }
$failures = @()
$adkNetworkSurfacePath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'WindowsApp\src\windows_adk_acquisition_platform.cpp'))
$manualUpdateNetworkSurfacePath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'WindowsApp\src\manual_update.cpp'))
$networkApiPatterns = @(
    '\bWinHttpOpen\s*\('
    '\bWinHttpConnect\s*\('
    '\bWinHttpOpenRequest\s*\('
    '\bWinHttpSendRequest\s*\('
    '\bWinHttpReceiveResponse\s*\('
    '\bWinHttpReadData\s*\('
)
foreach ($networkPattern in $networkApiPatterns) {
    $networkMatches = $sourceFiles |
        Select-String -Pattern $networkPattern -CaseSensitive
    foreach ($networkMatch in $networkMatches) {
        $networkPath = [IO.Path]::GetFullPath($networkMatch.Path)
        if (-not $networkPath.Equals(
                $adkNetworkSurfacePath,
                [StringComparison]::OrdinalIgnoreCase) -and
            -not $networkPath.Equals(
                $manualUpdateNetworkSurfacePath,
                [StringComparison]::OrdinalIgnoreCase)) {
            $relative = $networkMatch.Path.Substring($repoRoot.Length).TrimStart('\')
            $failures += "$relative`:$($networkMatch.LineNumber) contains a network API outside the two approved adapters"
        }
    }
}
$auditedPhysicalWriterPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'DiskModel\src\physical_disk.cpp'))
$auditedPhysicalWriterPatterns = @(
    '\bWriteFile\s*\('
    '\bGENERIC_WRITE\b'
    '\bIOCTL_DISK_SET_[A-Z0-9_]+\b'
)
$auditedImageFileWriterPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'ImageFormat\src\windows_file_staging.cpp'))
$auditedImageFileWriterPatterns = @(
    '\bWriteFile\s*\('
    '\bGENERIC_WRITE\b'
    '\bDeleteFileW\s*\('
    '\bSetEndOfFile\s*\('
    '\bSetFileInformationByHandle\s*\('
    '\bDELETE\b'
)
$auditedLogFileWriterPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'CloneCore\src\log.cpp'))
$auditedLogFileWriterPatterns = @(
    '\bWriteFile\s*\('
    '\bGENERIC_WRITE\b'
)
$auditedWinPeDiskPartScriptWriterPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'WinPEApp\src\mbr2gpt_direct_execution_service.cpp'))
$auditedWinPeDiskPartScriptWriterPatterns = @(
    '\bWriteFile\s*\('
    '\bGENERIC_WRITE\b'
    '\bDeleteFileW\s*\('
)
$auditedBcdStoreTransactionPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'BootRepair\src\bcdboot.cpp'))
$auditedBcdStoreTransactionPatterns = @(
    '\bSetFileInformationByHandle\s*\('
    '\bDELETE\b'
)
$auditedBcdStoreTransactionText = Get-Content `
    -LiteralPath $auditedBcdStoreTransactionPath `
    -Raw
$auditedBcdStoreTransactionRequiredPatterns = [ordered]@{
    '\bFILE_ID_INFO\b' = 'opened-handle file identity'
    '\bFileIdInfo\b' = 'volume and file ID query'
    '\bFileBasicInfo\b' = 'write and change timestamp query'
    'standard\.NumberOfLinks\s*!=\s*1U' = 'single hard-link gate'
    'FILE_FLAG_OPEN_REPARSE_POINT' = 'reparse-safe open'
    'FILE_READ_ATTRIBUTES\s*\|\s*DELETE\s*\|\s*SYNCHRONIZE' = 'handle-owned cleanup access'
    '\bFileDispositionInfo\b' = 'opened-handle delete disposition'
    '\bverify_bcd_store_read_only\s*\(' = 'BCD registry hive readback verification'
    '\bexecute_multi_windows_bcdboot_with_store_transaction\s*\(' = 'one rollback boundary for multi-Windows registration'
    'BcdBootStorePolicy::rebuild_fresh' = 'fresh first registration policy'
    'BcdBootStorePolicy::preserve_existing' = 'later registration append policy'
    '\brename_bcd_handle_no_replace\s*\(' = 'source-handle and destination-parent-handle rename'
    '\bFileRenameInfo\b' = 'handle-bound BCD rename'
    'ReplaceIfExists\s*=\s*FALSE' = 'no-replace BCD rename'
    '\bsource_after_failure\b' = 'failed-rename source identity readback'
    '\bdestination_after_failure\b' = 'failed-rename destination absence readback'
}
foreach ($pattern in $auditedBcdStoreTransactionRequiredPatterns.Keys) {
    if ($auditedBcdStoreTransactionText -notmatch $pattern) {
        $failures += "BCD store transaction must retain $($auditedBcdStoreTransactionRequiredPatterns[$pattern])"
    }
}
$auditedBcdStoreTransactionForbiddenPatterns = [ordered]@{
    '\bMoveFile(?:Ex)?W\s*\(' = 'path-only BCD rename'
    '\bDeleteFileW\s*\(' = 'path-only BCD cleanup'
    '\bReplaceFileW\s*\(' = 'path-only replacement'
    '\bCREATE_ALWAYS\b' = 'overwriting BCD creation'
    '\bTRUNCATE_EXISTING\b' = 'BCD truncation'
}
foreach ($pattern in $auditedBcdStoreTransactionForbiddenPatterns.Keys) {
    if ($auditedBcdStoreTransactionText -match $pattern) {
        $failures += "BCD store transaction contains forbidden $($auditedBcdStoreTransactionForbiddenPatterns[$pattern])"
    }
}

# BCD-001/002/004(preserve)/006: the WinPE product may execute only
# after an explicit Windows policy, immutable pure review, full revalidation,
# and ordered multi-Windows transaction. Third-party EFI deletion and
# unsafe/unknown WinRE remain separately fail-closed. Current-PC NVRAM uses a
# separate explicit reviewed transaction after BCDBoot. A verified Windows
# fallback image must be opened, hashed, bound, freshly locked and re-diagnosed.
$winPeAutomaticBootRepairUiPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'WinPEApp\src\automatic_boot_repair_ui.cpp'))
$winPeAutomaticBootRepairGuiPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'WinPEApp\src\gui_main.cpp'))
$bootRepairStandalonePath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'BootRepair\src\standalone_repair.cpp'))
$winPeAutomaticBootRepairTestPath = [IO.Path]::GetFullPath(
    (Join-Path $repoRoot 'tests\Unit\winpe_automatic_boot_repair_ui_tests.cpp'))
$winPeAutomaticBootRepairUiText = Get-Content `
    -LiteralPath $winPeAutomaticBootRepairUiPath -Raw
$winPeAutomaticBootRepairGuiText = Get-Content `
    -LiteralPath $winPeAutomaticBootRepairGuiPath -Raw
$bootRepairStandaloneText = Get-Content `
    -LiteralPath $bootRepairStandalonePath -Raw
$winPeAutomaticBootRepairTestText = Get-Content `
    -LiteralPath $winPeAutomaticBootRepairTestPath -Raw
$winPeAutomaticBootRepairUiRequiredPatterns = [ordered]@{
    '\bexplicitly_approved\b' = 'explicit Windows policy approval gate'
    '\bbuild_executable_reviewed_automatic_boot_repair\s*\(' = 'reviewed choices to executable batch mapping'
    'AutomaticThirdPartyEfiPolicy::\s*delete_non_microsoft' = 'explicit EFI delete intent routing'
    'AutomaticWinReRepairDisposition::\s*register_verified_windows_image' = 'explicit verified WinRE registration action'
    '\bbind_reviewed_automatic_boot_repair_winre_images\s*\(' = 'opened-handle Winre.wim evidence binding'
    '\bequivalent_winre_registration_image_identity\s*\(' = 'reviewed Winre.wim identity retention'
    'AutomaticWinReRepairDisposition::\s*normal_boot_only_partial' = 'partial normal-boot classification'
    'BootRepairThirdPartyEfiPolicy::preserve' = 'reviewed preserve-only transaction policy'
    '\befi_boot_ownership_allows_third_party_preserve\s*\(' = 'independent third-party EFI namespace preserve gate'
    '\breviewed_multi_windows_batch\s*=\s*true' = 'batch-only request binding'
}
foreach ($pattern in $winPeAutomaticBootRepairUiRequiredPatterns.Keys) {
    if ($winPeAutomaticBootRepairUiText -notmatch $pattern) {
        $failures += "WinPE automatic boot repair model must retain $($winPeAutomaticBootRepairUiRequiredPatterns[$pattern])"
    }
}
$winPeAutomaticBootRepairGuiRequiredPatterns = [ordered]@{
    '\bprompt_automatic_boot_repair_windows_choice\s*\(' = 'keyboard-operable explicit Windows choice'
    '\bTDF_USE_COMMAND_LINKS\b' = 'exclusive all-or-selected choice controls'
    'config\.nDefaultButton\s*=\s*IDCANCEL' = 'safe default cancellation'
    '\bbuild_product_automatic_boot_repair_choice_request\s*\(' = 'explicit product choice review request'
    '\breview_automatic_boot_repair_choices\s*\(' = 'immutable pure review wiring'
    '\brevalidate_automatic_boot_repair_choices\s*\(' = 'fresh discovery revalidation'
    '\bexecute_multi_windows\s*\(' = 'ordered one-transaction execution'
    '\bobserve_winre_registration_image_with_windows_apis\s*\(' = 'pre-confirmation Winre.wim observation'
    '\bexecute_winre_registration_transaction\s*\(' = 'shared reviewed WinRE registration transaction'
    '\bWinPeAutomaticBootRepairWinReTargetGuard\b' = 'per-mutation target revalidation guard'
    '\.expected_registered_path_kind\s*=\s*action\.expected_registered_path_kind' = 'reviewed Windows fallback registration path binding'
    'failed_rollback_incomplete' = 'WinRE rollback-incomplete partial disclosure'
    '\bnormal_boot_only_partial\b' = 'partial completion differentiation'
    '\bthird_party_efi_preserved\b' = 'preserve-only result disclosure'
    '\bnvram_unchanged\b' = 'no-NVRAM result disclosure'
    '\bprompt_automatic_boot_repair_nvram_choice\s*\(' = 'separate current-PC NVRAM choice'
    '\bexecute_current_pc_nvram_repair\s*\(' = 'audited NVRAM transaction product wiring'
    '\brepair_current_pc_nvram\b' = 'immutable NVRAM disposition binding'
}
foreach ($pattern in $winPeAutomaticBootRepairGuiRequiredPatterns.Keys) {
    if ($winPeAutomaticBootRepairGuiText -notmatch $pattern) {
        $failures += "WinPE automatic boot repair product GUI must retain $($winPeAutomaticBootRepairGuiRequiredPatterns[$pattern])"
    }
}
$bootRepairStandalonePreserveRequiredPatterns = [ordered]@{
    'if\s*\(request\.target\.reviewed_multi_windows_batch\)' = 'single-call reviewed-batch rejection'
    'if\s*\(!target\.reviewed_multi_windows_batch\)' = 'multi transaction pure-review gate'
    'BootRepairThirdPartyEfiPolicy::delete_non_microsoft' = 'third-party EFI delete rejection'
    'request\.third_party_efi_policy\s*!=\s*[\s\S]*?BootRepairThirdPartyEfiPolicy::preserve' = 'explicit third-party preserve gate'
    '\bequivalent_efi_boot_ownership\s*\(' = 'review/final ownership evidence equality'
    '\befi_boot_ownership_allows_third_party_preserve\s*\(' = 'independent namespace-only preservation'
    '\bexecute_multi_windows_bcdboot_with_windows_apis\s*\(' = 'single ordered BCD transaction'
}
foreach ($pattern in $bootRepairStandalonePreserveRequiredPatterns.Keys) {
    if ($bootRepairStandaloneText -notmatch $pattern) {
        $failures += "BootRepair reviewed ESP preserve transaction must retain $($bootRepairStandalonePreserveRequiredPatterns[$pattern])"
    }
}
$winPeAutomaticBootRepairTestRequiredPatterns = [ordered]@{
    '\bwindows_priority_requires_explicit_user_choice\s*\(' = 'all versus selected-only policy coverage'
    '\breviewed_multi_windows_partial_maps_to_one_safe_batch\s*\(' = 'ordered multi-Windows partial coverage'
    '\bthird_party_efi_defaults_to_preserve_and_delete_is_explicit\s*\(' = 'preserve default and explicit delete coverage'
    '\bverified_winre_fallback_is_bound_to_the_registration_transaction\s*\(' = 'WinRE registration binding and drift coverage'
}
foreach ($pattern in $winPeAutomaticBootRepairTestRequiredPatterns.Keys) {
    if ($winPeAutomaticBootRepairTestText -notmatch $pattern) {
        $failures += "WinPE automatic boot repair tests must retain $($winPeAutomaticBootRepairTestRequiredPatterns[$pattern])"
    }
}

# BCD-003 keeps missing ESP/BIOS-system creation in one audited Microsoft VDS
# adapter behind a pure immutable review and a second uppercase-OK transaction.
# The product must re-enter the normal BCD review only after exact final-layout
# readback.  Read-only inventory may classify a pre-format failure, but no raw
# device write API is permitted in this adapter.
$systemPartitionCreationPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'BootRepair\src\system_partition_creation.cpp'))
$systemPartitionCreationWindowsPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'BootRepair\src\system_partition_creation_windows.cpp'))
$systemPartitionCreationTestPath = [IO.Path]::GetFullPath(
    (Join-Path $repoRoot 'tests\Unit\system_partition_creation_tests.cpp'))
$systemPartitionCreationText = Get-Content `
    -LiteralPath $systemPartitionCreationPath -Raw
$systemPartitionCreationWindowsText = Get-Content `
    -LiteralPath $systemPartitionCreationWindowsPath -Raw
$systemPartitionCreationTestText = Get-Content `
    -LiteralPath $systemPartitionCreationTestPath -Raw

$vdsSystemPartitionInterfacePatterns = @(
    '\bIVdsVolumeShrink\b'
    '\bIVdsAdvancedDisk3?\b'
    '\bVDS_ASYNCOUT_SHRINKVOLUME\b'
    '\bVDS_ASYNCOUT_CREATEPARTITION\b'
)
foreach ($pattern in $vdsSystemPartitionInterfacePatterns) {
    $matches = $sourceFiles | Select-String -Pattern $pattern -CaseSensitive
    foreach ($match in $matches) {
        if (-not [IO.Path]::GetFullPath($match.Path).Equals(
                $systemPartitionCreationWindowsPath,
                [StringComparison]::OrdinalIgnoreCase)) {
            $relative = $match.Path.Substring($repoRoot.Length).TrimStart('\')
            $failures += "$relative`:$($match.LineNumber) contains a BCD-003 VDS interface outside the audited adapter"
        }
    }
}
$systemPartitionCreationRequiredPatterns = [ordered]@{
    'confirmation\.typed_token\s*!=\s*L"OK"' = 'separate uppercase OK confirmation gate'
    '\brevalidate_system_partition_creation_review\s*\(' = 'fresh complete pre-mutation review binding'
    '\bexpected_after_shrink\s*\(' = 'exact shrink readback gate'
    '\bexpected_completed_plan\s*\(' = 'exact ESP or Active final-layout gate'
    '\bexpected_raw_disk_layout\s*\(' = 'complete raw inventory cleanup binding'
    '\bafter_failed_shrink\b' = 'uncertain shrink result classification'
    '\battempt_rollback\s*\(' = 'explicit exact rollback state machine'
    'SystemPartitionCreationOutcome::rollback_incomplete' = 'rollback-incomplete distinction'
    'kGptEspBytes\s*=\s*260ULL\s*\*\s*kMiB' = 'fixed 260 MiB GPT ESP plan'
    'kMbrSystemBytes\s*=\s*100ULL\s*\*\s*kMiB' = 'fixed 100 MiB BIOS system plan'
    '\bmbr_primary_only\b' = 'logical and extended MBR rejection'
    'kReclaimabilityMargin' = 'maximum-shrink safety margin'
    'forbidden_volume_role_or_encryption' = 'system role and encryption fail-closed gate'
}
foreach ($pattern in $systemPartitionCreationRequiredPatterns.Keys) {
    if ($systemPartitionCreationText -notmatch $pattern) {
        $failures += "System partition creation core must retain $($systemPartitionCreationRequiredPatterns[$pattern])"
    }
}
$systemPartitionCreationWindowsRequiredPatterns = [ordered]@{
    '\bQueryMaxReclaimableBytes\s*\(' = 'Microsoft VDS read-only shrinkability query'
    'VDS_VF_SYSTEM_VOLUME' = 'current system-volume rejection'
    'VDS_VF_BOOT_VOLUME' = 'current boot-volume rejection'
    'VDS_VF_FVE_ENABLED' = 'BitLocker/FVE rejection'
    '\brevalidate_vds_disk_binding_exact\s*\(' = 'object-bound VDS disk mutation revalidation'
    '\brevalidate_vds_windows_volume_exact\s*\(' = 'object-bound VDS Volume GUID mutation revalidation'
    '\bcleanup_possible_created_partition_exact\s*\(' = 'double-inventory pre-format cleanup gate'
    '\bvalidate_created_vds_partition_exact\s*\(' = 'object-bound created partition cleanup gate'
    '\bShrink\s*\(' = 'single audited exact NTFS shrink seam'
    '\bCreatePartition\s*\(' = 'single audited system-partition creation adapter'
    '\bFormatEx\s*\(' = 'Microsoft VDS exact format seam'
    '\bDeletePartition\s*\(' = 'exact created-partition rollback seam'
    '\bExtend\s*\(' = 'exact Windows extent rollback seam'
    'VDS_PST_GPT' = 'GPT ESP creation branch'
    'VDS_PST_MBR' = 'MBR Active creation branch'
}
foreach ($pattern in $systemPartitionCreationWindowsRequiredPatterns.Keys) {
    if ($systemPartitionCreationWindowsText -notmatch $pattern) {
        $failures += "System partition creation VDS adapter must retain $($systemPartitionCreationWindowsRequiredPatterns[$pattern])"
    }
}
$systemPartitionCreationWindowsForbiddenPatterns = [ordered]@{
    '\bWriteFile\s*\(' = 'raw or file write outside VDS'
    '\bDeviceIoControl\s*\(' = 'raw storage mutation outside VDS'
    '\bCreateProcessW\s*\(' = 'external disk-management process'
}
foreach ($pattern in $systemPartitionCreationWindowsForbiddenPatterns.Keys) {
    if ($systemPartitionCreationWindowsText -match $pattern) {
        $failures += "System partition creation VDS adapter contains forbidden $($systemPartitionCreationWindowsForbiddenPatterns[$pattern])"
    }
}
$systemPartitionCreationTestRequiredPatterns = [ordered]@{
    '\bpure_review_binds_exact_safe_geometry\s*\(' = 'GPT ESP safe geometry coverage'
    '\bmbr_bios_active_creation_uses_its_own_exact_transaction\s*\(' = 'MBR Active successful transaction coverage'
    '\bunsafe_or_insufficient_volume_fails_closed\s*\(' = 'unsafe volume and capacity failure coverage'
    '\bexact_uppercase_ok_is_mandatory\s*\(' = 'lowercase no-call coverage'
    '\bfailed_shrink_is_classified_by_exact_readback\s*\(' = 'uncertain shrink no-write and rollback coverage'
    '\braw_layout_cleanup_binding_requires_every_partition\s*\(' = 'raw cleanup full-layout coverage'
    '\bpost_shrink_failure_rolls_back_exact\s*\(' = 'post-shrink rollback coverage'
    '\bunsafe_post_shrink_volume_state_rolls_back_before_creation\s*\(' = 'post-shrink NTFS health fail-closed coverage'
    '\bchanged_final_layout_is_deleted_and_rolled_back\s*\(' = 'unexpected final geometry rollback coverage'
    '\bpre_mutation_drift_never_calls_shrink\s*\(' = 'pre-mutation drift no-write coverage'
}
foreach ($pattern in $systemPartitionCreationTestRequiredPatterns.Keys) {
    if ($systemPartitionCreationTestText -notmatch $pattern) {
        $failures += "System partition creation tests must retain $($systemPartitionCreationTestRequiredPatterns[$pattern])"
    }
}
$systemPartitionCreationProductRequiredPatterns = [ordered]@{
    '\bprompt_system_partition_creation_windows_choice\s*\(' = 'dedicated destructive-stage target choice'
    '\breview_system_partition_creation\s*\(' = 'pure system-partition review product routing'
    '\bexecute_system_partition_creation\s*\(' = 'reviewed creation transaction product routing'
    '\bcompleted_creation_plan\b' = 'verified completed-layout handoff'
    '\bstart_boot_review\s*\(' = 'normal BCD review continuation'
    'rollback未確認のため、後続の起動修復を開始しません。' = 'rollback-incomplete product fail-closed disclosure'
}
foreach ($pattern in $systemPartitionCreationProductRequiredPatterns.Keys) {
    if ($winPeAutomaticBootRepairGuiText -notmatch $pattern) {
        $failures += "WinPE BCD-003 product route must retain $($systemPartitionCreationProductRequiredPatterns[$pattern])"
    }
}

# BCD-006 shared WinRE registration transaction: review and execution bind the
# exact image file object and expected REAgentC path kind.  Only the signed
# current System32 tool may run, every mutation is guarded, and any failed
# completion verification enters the explicit rollback state machine.
$winReRegistrationPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'BootRepair\src\winre_registration.cpp'))
$winReRegistrationTestPath = [IO.Path]::GetFullPath(
    (Join-Path $repoRoot 'tests\Unit\winre_registration_tests.cpp'))
$winReDiagnosticPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'BootRepair\src\winre_diagnostic.cpp'))
$winReDiagnosticTestPath = [IO.Path]::GetFullPath(
    (Join-Path $repoRoot 'tests\Unit\winre_diagnostic_tests.cpp'))
$winReRegistrationText = Get-Content -LiteralPath $winReRegistrationPath -Raw
$winReRegistrationTestText = Get-Content -LiteralPath $winReRegistrationTestPath -Raw
$winReDiagnosticText = Get-Content -LiteralPath $winReDiagnosticPath -Raw
$winReDiagnosticTestText = Get-Content -LiteralPath $winReDiagnosticTestPath -Raw
$winReRegistrationRequiredPatterns = [ordered]@{
    '\bcandidate_directory_matches_path_kind\s*\(' = 'reviewed directory and REAgentC path-kind binding'
    '\bexpected_registered_path_kind\b' = 'immutable expected registration path kind'
    '\bFILE_FLAG_OPEN_REPARSE_POINT\b' = 'non-reparse ancestor inspection'
    'standard\.NumberOfLinks\s*!=\s*1U' = 'single-link image gate'
    'GENERIC_READ,\s*FILE_SHARE_READ' = 'write and delete sharing denied while image is locked'
    '\bFileIdInfo\b' = 'opened file-object identity'
    '\bFileBasicInfo\b' = 'write and change time identity'
    '\bBCryptHashData\s*\(' = 'complete same-handle SHA-256'
    '\bverify_microsoft_signed\s*\(' = 'Microsoft REAgentC trust gate'
    '\brevalidate_target\s*\(' = 'target revalidation before each mutation and diagnosis'
    '\battempt_rollback\s*\(' = 'explicit rollback transaction'
    'WinReRegistrationOutcome::failed_safe_unregistered' = 'cloned-source safe-unregistered failure state'
    'WinReRegistrationOutcome::failed_rollback_incomplete' = 'rollback-incomplete failure distinction'
}
foreach ($pattern in $winReRegistrationRequiredPatterns.Keys) {
    if ($winReRegistrationText -notmatch $pattern) {
        $failures += "WinRE registration transaction must retain $($winReRegistrationRequiredPatterns[$pattern])"
    }
}
$winReRegistrationForbiddenPatterns = [ordered]@{
    '\bWriteFile\s*\(' = 'direct candidate write'
    '\bGENERIC_WRITE\b' = 'candidate write access'
    '\bDeleteFileW\s*\(' = 'path-only candidate deletion'
    '\bMoveFileExW\s*\(' = 'path-only candidate move'
    '\bReplaceFileW\s*\(' = 'path-only candidate replacement'
}
foreach ($pattern in $winReRegistrationForbiddenPatterns.Keys) {
    if ($winReRegistrationText -match $pattern) {
        $failures += "WinRE registration transaction contains forbidden $($winReRegistrationForbiddenPatterns[$pattern])"
    }
}
$winReRegistrationTestRequiredPatterns = [ordered]@{
    '\btest_expected_registered_path_kind_is_immutable\s*\(' = 'path-kind success, mismatch rollback, and preflight rejection coverage'
    '\btest_validation_trust_and_identity_fail_before_mutation\s*\(' = 'pre-mutation trust and identity failure coverage'
    '\btest_set_failure_restores_verified_prior_registration\s*\(' = 'registered-state rollback coverage'
    '\btest_enable_failure_disables_previously_unregistered_winre\s*\(' = 'unregistered-state rollback coverage'
    '\btest_identity_loss_after_mutation_stops_additional_writes\s*\(' = 'target-loss no-additional-write coverage'
    '\btest_cloned_source_stale_failure_becomes_safe_unregistered\s*\(' = 'cloned stale safe-unregistered coverage'
}
foreach ($pattern in $winReRegistrationTestRequiredPatterns.Keys) {
    if ($winReRegistrationTestText -notmatch $pattern) {
        $failures += "WinRE registration tests must retain $($winReRegistrationTestRequiredPatterns[$pattern])"
    }
}
$winReDiagnosticRequiredPatterns = [ordered]@{
    'WinReRegisteredPathKind::windows_system32_recovery' = 'Windows fallback path classification'
    'allow_mismatched_registered_location_as_cloned_source_stale' = 'direct-clone stale registration intent'
    'registered_location_mismatch_classified_as_cloned_source_stale' = 'explicit stale-source evidence'
    'return Result<WinReDiagnosticReport>::success\(\s*std::move\(report\)\)' = 'foreign path classification without dereference'
}
foreach ($pattern in $winReDiagnosticRequiredPatterns.Keys) {
    if ($winReDiagnosticText -notmatch $pattern) {
        $failures += "WinRE diagnostic must retain $($winReDiagnosticRequiredPatterns[$pattern])"
    }
}
$winReDiagnosticTestRequiredPatterns = [ordered]@{
    '\btest_registered_location_parser_accepts_windows_fallback_path\s*\(' = 'fallback path parser coverage'
    '\btest_cloned_source_stale_mode_never_opens_foreign_path\s*\(' = 'foreign cloned-source path non-dereference coverage'
}
foreach ($pattern in $winReDiagnosticTestRequiredPatterns.Keys) {
    if ($winReDiagnosticTestText -notmatch $pattern) {
        $failures += "WinRE diagnostic tests must retain $($winReDiagnosticTestRequiredPatterns[$pattern])"
    }
}

# BCD-005: current-PC NVRAM repair is confined to one audited adapter. It may
# run only after explicit "this PC" review plus uppercase OK, binds an exact
# GPT ESP device path, appends a missing Windows Boot Manager after the prior
# BootOrder without reordering it, and read-backs every mutation. Tests use a
# mock platform only and never call host firmware APIs.
$nvramRepairPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'BootRepair\src\nvram_repair.cpp'))
$nvramRepairTestPath = [IO.Path]::GetFullPath(
    (Join-Path $repoRoot 'tests\Unit\nvram_repair_tests.cpp'))
$nvramRepairText = Get-Content -LiteralPath $nvramRepairPath -Raw
$nvramRepairTestText = Get-Content -LiteralPath $nvramRepairTestPath -Raw
$firmwareVariableApiPatterns = @(
    '\bGetFirmwareEnvironmentVariableExW\s*\('
    '\bSetFirmwareEnvironmentVariableExW\s*\('
)
foreach ($pattern in $firmwareVariableApiPatterns) {
    $matches = $sourceFiles | Select-String -Pattern $pattern -CaseSensitive
    foreach ($match in $matches) {
        if (-not [IO.Path]::GetFullPath($match.Path).Equals(
                $nvramRepairPath,
                [StringComparison]::OrdinalIgnoreCase)) {
            $relative = $match.Path.Substring($repoRoot.Length).TrimStart('\')
            $failures += "$relative`:$($match.LineNumber) contains a firmware-variable API outside the audited BootRepair adapter"
        }
    }
}
$nvramRepairRequiredPatterns = [ordered]@{
    'explicitly_for_current_pc' = 'explicit this-PC intent binding'
    'confirmation\.typed_token\s*!=\s*L"OK"' = 'uppercase OK confirmation gate'
    'logical_sector_size\s*!=\s*512U' = 'explicit 4Kn fail-closed boundary'
    '\bparse_partition_guid\s*\(' = 'exact GPT ESP signature parsing'
    'kWindowsBootManagerPath' = 'fixed Microsoft boot-manager path'
    'kMaximumBootOrderEntries' = 'bounded BootOrder parsing'
    'kMaximumCandidateProbeCount' = 'bounded Boot-number probing'
    'kEfiSystemPartitionType' = 'exact ESP partition-type gate'
    '\bsame_variable\s*\(' = 'immediate variable race detection'
    '\breplace_efi_global_variable_if_exact\s*\(' = 'conditional firmware mutation and rollback'
    'platform\.revalidate_target\s*\(' = 'fresh target revalidation before mutation'
    'new_order\.push_back\s*\(' = 'missing entry appended after prior BootOrder'
    '\brestore_variable\s*\(' = 'exact-variable rollback path'
    '\bGetFirmwareEnvironmentVariableExW\s*\(' = 'documented firmware read API'
    '\bSetFirmwareEnvironmentVariableExW\s*\(' = 'documented firmware mutation API'
    '\bSE_SYSTEM_ENVIRONMENT_NAME\b' = 'temporary system-environment privilege'
    '\bAdjustTokenPrivileges\s*\(' = 'bounded privilege enable and restore'
}
foreach ($pattern in $nvramRepairRequiredPatterns.Keys) {
    if ($nvramRepairText -notmatch $pattern) {
        $failures += "Current-PC NVRAM transaction must retain $($nvramRepairRequiredPatterns[$pattern])"
    }
}
$nvramRepairForbiddenPatterns = [ordered]@{
    '\bWriteFile\s*\(' = 'file or raw-disk write'
    '\bCreateProcessW\s*\(' = 'external process mutation'
    '\bDeleteFileW\s*\(' = 'path deletion'
    '\bMoveFileExW\s*\(' = 'path move'
    '\bReplaceFileW\s*\(' = 'path replacement'
}
foreach ($pattern in $nvramRepairForbiddenPatterns.Keys) {
    if ($nvramRepairText -match $pattern) {
        $failures += "Current-PC NVRAM transaction contains forbidden $($nvramRepairForbiddenPatterns[$pattern])"
    }
}
$nvramRepairSetCount = ([regex]::Matches(
        $nvramRepairText,
        '\bSetFirmwareEnvironmentVariableExW\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$nvramRepairGetCount = ([regex]::Matches(
        $nvramRepairText,
        '\bGetFirmwareEnvironmentVariableExW\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
if ($nvramRepairSetCount -ne 1 -or $nvramRepairGetCount -ne 1) {
    $failures += 'Current-PC NVRAM adapter must retain exactly one firmware read seam and one serialized conditional mutation callsite'
}
$nvramRepairTestRequiredPatterns = [ordered]@{
    '\btest_load_option_round_trip_and_tamper\s*\(' = 'exact ESP and tamper coverage'
    '\btest_confirmation_stops_before_platform\s*\(' = 'pre-platform confirmation coverage'
    '\btest_existing_active_entry_is_read_only_success\s*\(' = 'verified no-op coverage'
    '\btest_new_entry_appends_without_reordering\s*\(' = 'BootOrder preservation coverage'
    '\btest_inactive_entry_only_activates_same_option\s*\(' = 'same-entry activation coverage'
    '\btest_duplicate_exact_entries_fail_before_write\s*\(' = 'duplicate fail-closed coverage'
    '\btest_target_drift_before_order_write_rolls_back_option\s*\(' = 'pre-order target drift rollback coverage'
    '\btest_boot_order_readback_mismatch_does_not_overwrite_unknown_value\s*\(' = 'concurrent mismatch no-overwrite coverage'
    '\btest_hidden_matching_option_is_used_before_first_absent_slot\s*\(' = 'hidden duplicate avoidance coverage'
    '\btest_final_target_drift_rolls_back_exact_written_values\s*\(' = 'final target revalidation rollback coverage'
    '\btest_non_esp_type_is_pre_platform_failure\s*\(' = 'exact ESP type no-I/O coverage'
    '\btest_unsupported_4kn_is_pre_platform_failure\s*\(' = '4Kn no-I/O coverage'
}
foreach ($pattern in $nvramRepairTestRequiredPatterns.Keys) {
    if ($nvramRepairTestText -notmatch $pattern) {
        $failures += "Current-PC NVRAM tests must retain $($nvramRepairTestRequiredPatterns[$pattern])"
    }
}
$nvramProductTestRequiredPatterns = [ordered]@{
    '\bcurrent_pc_nvram_requires_separate_explicit_uefi_choice\s*\(' = 'product explicit-choice and immutable-binding coverage'
}
foreach ($pattern in $nvramProductTestRequiredPatterns.Keys) {
    if ($winPeAutomaticBootRepairTestText -notmatch $pattern) {
        $failures += "WinPE automatic boot repair tests must retain $($nvramProductTestRequiredPatterns[$pattern])"
    }
}

# BCD-004 separates its pure immutable planner from one audited Win32 adapter.
# Product wiring may reach only the non-injected factory after dedicated
# review, uppercase OK and the core's execution-time exact reinspection.
$efiDeleteTransactionPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'BootRepair\src\efi_delete_transaction.cpp'))
$efiDeleteTransactionHeaderPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'BootRepair\include\ytec\bootrepair\efi_delete_transaction.h'))
$efiDeleteTransactionTestPath = [IO.Path]::GetFullPath(
    (Join-Path $repoRoot 'tests\Unit\efi_delete_transaction_tests.cpp'))
$efiDeleteWindowsPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'BootRepair\src\efi_delete_transaction_windows.cpp'))
$efiDeleteWindowsHeaderPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'BootRepair\include\ytec\bootrepair\efi_delete_transaction_windows.h'))
$efiDeleteWindowsTestPath = [IO.Path]::GetFullPath(
    (Join-Path $repoRoot 'tests\Unit\efi_delete_transaction_windows_tests.cpp'))
$efiDeleteWinPeUiPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'WinPEApp\src\automatic_boot_repair_ui.cpp'))
$efiDeleteWinPeGuiPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'WinPEApp\src\gui_main.cpp'))
$efiDeleteTransactionText = Get-Content `
    -LiteralPath $efiDeleteTransactionPath -Raw
$efiDeleteTransactionHeaderText = Get-Content `
    -LiteralPath $efiDeleteTransactionHeaderPath -Raw
$efiDeleteTransactionTestText = Get-Content `
    -LiteralPath $efiDeleteTransactionTestPath -Raw
$efiDeleteWindowsText = Get-Content -LiteralPath $efiDeleteWindowsPath -Raw
$efiDeleteWindowsHeaderText = Get-Content `
    -LiteralPath $efiDeleteWindowsHeaderPath -Raw
$efiDeleteWindowsTestText = Get-Content `
    -LiteralPath $efiDeleteWindowsTestPath -Raw
$efiDeleteWinPeUiText = Get-Content -LiteralPath $efiDeleteWinPeUiPath -Raw
$efiDeleteWinPeGuiText = Get-Content -LiteralPath $efiDeleteWinPeGuiPath -Raw
$auditedEfiDeleteWindowsPatterns = @(
    '\bSetFileInformationByHandle\s*\('
    '\bDELETE\b'
)
$efiDeleteRequiredPatterns = [ordered]@{
    'confirmation\.typed_token\s*!=\s*L"OK"' = 'uppercase OK confirmation gate'
    '\bvalidate_stable_identity\s*\(' = 'fresh stable disk identity gate'
    '\bequivalent_efi_delete_esp_identity\s*\(' = 'exact ESP identity gate'
    'partition_type_identifier' = 'GPT ESP type identity binding'
    'partition_attributes' = 'GPT partition attribute identity binding'
    'filesystem_name' = 'FAT32 filesystem identity binding'
    '\bequivalent_efi_delete_manifest\s*\(' = 'full tree manifest gate'
    '\befi_boot_ownership_allows_third_party_preserve\s*\(' = 'independent namespace ownership gate'
    'kMaximumEfiDeleteTreeEntries' = 'bounded tree count'
    'kMaximumEfiDeleteTreeDepth' = 'bounded tree depth'
    '\bBCryptHashData\s*\(' = 'immutable SHA-256 manifest'
    '\brollback_quarantined_candidates\s*\(' = 'exact reverse rollback state machine'
    'microsoft_bcd_rebuild_readback_verified' = 'BCD readback commit gate'
    '\brollback_microsoft_bcd_rebuild_if_identity_matches\s*\(' = 'exact BCD rollback after no-delete stop'
    '\bcommit_microsoft_bcd_rebuild\s*\(' = 'retained BCD rollback boundary commit'
    'EfiDeleteTransactionOutcome::partial_rollback' = 'partial rollback distinction'
    'EfiDeleteTransactionOutcome::partial_delete' = 'partial delete distinction'
}
foreach ($pattern in $efiDeleteRequiredPatterns.Keys) {
    if ($efiDeleteTransactionText -notmatch $pattern) {
        $failures += "EFI delete transaction core must retain $($efiDeleteRequiredPatterns[$pattern])"
    }
}
$efiDeleteHeaderRequiredPatterns = [ordered]@{
    'class IEfiDeleteTransactionPlatform' = 'injected platform contract'
    '\bmove_candidate_to_quarantine_handle_bound\s*\(' = 'handle-bound no-replace quarantine move contract'
    '\bdelete_quarantined_candidate_tree_handle_bound\s*\(' = 'handle-bound recursive deletion contract'
    '\brollback_microsoft_bcd_rebuild_if_identity_matches\s*\(' = 'handle-bound BCD rollback contract'
    '\bcommit_microsoft_bcd_rebuild\s*\(' = 'BCD rollback boundary commit contract'
    '\bEfiDeletePlatformFailureKind\b' = 'foreign tamper race failure taxonomy'
}
foreach ($pattern in $efiDeleteHeaderRequiredPatterns.Keys) {
    if ($efiDeleteTransactionHeaderText -notmatch $pattern) {
        $failures += "EFI delete transaction header must retain $($efiDeleteHeaderRequiredPatterns[$pattern])"
    }
}
$efiDeleteForbiddenPatterns = [ordered]@{
    '\bCreateFileW\s*\(' = 'production filesystem adapter'
    '\bMoveFileExW\s*\(' = 'path-only move'
    '\bDeleteFileW\s*\(' = 'path-only file deletion'
    '\bRemoveDirectoryW\s*\(' = 'path-only directory deletion'
    '\bSetFileInformationByHandle\s*\(' = 'unreviewed destructive Win32 adapter'
    '\bWriteFile\s*\(' = 'direct filesystem write'
    '\bGENERIC_WRITE\b' = 'filesystem write access'
}
foreach ($pattern in $efiDeleteForbiddenPatterns.Keys) {
    if ($efiDeleteTransactionText -match $pattern -or
        $efiDeleteTransactionHeaderText -match $pattern) {
        $failures += "EFI delete pure core contains forbidden $($efiDeleteForbiddenPatterns[$pattern])"
    }
}
$efiDeleteTestRequiredPatterns = [ordered]@{
    '\btest_manifest_is_canonical_and_complete\s*\(' = 'canonical manifest coverage'
    '\btest_review_rejects_unsafe_names_and_object_types\s*\(' = 'unsafe path object coverage'
    '\btest_confirmation_and_fresh_identity_stop_before_writes\s*\(' = 'pre-mutation gates'
    '\btest_foreign_quarantine_and_move_race_are_distinct\s*\(' = 'foreign and race distinction'
    '\btest_partial_rollback_is_not_reported_as_recovered\s*\(' = 'partial rollback coverage'
    '\btest_bcd_failure_rolls_back_before_any_delete\s*\(' = 'BCD-before-delete ordering'
    '\btest_success_orders_quarantine_bcd_delete_and_cleanup\s*\(' = 'successful transaction order'
    '\btest_final_delete_and_cleanup_failures_remain_explicit\s*\(' = 'partial finalization coverage'
    'partition_attributes\s*\^=' = 'fresh GPT ESP attribute drift coverage'
    'quarantine_identity\.volume_serial_number\s*\^=' = 'foreign-volume quarantine rejection coverage'
    'move completion unknown' = 'partial-or-unknown quarantine move coverage'
    'contradictory mutation evidence' = 'invalid platform step-result coverage'
    'microsoft_bcd_rolled_back_after_delete_stop' = 'post-BCD exact rollback coverage'
    '\bbcd_rollback_failure\b' = 'BCD rollback failure coverage'
    '\bbcd_commit_failure\b' = 'BCD rollback-boundary commit failure coverage'
}
foreach ($pattern in $efiDeleteTestRequiredPatterns.Keys) {
    if ($efiDeleteTransactionTestText -notmatch $pattern) {
        $failures += "EFI delete transaction tests must retain $($efiDeleteTestRequiredPatterns[$pattern])"
    }
}
$efiDeleteWindowsRequiredPatterns = [ordered]@{
    '\bNtCreateFile\b' = 'RootDirectory-relative non-reparse open'
    '\bSetFileInformationByHandle\s*\(' = 'handle-bound rename and disposition'
    'ReplaceIfExists\s*=\s*FALSE' = 'no-replace quarantine rename'
    '\bFileDispositionInfo\b' = 'handle-bound recursive deletion'
    '\bIOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS\b' = 'single-extent ESP identity'
    '\bIOCTL_DISK_GET_PARTITION_INFO_EX\b' = 'GPT type/id/attributes identity'
    '\bGetVolumeInformationW\s*\(' = 'FAT32 and serial identity'
    '\bverify_candidate_tree_handle\s*\(' = 'exact manifest readback'
    '\brollback_microsoft_bcd_rebuild_if_identity_matches\s*\(' = 'outer BCD exact rollback'
    'kBcdRollbackSuffix' = 'fixed owned outer BCD backup'
    '\bclassify_windows_efi_delete_failed_bcd_backup_move_readback\s*\(' = 'outer BCD move failure exact readback classification'
    'source_after_failure\s*&&\s*destination_after_failure' = 'query-success gate before no-mutation classification'
    '\bsource_after_move\b' = 'successful outer BCD move source-absence readback'
    '\bdestination_after_move\b' = 'successful outer BCD move destination identity readback'
    'prior_bcd_identity_\s*=\s*destination_after_move\.value\(\)\.value\(\)' = 'post-rename exact BCD rollback identity retention'
}
foreach ($pattern in $efiDeleteWindowsRequiredPatterns.Keys) {
    if ($efiDeleteWindowsText -notmatch $pattern) {
        $failures += "EFI delete Win32 adapter must retain $($efiDeleteWindowsRequiredPatterns[$pattern])"
    }
}
$efiDeleteWindowsForbiddenPatterns = [ordered]@{
    '\bDeleteFileW\s*\(' = 'path-only file deletion'
    '\bRemoveDirectoryW\s*\(' = 'path-only directory deletion'
    '\bMoveFile(?:Ex)?W\s*\(' = 'path-only rename'
    '\bGENERIC_WRITE\b' = 'broad filesystem write access'
}
foreach ($pattern in $efiDeleteWindowsForbiddenPatterns.Keys) {
    if ($efiDeleteWindowsText -match $pattern -or
        $efiDeleteWindowsHeaderText -match $pattern) {
        $failures += "EFI delete Win32 adapter contains forbidden $($efiDeleteWindowsForbiddenPatterns[$pattern])"
    }
}
$efiDeleteProductRequiredPatterns = [ordered]@{
    '保持（既定）' = 'non-destructive default choice'
    '削除（危険）' = 'explicit dangerous choice'
    '\bformat_reviewed_efi_delete_plan\s*\(' = 'dedicated immutable review output'
    '\bmake_windows_efi_delete_transaction_platform\s*\(' = 'production adapter factory'
    '\bexecute_efi_delete_transaction\s*\(' = 'core transaction routing'
    'typed_token\s*=\s*L"OK"' = 'uppercase OK transaction gate'
    'EfiDeleteTransactionOutcome::committed' = 'explicit result classification gate'
}
foreach ($pattern in $efiDeleteProductRequiredPatterns.Keys) {
    if ($efiDeleteWinPeGuiText -notmatch $pattern) {
        $failures += "WinPE EFI delete product wiring must retain $($efiDeleteProductRequiredPatterns[$pattern])"
    }
}
if ($efiDeleteWinPeGuiText -match
        '\bmake_windows_efi_delete_transaction_platform_for_failure_injection\s*\(') {
    $failures += 'WinPE product source must not call the EFI delete failure-injection factory'
}
if ($efiDeleteWinPeUiText -notmatch
        '\bthird_party_efi_delete_explicitly_approved\b' -or
    $efiDeleteWinPeUiText -notmatch
        '\bbuild_windows_efi_delete_esp_request\s*\(') {
    $failures += 'WinPE EFI delete pure UI contract must retain explicit approval and exact ESP routing'
}
if ($efiDeleteWindowsTestText -notmatch
        '\bfactory_rejects_unsafe_bcd_batches_without_io\s*\(' -or
    $efiDeleteWindowsTestText -notmatch
        '\bfailure_extent_is_honest\s*\(' -or
    $efiDeleteWindowsTestText -notmatch
        '\bfailed_bcd_backup_move_readback_is_fail_closed\s*\(' -or
    $efiDeleteWindowsTestText -notmatch
        '\bafter_bcd_backup_move\b') {
    $failures += 'EFI delete Windows tests must retain no-I/O factory, failure-extent, and outer-BCD move-readback coverage'
}
$auditedShrinkBundleWriterPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'MigrationEngine\src\bundle_capture.cpp'))
$auditedShrinkBundleWriterPatterns = @(
    '\bWriteFile\s*\('
    '\bGENERIC_WRITE\b'
    '\bMoveFileExW\s*\('
    '\bDeleteFileW\s*\('
)
$auditedCheckpointWriterPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'OperationCore\src\checkpoint.cpp'))
$auditedCheckpointWriterPatterns = @(
    '\bWriteFile\s*\('
    '\bGENERIC_WRITE\b'
    '\bMoveFileExW\s*\('
    '\bReplaceFileW\s*\('
    '\bSetFileInformationByHandle\s*\('
    '\bDELETE\b'
)
$auditedWindowsResumeSlotWriterPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'OperationCore\src\windows_resume_slot_platform.cpp'))
$auditedWindowsResumeSlotWriterPatterns = @(
    '\bWriteFile\s*\('
    '\bGENERIC_WRITE\b'
    '\bMoveFileExW\s*\('
    '\bReplaceFileW\s*\('
    '\bSetFileInformationByHandle\s*\('
    '\bDELETE\b'
)
$auditedTsumugiStreamWriterPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'ImageFormat\src\tsumugi_stream.cpp'))
$auditedTsumugiStreamWriterPatterns = @(
    '\bWriteFile\s*\('
    '\bGENERIC_WRITE\b'
    '\bDeleteFileW\s*\('
    '\bSetEndOfFile\s*\('
    '\bSetFileInformationByHandle\s*\('
    '\bDELETE\b'
)
$auditedTsumugiRescueStagingPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'ImageFormat\src\windows_tsumugi_rescue_staging.cpp'))
$auditedTsumugiRescueStagingPatterns = @(
    '\bWriteFile\s*\('
    '\bGENERIC_WRITE\b'
    '\bSetEndOfFile\s*\('
    '\bSetFileInformationByHandle\s*\('
    '\bDELETE\b'
)
$auditedStartupDataProbePath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'WindowsApp\src\startup_data_policy.cpp'))
$auditedStartupDataProbePatterns = @(
    '\bWriteFile\s*\('
    '\bGENERIC_WRITE\b'
    '\bDELETE\b'
)
$auditedAdkAcquisitionPlatformPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'WindowsApp\src\windows_adk_acquisition_platform.cpp'))
$auditedAdkAcquisitionPlatformPatterns = @(
    '\bWriteFile\s*\('
    '\bGENERIC_WRITE\b'
    '\bSetFileInformationByHandle\s*\('
    '\bDELETE\b'
)
$auditedWindowsAdkEulaExtractorPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'WindowsApp\src\windows_adk_eula_extractor.cpp'))
$auditedWindowsAdkEulaExtractorPatterns = @(
    '\bWriteFile\s*\('
    '\bGENERIC_WRITE\b'
    '\bSetFileInformationByHandle\s*\('
    '\bDELETE\b'
)
$auditedWindowsAdkManagementPlatformPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'WindowsApp\src\windows_adk_management_platform.cpp'))
$auditedWindowsAdkManagementPlatformPatterns = @(
    '\bWriteFile\s*\('
    '\bGENERIC_WRITE\b'
    '\bSetFileInformationByHandle\s*\('
    '\bDELETE\b'
)
$auditedProductLogRetentionPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'WindowsApp\src\log_retention.cpp'))
$auditedProductLogRetentionPatterns = @(
    '\bSetFileInformationByHandle\s*\('
    '\bDELETE\b'
)
$auditedWindowsNtfsShrinkCapturePath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'WindowsApp\src\windows_ntfs_shrink_capture.cpp'))
$auditedWindowsNtfsShrinkCapturePatterns = @(
    '\bSetFileInformationByHandle\s*\('
    '\bDELETE\b'
)
$auditedWindowsShrinkRestorePlatformPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'WindowsApp\src\windows_shrink_restore_platform.cpp'))
$auditedWindowsShrinkRestorePlatformPatterns = @(
    '\bWriteFile\s*\('
    '\bGENERIC_WRITE\b'
    '\bSetEndOfFile\s*\('
    '\bSetFileInformationByHandle\s*\('
    '\bDELETE\b'
    '\bFSCTL_LOCK_VOLUME\b'
    '\bFSCTL_DISMOUNT_VOLUME\b'
    '\bFSCTL_EXTEND_VOLUME\b'
    '\bIOCTL_VOLUME_OFFLINE\b'
)
$auditedWindowsFileSystemRecreateTargetPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'MigrationEngine\src\windows_file_system_recreate_target_io.cpp'))
$auditedWindowsFileSystemRecreateTargetPatterns = @(
    '\bWriteFile\s*\('
    '\bGENERIC_WRITE\b'
    '\bSetEndOfFile\s*\('
    '\bSetFileInformationByHandle\s*\('
    '\bFSCTL_LOCK_VOLUME\b'
    '\bFSCTL_DISMOUNT_VOLUME\b'
    '\bIOCTL_VOLUME_OFFLINE\b'
)
$auditedWindowsOnlineDirectCloneSourcePath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'WindowsApp\src\online_direct_clone.cpp'))
$auditedWindowsOnlineDirectCloneSourcePatterns = @(
    '\bFSCTL_LOCK_VOLUME\b'
)
$auditedWindowsDirectShrinkClonePlatformPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'WindowsApp\src\windows_direct_shrink_clone_platform.cpp'))
$windowsOnlineDirectShrinkClonePath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'WindowsApp\src\online_direct_shrink_clone.cpp'))
$windowsDirectShrinkClonePlatformTestPath = [IO.Path]::GetFullPath(
    (Join-Path $repoRoot 'tests\Unit\windows_direct_shrink_clone_platform_tests.cpp'))
$windowsOnlineDirectShrinkCloneTestPath = [IO.Path]::GetFullPath(
    (Join-Path $repoRoot 'tests\Unit\windows_online_direct_shrink_clone_tests.cpp'))
$windowsDirectShrinkProductUiPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'WindowsApp\src\main.cpp'))
$cloneBootFinalizationPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'BootRepair\src\clone_boot_finalization.cpp'))
$cloneBootFinalizationTestPath = [IO.Path]::GetFullPath(
    (Join-Path $repoRoot 'tests\Unit\clone_boot_finalization_tests.cpp'))
$auditedWindowsDirectShrinkClonePlatformPatterns = @(
    '\bGENERIC_WRITE\b'
    '\bSetFileInformationByHandle\s*\('
    '\bDELETE\b'
)
$auditedSupportZipWriterPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'WindowsApp\src\support_zip.cpp'))
$auditedSupportZipWriterPatterns = @(
    '\bWriteFile\s*\('
    '\bGENERIC_WRITE\b'
    '\bSetFileInformationByHandle\s*\('
    '\bDELETE\b'
)
$auditedFirstRunGuidanceWriterPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'WindowsApp\src\first_run_guidance.cpp'))
$auditedFirstRunGuidanceWriterPatterns = @(
    '\bWriteFile\s*\('
    '\bGENERIC_WRITE\b'
    '\bSetFileInformationByHandle\s*\('
    '\bDELETE\b'
)

foreach ($pattern in $forbiddenSourcePatterns.Keys) {
    $matches = $sourceFiles | Select-String -Pattern $pattern -CaseSensitive
    foreach ($match in $matches) {
        $isAuditedPhysicalWriter = [IO.Path]::GetFullPath($match.Path).Equals(
            $auditedPhysicalWriterPath,
            [StringComparison]::OrdinalIgnoreCase)
        if ($isAuditedPhysicalWriter -and $auditedPhysicalWriterPatterns -contains $pattern) {
            continue
        }
        $isAuditedImageFileWriter = [IO.Path]::GetFullPath($match.Path).Equals(
            $auditedImageFileWriterPath,
            [StringComparison]::OrdinalIgnoreCase)
        if ($isAuditedImageFileWriter -and $auditedImageFileWriterPatterns -contains $pattern) {
            continue
        }
        $isAuditedLogFileWriter = [IO.Path]::GetFullPath($match.Path).Equals(
            $auditedLogFileWriterPath,
            [StringComparison]::OrdinalIgnoreCase)
        if ($isAuditedLogFileWriter -and $auditedLogFileWriterPatterns -contains $pattern) {
            continue
        }
        $isAuditedWinPeDiskPartScriptWriter =
            [IO.Path]::GetFullPath($match.Path).Equals(
                $auditedWinPeDiskPartScriptWriterPath,
                [StringComparison]::OrdinalIgnoreCase)
        if ($isAuditedWinPeDiskPartScriptWriter -and
            $auditedWinPeDiskPartScriptWriterPatterns -contains $pattern) {
            continue
        }
        $isAuditedBcdStoreTransaction =
            [IO.Path]::GetFullPath($match.Path).Equals(
                $auditedBcdStoreTransactionPath,
                [StringComparison]::OrdinalIgnoreCase)
        if ($isAuditedBcdStoreTransaction -and
            $auditedBcdStoreTransactionPatterns -contains $pattern) {
            continue
        }
        $isAuditedEfiDeleteWindows =
            [IO.Path]::GetFullPath($match.Path).Equals(
                $efiDeleteWindowsPath,
                [StringComparison]::OrdinalIgnoreCase)
        if ($isAuditedEfiDeleteWindows -and
            $auditedEfiDeleteWindowsPatterns -contains $pattern) {
            continue
        }
        $isAuditedShrinkBundleWriter =
            [IO.Path]::GetFullPath($match.Path).Equals(
                $auditedShrinkBundleWriterPath,
                [StringComparison]::OrdinalIgnoreCase)
        if ($isAuditedShrinkBundleWriter -and
            $auditedShrinkBundleWriterPatterns -contains $pattern) {
            continue
        }
        $isAuditedCheckpointWriter =
            [IO.Path]::GetFullPath($match.Path).Equals(
                $auditedCheckpointWriterPath,
                [StringComparison]::OrdinalIgnoreCase)
        if ($isAuditedCheckpointWriter -and
            $auditedCheckpointWriterPatterns -contains $pattern) {
            continue
        }
        $isAuditedWindowsResumeSlotWriter =
            [IO.Path]::GetFullPath($match.Path).Equals(
                $auditedWindowsResumeSlotWriterPath,
                [StringComparison]::OrdinalIgnoreCase)
        if ($isAuditedWindowsResumeSlotWriter -and
            $auditedWindowsResumeSlotWriterPatterns -contains $pattern) {
            continue
        }
        $isAuditedTsumugiStreamWriter =
            [IO.Path]::GetFullPath($match.Path).Equals(
                $auditedTsumugiStreamWriterPath,
                [StringComparison]::OrdinalIgnoreCase)
        if ($isAuditedTsumugiStreamWriter -and
            $auditedTsumugiStreamWriterPatterns -contains $pattern) {
            continue
        }
        $isAuditedTsumugiRescueStaging =
            [IO.Path]::GetFullPath($match.Path).Equals(
                $auditedTsumugiRescueStagingPath,
                [StringComparison]::OrdinalIgnoreCase)
        if ($isAuditedTsumugiRescueStaging -and
            $auditedTsumugiRescueStagingPatterns -contains $pattern) {
            continue
        }
        $isAuditedStartupDataProbe =
            [IO.Path]::GetFullPath($match.Path).Equals(
                $auditedStartupDataProbePath,
                [StringComparison]::OrdinalIgnoreCase)
        if ($isAuditedStartupDataProbe -and
            $auditedStartupDataProbePatterns -contains $pattern) {
            continue
        }
        $isAuditedAdkAcquisitionPlatform =
            [IO.Path]::GetFullPath($match.Path).Equals(
                $auditedAdkAcquisitionPlatformPath,
                [StringComparison]::OrdinalIgnoreCase)
        if ($isAuditedAdkAcquisitionPlatform -and
            $auditedAdkAcquisitionPlatformPatterns -contains $pattern) {
            continue
        }
        $isAuditedWindowsAdkEulaExtractor =
            [IO.Path]::GetFullPath($match.Path).Equals(
                $auditedWindowsAdkEulaExtractorPath,
                [StringComparison]::OrdinalIgnoreCase)
        if ($isAuditedWindowsAdkEulaExtractor -and
            $auditedWindowsAdkEulaExtractorPatterns -contains $pattern) {
            continue
        }
        $isAuditedWindowsAdkManagementPlatform =
            [IO.Path]::GetFullPath($match.Path).Equals(
                $auditedWindowsAdkManagementPlatformPath,
                [StringComparison]::OrdinalIgnoreCase)
        if ($isAuditedWindowsAdkManagementPlatform -and
            $auditedWindowsAdkManagementPlatformPatterns -contains $pattern) {
            continue
        }
        $isAuditedProductLogRetention =
            [IO.Path]::GetFullPath($match.Path).Equals(
                $auditedProductLogRetentionPath,
                [StringComparison]::OrdinalIgnoreCase)
        if ($isAuditedProductLogRetention -and
            $auditedProductLogRetentionPatterns -contains $pattern) {
            continue
        }
        $isAuditedWindowsNtfsShrinkCapture =
            [IO.Path]::GetFullPath($match.Path).Equals(
                $auditedWindowsNtfsShrinkCapturePath,
                [StringComparison]::OrdinalIgnoreCase)
        if ($isAuditedWindowsNtfsShrinkCapture -and
            $auditedWindowsNtfsShrinkCapturePatterns -contains $pattern) {
            continue
        }
        $isAuditedWindowsShrinkRestorePlatform =
            [IO.Path]::GetFullPath($match.Path).Equals(
                $auditedWindowsShrinkRestorePlatformPath,
                [StringComparison]::OrdinalIgnoreCase)
        if ($isAuditedWindowsShrinkRestorePlatform -and
            $auditedWindowsShrinkRestorePlatformPatterns -contains $pattern) {
            continue
        }
        $isAuditedWindowsFileSystemRecreateTarget =
            [IO.Path]::GetFullPath($match.Path).Equals(
                $auditedWindowsFileSystemRecreateTargetPath,
                [StringComparison]::OrdinalIgnoreCase)
        if ($isAuditedWindowsFileSystemRecreateTarget -and
            $auditedWindowsFileSystemRecreateTargetPatterns -contains $pattern) {
            continue
        }

        $isAuditedWindowsOnlineDirectCloneSource =
            [IO.Path]::GetFullPath($match.Path).Equals(
                $auditedWindowsOnlineDirectCloneSourcePath,
                [StringComparison]::OrdinalIgnoreCase)
        if ($isAuditedWindowsOnlineDirectCloneSource -and
            $auditedWindowsOnlineDirectCloneSourcePatterns -contains $pattern) {
            continue
        }
        $isAuditedWindowsDirectShrinkClonePlatform =
            [IO.Path]::GetFullPath($match.Path).Equals(
                $auditedWindowsDirectShrinkClonePlatformPath,
                [StringComparison]::OrdinalIgnoreCase)
        if ($isAuditedWindowsDirectShrinkClonePlatform -and
            $auditedWindowsDirectShrinkClonePlatformPatterns -contains $pattern) {
            continue
        }
        $isAuditedSupportZipWriter =
            [IO.Path]::GetFullPath($match.Path).Equals(
                $auditedSupportZipWriterPath,
                [StringComparison]::OrdinalIgnoreCase)
        if ($isAuditedSupportZipWriter -and
            $auditedSupportZipWriterPatterns -contains $pattern) {
            continue
        }
        $isAuditedFirstRunGuidanceWriter =
            [IO.Path]::GetFullPath($match.Path).Equals(
                $auditedFirstRunGuidanceWriterPath,
                [StringComparison]::OrdinalIgnoreCase)
        if ($isAuditedFirstRunGuidanceWriter -and
            $auditedFirstRunGuidanceWriterPatterns -contains $pattern) {
            continue
        }
        $relative = $match.Path.Substring($repoRoot.Length).TrimStart('\')
        $failures += "$relative`:$($match.LineNumber) contains $($forbiddenSourcePatterns[$pattern])"
    }
}

$startupDataProbeText = Get-Content `
    -LiteralPath $auditedStartupDataProbePath `
    -Raw
$startupDataProbeRequiredPatterns = [ordered]@{
    '\bCREATE_NEW\b' = 'non-overwriting private probe creation'
    '\bFILE_FLAG_DELETE_ON_CLOSE\b' = 'automatic probe cleanup'
    '\bFILE_FLAG_WRITE_THROUGH\b' = 'write-through probe creation'
    '\bFILE_FLAG_OPEN_REPARSE_POINT\b' = 'non-reparse directory inspection'
    '\bGetFileInformationByHandleEx\s*\(' = 'handle-based directory inspection'
    '\bFlushFileBuffers\s*\(' = 'probe flush before verification'
    '\bReadFile\s*\(' = 'probe readback verification'
    '\bevaluate_tsumugi_portable_data_path_gate\s*\(' = 'pure EXE-adjacent data image-path gate'
    '\bpath_is_inside_or_equal\s*\(' = 'case-insensitive component-boundary check'
    'canonical_partial_path' = 'adjacent .partial path proof'
}
foreach ($pattern in $startupDataProbeRequiredPatterns.Keys) {
    if ($startupDataProbeText -notmatch $pattern) {
        $failures += "WindowsApp startup data probe must retain $($startupDataProbeRequiredPatterns[$pattern])"
    }
}
$startupDataWriteFileCount = ([regex]::Matches(
        $startupDataProbeText,
        '\bWriteFile\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$startupDataGenericWriteCount = ([regex]::Matches(
        $startupDataProbeText,
        '\bGENERIC_WRITE\b',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$startupDataDeleteAccessCount = ([regex]::Matches(
        $startupDataProbeText,
        '\bDELETE\b',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
if ($startupDataWriteFileCount -ne 1 -or
    $startupDataGenericWriteCount -ne 1 -or
    $startupDataDeleteAccessCount -ne 1) {
    $failures += 'WindowsApp startup data probe may contain exactly one WriteFile, GENERIC_WRITE, and DELETE access'
}

$adkAcquisitionPlatformText = Get-Content `
    -LiteralPath $auditedAdkAcquisitionPlatformPath `
    -Raw
$adkAcquisitionRequiredPatterns = [ordered]@{
    '\bCREATE_NEW\b' = 'CREATE_NEW staging payload creation'
    '\bFILE_FLAG_OPEN_REPARSE_POINT\b' = 'handle-based reparse rejection'
    '\bFILE_SHARE_READ\b' = 'read-only source and prelaunch locking'
    '\bFlushFileBuffers\s*\(' = 'staged payload flush before verification'
    '\bmaximum_bytes\b' = 'bounded download and offline copy'
    '\blink_count\s*!=\s*1U' = 'hard-link rejection'
    '\bWINHTTP_ACCESS_TYPE_NO_PROXY\b' = 'ambient proxy credential avoidance'
    '\bWINHTTP_DISABLE_AUTHENTICATION\b' = 'HTTP authentication disablement'
    '\bWINHTTP_DISABLE_REDIRECTS\b' = 'manual exact redirect handling'
    '\bWINHTTP_OPTION_REDIRECT_POLICY_NEVER\b' = 'automatic redirect prohibition'
    '\bWINHTTP_OPTION_SECURE_PROTOCOLS\b' = 'explicit TLS protocol floor'
    '\bWINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2\b' = 'TLS 1.2 minimum'
    '\bvalidate_windows_adk_download_policy\s*\(' = 'pure exact URL policy validation'
    '\bsha256_locked_handle\s*\(' = 'same-handle SHA-256 revalidation'
    '\bverify_microsoft_signed\s*\(' = 'Microsoft Authenticode signer verification'
    '\bquery_file_version_unlocked\s*\(' = 'file-version revalidation'
    '\bexpand_and_verify_patch_archive\s*\(' = 'bounded pinned ZIP/MSP expansion and verification'
    '\bvalidate_and_build_windows_adk_launch_plan\s*\(' = 'fixed shell-free launch plan'
    'if\s*\(!CreateProcessW\s*\(' = 'direct CreateProcess launch'
    '\bmark_handle_for_delete\s*\(' = 'handle-identity cleanup'
}
foreach ($pattern in $adkAcquisitionRequiredPatterns.Keys) {
    if ($adkAcquisitionPlatformText -notmatch $pattern) {
        $failures += "Windows ADK acquisition platform must retain $($adkAcquisitionRequiredPatterns[$pattern])"
    }
}
$adkAcquisitionForbiddenPatterns = [ordered]@{
    '\bCREATE_ALWAYS\b' = 'CREATE_ALWAYS overwrite'
    '\bOPEN_ALWAYS\b' = 'OPEN_ALWAYS existing-file reuse'
    '\bTRUNCATE_EXISTING\b' = 'TRUNCATE_EXISTING overwrite'
    '\bFILE_SHARE_DELETE\b' = 'delete sharing during identity lock'
    '\bWINHTTP_ACCESS_TYPE_DEFAULT_PROXY\b' = 'ambient proxy use'
    '\bWINHTTP_OPTION_SECURITY_FLAGS\b' = 'TLS certificate relaxation'
    '\bWinHttpSetCredentials\s*\(' = 'HTTP credential forwarding'
    '\bShellExecute(?:Ex)?W?\s*\(' = 'shell execution'
    '\bWinExec\s*\(' = 'legacy shell-like execution'
    '\bURLDownloadToFileW?\s*\(' = 'unbounded URL download helper'
    '\bCopyFile(?:Ex)?W?\s*\(' = 'path-only offline copy'
    '\bremove_all\s*\(' = 'recursive broad cleanup'
    '\bTerminateProcess\s*\(' = 'forced installer termination'
}
foreach ($pattern in $adkAcquisitionForbiddenPatterns.Keys) {
    if ($adkAcquisitionPlatformText -match $pattern) {
        $failures += "Windows ADK acquisition platform contains forbidden $($adkAcquisitionForbiddenPatterns[$pattern])"
    }
}
$adkWriteFileCount = ([regex]::Matches(
        $adkAcquisitionPlatformText,
        '\bWriteFile\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$adkGenericWriteCount = ([regex]::Matches(
        $adkAcquisitionPlatformText,
        '\bGENERIC_WRITE\b',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$adkDispositionCount = ([regex]::Matches(
        $adkAcquisitionPlatformText,
        '\bSetFileInformationByHandle\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$adkDeleteAccessCount = ([regex]::Matches(
        $adkAcquisitionPlatformText,
        '\bDELETE\b',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
if ($adkWriteFileCount -ne 3 -or
    $adkGenericWriteCount -ne 1 -or
    $adkDispositionCount -ne 1 -or
    $adkDeleteAccessCount -ne 3) {
    $failures += 'Windows ADK acquisition platform must retain exactly three bounded WriteFile calls, one GENERIC_WRITE, one handle disposition call, and three DELETE accesses'
}

$manualUpdateText = Get-Content `
    -LiteralPath $manualUpdateNetworkSurfacePath `
    -Raw
$manualUpdateHeaderPath = Join-Path `
    $sourceRoot `
    'WindowsApp\include\ytec\windowsapp\manual_update.h'
$manualUpdateHeaderText = Get-Content `
    -LiteralPath $manualUpdateHeaderPath `
    -Raw
$manualUpdateMainPath = Join-Path $sourceRoot 'WindowsApp\src\main.cpp'
$manualUpdateMainText = Get-Content -LiteralPath $manualUpdateMainPath -Raw
$manualUpdateRequiredPatterns = [ordered]@{
    'https://ytec\.cloudfree\.jp/ytb/tsumugi-drive/update-v1\.json' = 'fixed Y-TEC HTTPS manifest URL'
    'kMaximumManualUpdateManifestBytes\{16U \* 1024U\}' = '16 KiB response cap'
    '\buser_initiated\b' = 'explicit user-action gate'
    '\bWINHTTP_ACCESS_TYPE_NO_PROXY\b' = 'ambient proxy avoidance'
    '\bWINHTTP_OPTION_REDIRECT_POLICY_NEVER\b' = 'redirect prohibition'
    '\bWINHTTP_DISABLE_AUTHENTICATION\b' = 'HTTP authentication disablement'
    '\bWINHTTP_DISABLE_COOKIES\b' = 'cookie disablement'
    '\bWINHTTP_OPTION_SECURE_PROTOCOLS\b' = 'explicit TLS floor'
    '\bWINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2\b' = 'TLS 1.2 requirement'
    '\bapplication/json\b' = 'fixed JSON content type'
    '\bparse_manual_update_manifest\s*\(' = 'bounded fixed-schema parser'
}
$manualUpdateCombinedText = $manualUpdateHeaderText + "`n" + $manualUpdateText
foreach ($pattern in $manualUpdateRequiredPatterns.Keys) {
    if ($manualUpdateCombinedText -notmatch $pattern) {
        $failures += "Manual update adapter must retain $($manualUpdateRequiredPatterns[$pattern])"
    }
}
$manualUpdateForbiddenPatterns = [ordered]@{
    '\bWINHTTP_ACCESS_TYPE_DEFAULT_PROXY\b' = 'ambient proxy use'
    '\bWINHTTP_OPTION_SECURITY_FLAGS\b' = 'TLS certificate relaxation'
    '\bWinHttpSetCredentials\s*\(' = 'HTTP credential forwarding'
    '\bWinHttpWriteData\s*\(' = 'HTTP upload'
    '\bURLDownloadToFileW?\s*\(' = 'automatic package download'
    '\bCreateProcessW\s*\(' = 'downloaded process execution'
}
foreach ($pattern in $manualUpdateForbiddenPatterns.Keys) {
    if ($manualUpdateText -match $pattern) {
        $failures += "Manual update adapter contains forbidden $($manualUpdateForbiddenPatterns[$pattern])"
    }
}
if ($manualUpdateMainText -notmatch
        'kManualUpdateActionId[\s\S]*start_manual_update_check\s*\(' -or
    $manualUpdateMainText -notmatch
        'check_manual_update_with_windows_apis\s*\(') {
    $failures += 'Manual update product route must remain an explicit diagnostics-page button action'
}
if ($manualUpdateMainText -notmatch
        'GetSaveFileNameW[\s\S]{0,3000}require_windows_tsumugi_destination_outside_portable_data[\s\S]{0,1500}encryption_choice') {
    $failures += 'Windows image creation must enforce the EXE-adjacent data .tsumugi/.partial gate immediately after destination selection and before later prompts or I/O'
}
if ($manualUpdateMainText -notmatch 'TsumugiDrive-failed-' -or
    $manualUpdateMainText -notmatch '\bcomplete_product_log_on_shutdown\s*\(' -or
    $manualUpdateMainText -notmatch '\bintentionally_ram_isolated\s*\(') {
    $failures += 'Windows product logging must remain failed-first, clean-close finalized, and visibly RAM-only when isolated'
}

$windowsAdkEulaExtractorText = Get-Content `
    -LiteralPath $auditedWindowsAdkEulaExtractorPath `
    -Raw
$windowsAdkEulaExtractorRequiredPatterns = [ordered]@{
    '\bCREATE_NEW\b' = 'non-overwriting owned CAB and EULA member creation'
    '\bFILE_FLAG_OPEN_REPARSE_POINT\b' = 'non-following temporary file creation'
    '\blink_count\s*!=\s*1U' = 'hard-link rejection'
    '\bcopy_and_readback_container\s*\(' = 'fixed attached-CAB range copy and readback'
    '\bsha256_handle\s*\(' = 'same-handle bootstrap and EULA SHA-256 verification'
    '\bsource_hash_before\b' = 'whole-bootstrap pre-extraction digest'
    '\bsource_hash_after\b' = 'whole-bootstrap post-extraction digest'
    '\bsame_identity\s*\(' = 'before/after file-object identity binding'
    '\bFDIIsCabinet\s*\(' = 'Cabinet API structural validation'
    '\bFDICopy\s*\(' = 'Cabinet API bounded member enumeration'
    '\bfdintNEXT_CABINET\b' = 'continued-cabinet rejection'
    '\bXmlReaderProperty_DtdProcessing\b' = 'XmlLite DTD policy configuration'
    '\bDtdProcessing_Prohibit\b' = 'DTD expansion prohibition'
    '\bvalidate_windows_adk_burn_eula_mapping\s*\(' = 'exact Burn UX member mapping'
    '\bset_delete_disposition\s*\(' = 'handle-bound owned temporary cleanup'
    '\btemporary_files_removed\b' = 'cleanup evidence in consent receipt'
}
foreach ($pattern in $windowsAdkEulaExtractorRequiredPatterns.Keys) {
    if ($windowsAdkEulaExtractorText -notmatch $pattern) {
        $failures += "Windows ADK EULA extractor must retain $($windowsAdkEulaExtractorRequiredPatterns[$pattern])"
    }
}
$windowsAdkEulaExtractorForbiddenPatterns = [ordered]@{
    '\bCREATE_ALWAYS\b' = 'overwriting file creation'
    '\bOPEN_ALWAYS\b' = 'existing-file reuse'
    '\bTRUNCATE_EXISTING\b' = 'existing-file truncation'
    '\bFILE_SHARE_WRITE\b' = 'write sharing during extraction'
    '\bFILE_SHARE_DELETE\b' = 'delete sharing during extraction'
    '\bDeleteFileW\s*\(' = 'path-only cleanup'
    '\bMoveFileExW\s*\(' = 'path-only move'
    '\bReplaceFileW\s*\(' = 'path-only replacement'
    '\bRemoveDirectoryW\s*\(' = 'directory deletion'
    '\bCopyFile(?:Ex)?W?\s*\(' = 'path-only copy'
    '\bShellExecute(?:Ex)?W?\s*\(' = 'shell execution'
    '\bCreateProcessW\s*\(' = 'process launch'
    '\bWinExec\s*\(' = 'legacy process launch'
}
foreach ($pattern in $windowsAdkEulaExtractorForbiddenPatterns.Keys) {
    if ($windowsAdkEulaExtractorText -match $pattern) {
        $failures += "Windows ADK EULA extractor contains forbidden $($windowsAdkEulaExtractorForbiddenPatterns[$pattern])"
    }
}
$windowsAdkEulaWriteFileCount = ([regex]::Matches(
        $windowsAdkEulaExtractorText,
        '\bWriteFile\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$windowsAdkEulaGenericWriteCount = ([regex]::Matches(
        $windowsAdkEulaExtractorText,
        '\bGENERIC_WRITE\b',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$windowsAdkEulaDispositionCount = ([regex]::Matches(
        $windowsAdkEulaExtractorText,
        '\bSetFileInformationByHandle\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$windowsAdkEulaDeleteAccessCount = ([regex]::Matches(
        $windowsAdkEulaExtractorText,
        '\bDELETE\b',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
if ($windowsAdkEulaWriteFileCount -ne 2 -or
    $windowsAdkEulaGenericWriteCount -ne 1 -or
    $windowsAdkEulaDispositionCount -ne 2 -or
    $windowsAdkEulaDeleteAccessCount -ne 1) {
    $failures += 'Windows ADK EULA extractor must retain exactly two bounded WriteFile calls, one GENERIC_WRITE/DELETE access, and two handle disposition calls'
}

# MED-002..005: the current production manifest is a hard release gate. The
# product route may render its fixed actions, but path selection, factories,
# communication, UAC and installers remain unreachable while either release
# proof is false. The production adapter is separately audited for future use.
$adkManagementHeaderPath = Join-Path `
    $sourceRoot `
    'WindowsApp\include\ytec\windowsapp\adk_management.h'
$adkManagementSourcePath = Join-Path `
    $sourceRoot `
    'WindowsApp\src\adk_management.cpp'
$adkManagementPlatformPath = Join-Path `
    $sourceRoot `
    'WindowsApp\src\windows_adk_management_platform.cpp'
$adkManagementTestPath = Join-Path `
    $repoRoot `
    'tests\Unit\adk_management_tests.cpp'
$adkManagementMainPath = Join-Path `
    $sourceRoot `
    'WindowsApp\src\main.cpp'
$adkAcquisitionCorePath = Join-Path `
    $sourceRoot `
    'WindowsApp\src\adk_acquisition.cpp'
foreach ($requiredPath in @(
        $adkManagementHeaderPath,
        $adkManagementSourcePath,
        $adkManagementPlatformPath,
        $adkManagementTestPath,
        $adkManagementMainPath,
        $adkAcquisitionCorePath)) {
    if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
        $failures += "ADK management product contract is missing $requiredPath"
    }
}
if ((Test-Path -LiteralPath $adkManagementHeaderPath -PathType Leaf) -and
    (Test-Path -LiteralPath $adkManagementSourcePath -PathType Leaf) -and
    (Test-Path -LiteralPath $adkManagementPlatformPath -PathType Leaf) -and
    (Test-Path -LiteralPath $adkManagementTestPath -PathType Leaf) -and
    (Test-Path -LiteralPath $adkManagementMainPath -PathType Leaf) -and
    (Test-Path -LiteralPath $adkAcquisitionCorePath -PathType Leaf)) {
    # These source files are UTF-8 without a required BOM. Windows PowerShell
    # 5.1 otherwise decodes them through the active ANSI code page, while
    # PowerShell 7 assumes UTF-8. Pin the encoding so the Japanese product
    # labels are audited identically in both supported shells.
    $adkManagementHeaderText = Get-Content -LiteralPath $adkManagementHeaderPath -Raw -Encoding UTF8
    $adkManagementSourceText = Get-Content -LiteralPath $adkManagementSourcePath -Raw -Encoding UTF8
    $adkManagementPlatformText = Get-Content -LiteralPath $adkManagementPlatformPath -Raw -Encoding UTF8
    $adkManagementTestText = Get-Content -LiteralPath $adkManagementTestPath -Raw -Encoding UTF8
    $adkManagementMainText = Get-Content -LiteralPath $adkManagementMainPath -Raw -Encoding UTF8
    $adkAcquisitionCoreText = Get-Content -LiteralPath $adkAcquisitionCorePath -Raw -Encoding UTF8
    if ($adkAcquisitionCoreText -notmatch
            '\.unattended_install_no_unexpected_restart_confirmed\s*=\s*false' -or
        $adkAcquisitionCoreText -notmatch
            '\.primary_source_pins_confirmed\s*=\s*false') {
        $failures += 'Current ADK production manifest must retain both no-unexpected-restart and primary-source release gates as false'
    }
    $adkManagementMainRequiredPatterns = [ordered]@{
        '\bshow_adk_management_dialog\s*\(' = 'explicit product action dialog'
        '\bbuild_adk_management_view\s*\(' = 'pure release-gate summary before any action'
        '\bexecute_adk_management_action\s*\(' = 'background core orchestration'
        '\bmake_windows_adk_acquisition_platform\s*\(' = 'production acquisition factory wiring'
        '\bmake_windows_adk_management_platform\s*\(' = 'production management factory wiring'
        '\bkAdkManagementCompleteMessage\b' = 'background completion message'
        '\bGetFocus\s*\(' = 'previous-focus capture'
        '\bSetFocus\s*\(' = 'previous-focus restoration'
    }
    foreach ($pattern in $adkManagementMainRequiredPatterns.Keys) {
        if ($adkManagementMainText -notmatch $pattern) {
            $failures += "WindowsApp ADK management route must retain $($adkManagementMainRequiredPatterns[$pattern])"
        }
    }
    $adkManagementHeaderRequiredPatterns = [ordered]@{
        '\.title\s*=\s*L"ADK取得・管理"' = 'fixed management title'
        'L"固定Microsoft公式URLから取得・導入"' = 'fixed official-download action'
        'L"検証済みオフラインレイアウトから導入"' = 'fixed offline-install action'
        'L"公式オフラインレイアウトを新規作成"' = 'fixed layout-generation action'
        'L"Tsumugiが導入したADKだけを削除"' = 'managed-only uninstall action'
        '\.explicit_start_required\s*=\s*true' = 'explicit start contract'
        '\.background_execution\s*=\s*true' = 'background execution contract'
        '\.escape_cancels_review\s*=\s*true' = 'Esc review cancellation contract'
        '\.previous_focus_restored\s*=\s*true' = 'focus restoration contract'
        '\.official_page_never_opens_automatically\s*=\s*true' = 'manual-only external page contract'
    }
    foreach ($pattern in $adkManagementHeaderRequiredPatterns.Keys) {
        if ($adkManagementHeaderText -notmatch $pattern) {
            $failures += "WindowsApp ADK management UI contract must retain $($adkManagementHeaderRequiredPatterns[$pattern])"
        }
    }
    $adkManagementSourceRequiredPatterns = [ordered]@{
        '\bvalidate_adk_release_manifest\s*\(manifest\)' = 'release manifest as first execution gate'
        '\bload_managed_installation_record\s*\(' = 'EXE-adjacent managed record precheck'
        '\bsafe_registration_id\s*\(' = 'strict MSI/MSP managed identity gate'
        '\bbuild_managed_adk_uninstall_plan\s*\(' = 'exact managed uninstall plan'
        '\bbegin_new_offline_layout\s*\(' = 'new offline layout transaction'
        '\bpublish_offline_layout_payload\s*\(' = 'verified payload publication'
        '\bfinalize_offline_layout\s*\(' = 'complete layout manifest publication'
    }
    foreach ($pattern in $adkManagementSourceRequiredPatterns.Keys) {
        if ($adkManagementSourceText -notmatch $pattern) {
            $failures += "WindowsApp ADK management core must retain $($adkManagementSourceRequiredPatterns[$pattern])"
        }
    }
    $adkManagementTestRequiredPatterns = [ordered]@{
        '\btest_product_gate_stops_every_factory_and_path_use\s*\(' = 'closed production gate matrix'
        '\btest_unreviewed_registration_id_stops_before_every_factory\s*\(' = 'invalid managed identity pre-factory rejection'
        'acquisition_factory_count\s*==\s*0U' = 'zero acquisition-platform construction assertion'
        'management_factory_count\s*==\s*0U' = 'zero management-platform construction assertion'
        'installer_count\s*==\s*0U' = 'zero installer assertion'
        'load_count\s*==\s*0U' = 'zero folder/record read assertion'
        '\bcalculate_adk_management_layout\s*\(width,\s*height\)' = 'bounded layout assertions'
        'std::pair\{960,\s*516\}' = '1024x600 at 200 percent logical layout coverage'
        '\btest_mock_offline_layout_generation_route\s*\(' = 'mock layout generation route'
        '\btest_mock_managed_uninstall_route\s*\(' = 'mock managed uninstall route'
        'L"cmd\.exe /c must-never-run"' = 'production arbitrary-uninstall rejection probe'
    }
    foreach ($pattern in $adkManagementTestRequiredPatterns.Keys) {
        if ($adkManagementTestText -notmatch $pattern) {
            $failures += "WindowsApp ADK management tests must retain $($adkManagementTestRequiredPatterns[$pattern])"
        }
    }
    $adkManagementPlatformRequiredPatterns = [ordered]@{
        '\bmanaged_record_path\s*\(' = 'fixed EXE-adjacent managed record'
        '\bCREATE_NEW\b' = 'non-overwriting record and layout creation'
        '\bFileRenameInfo\b' = 'handle-bound non-overwriting managed-record publication'
        '\bFileIdInfo\b' = 'opened-handle file identity'
        '\bFileStandardInfo\b' = 'length/link count observation'
        '\bFileAttributeTagInfo\b' = 'non-reparse observation'
        '\bhash_handle\s*\(' = 'same-handle SHA-256 verification'
        '\bMsiConfigureProductExW\s*\(' = 'fixed MSI removal API'
        '\bMsiRemovePatchesW\s*\(' = 'fixed MSP removal API'
        'L"REBOOT=ReallySuppress"' = 'no-restart uninstall property'
        '\bmark_delete\s*\(' = 'handle-bound exact cleanup'
    }
    foreach ($pattern in $adkManagementPlatformRequiredPatterns.Keys) {
        if ($adkManagementPlatformText -notmatch $pattern) {
            $failures += "Windows ADK management platform must retain $($adkManagementPlatformRequiredPatterns[$pattern])"
        }
    }
    $adkManagementPlatformForbiddenPatterns = [ordered]@{
        '\bCREATE_ALWAYS\b' = 'overwriting file creation'
        '\bOPEN_ALWAYS\b' = 'existing-file reuse'
        '\bTRUNCATE_EXISTING\b' = 'existing-file truncation'
        '\bFILE_SHARE_DELETE\b' = 'delete sharing during identity lock'
        '\bShellExecute(?:Ex)?W?\s*\(' = 'shell execution'
        '\bCreateProcessW\s*\(' = 'arbitrary process execution'
        '\bWinExec\s*\(' = 'legacy process execution'
        '\bDeleteFileW\s*\(' = 'path-only file deletion'
        '\bRemoveDirectoryW\s*\(' = 'path-only directory deletion'
        '\bremove_all\s*\(' = 'recursive broad cleanup'
    }
    foreach ($pattern in $adkManagementPlatformForbiddenPatterns.Keys) {
        if ($adkManagementPlatformText -match $pattern) {
            $failures += "Windows ADK management platform contains forbidden $($adkManagementPlatformForbiddenPatterns[$pattern])"
        }
    }
    $adkManagementWriteFileCount = ([regex]::Matches(
            $adkManagementPlatformText,
            '\bWriteFile\s*\(',
            [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
    $adkManagementGenericWriteCount = ([regex]::Matches(
            $adkManagementPlatformText,
            '\bGENERIC_WRITE\b',
            [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
    $adkManagementDispositionCount = ([regex]::Matches(
            $adkManagementPlatformText,
            '\bSetFileInformationByHandle\s*\(',
            [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
    $adkManagementDeleteAccessCount = ([regex]::Matches(
            $adkManagementPlatformText,
            '\bDELETE\b',
            [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
    if ($adkManagementWriteFileCount -ne 2 -or
        $adkManagementGenericWriteCount -ne 3 -or
        $adkManagementDispositionCount -ne 2 -or
        $adkManagementDeleteAccessCount -ne 5) {
        $failures += 'Windows ADK management writer tokens must remain fixed at 2 bounded WriteFile calls, 3 GENERIC_WRITE accesses, 2 handle information calls, and 5 DELETE accesses'
    }
    if ($adkManagementSourceText -match
            '\b(?:ShellExecute|WinHttp|CreateProcess)[A-Za-z0-9_]*\s*\(') {
        $failures += 'Windows ADK management core must not directly access shell, network, or process APIs'
    }
}

$productLogRetentionText = Get-Content `
    -LiteralPath $auditedProductLogRetentionPath `
    -Raw
$productLogRetentionRequiredPatterns = [ordered]@{
    '\bclassify_product_log_file_name\s*\(' = 'strict owned-name classification'
    '\bFILE_FLAG_OPEN_REPARSE_POINT\b' = 'non-following candidate and directory opens'
    '\bFileIdInfo\b' = 'stable file identity capture'
    'NumberOfLinks\s*==\s*1U' = 'hard-link rejection'
    '\bReadFile\s*\(' = 'UTF-8 BOM ownership evidence'
    '\bFILE_DISPOSITION_INFO\b' = 'handle-scoped deletion'
    '\bFILE_RENAME_INFO\b' = 'handle-scoped failed-to-normal classification'
    'ReplaceIfExists\s*=\s*FALSE' = 'non-overwriting log classification'
    '\bunchanged\s*\(' = 'identity, size, time, attribute, and BOM recheck'
    '\bplan_product_log_completion\s*\(' = 'failed-first clean completion policy'
    '\binspect_windows_startup_data_backing\s*\(' = 'current EXE-adjacent data re-observation before classification'
    'kProductLogNormalRetentionDays' = '30-day normal retention constant'
    'kProductLogFailureRetentionDays' = '90-day failure retention constant'
    'kProductLogNormalBudgetBytes' = '200 MiB normal retention budget'
}
foreach ($pattern in $productLogRetentionRequiredPatterns.Keys) {
    if ($productLogRetentionText -notmatch $pattern) {
        $failures += "Windows product log retention must retain $($productLogRetentionRequiredPatterns[$pattern])"
    }
}
$productLogRetentionForbiddenPatterns = [ordered]@{
    '\bDeleteFileW\s*\(' = 'path-only deletion'
    '\bMoveFileExW\s*\(' = 'path-only move'
    '\bRemoveDirectoryW\s*\(' = 'directory deletion'
    '\bCREATE_ALWAYS\b' = 'overwriting file creation'
    '\bGENERIC_WRITE\b' = 'file-content write access'
    '\bWriteFile\s*\(' = 'file-content write call'
    '\bPhysicalDrive\b' = 'physical disk path'
}
foreach ($pattern in $productLogRetentionForbiddenPatterns.Keys) {
    if ($productLogRetentionText -match $pattern) {
        $failures += "Windows product log retention contains forbidden $($productLogRetentionForbiddenPatterns[$pattern])"
    }
}
$productLogDispositionCount = ([regex]::Matches(
        $productLogRetentionText,
        '\bSetFileInformationByHandle\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$productLogDeleteAccessCount = ([regex]::Matches(
        $productLogRetentionText,
        '\bDELETE\b',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
if ($productLogDispositionCount -ne 2 -or
    $productLogDeleteAccessCount -ne 2) {
    $failures += 'Windows product log retention must retain exactly two handle disposition calls and two DELETE accesses (retention deletion plus non-overwriting completion classification)'
}

$windowsNtfsShrinkCaptureText = Get-Content `
    -LiteralPath $auditedWindowsNtfsShrinkCapturePath `
    -Raw
$windowsNtfsShrinkCaptureRequiredPatterns = [ordered]@{
    '\bCreateDirectoryW\s*\(' = 'private owned scratch child directory creation'
    'FILE_LIST_DIRECTORY\s*\|\s*FILE_READ_ATTRIBUTES\s*\|\s*DELETE' = 'owned directory handle access'
    'FILE_FLAG_BACKUP_SEMANTICS\s*\|\s*FILE_FLAG_OPEN_REPARSE_POINT' = 'non-following owned directory open'
    '\bFileAttributeTagInfo\b' = 'directory and WIM reparse inspection'
    'FILE_ATTRIBUTE_DIRECTORY\s*\|\s*FILE_ATTRIBUTE_REPARSE_POINT' = 'regular non-reparse WIM requirement'
    '\bstandard\.NumberOfLinks\s*!=\s*1U' = 'single-link WIM requirement'
    'GENERIC_READ\s*\|\s*FILE_READ_ATTRIBUTES\s*\|\s*DELETE' = 'read-only sealed WIM handle access'
    'FILE_SHARE_READ,\s*\r?\n\s*nullptr,\s*\r?\n\s*OPEN_EXISTING' = 'sealed WIM handle without write or delete sharing'
    '\bfound->handle\s*=\s*std::move\(handle\)' = 'held sealed WIM handle'
    '\bfound->sealed\s*=\s*true' = 'sealed WIM state transition'
    '\bFILE_DISPOSITION_INFO\b' = 'handle-identity owned cleanup'
    '\bmicrosoft_signature_verified\b' = 'signed Microsoft DISM evidence'
    '\bsnapshot_device_path\b' = 'snapshot-only WIM capture source'
    '\bquery_windows_volume_bindings_by_offset\s*\(' = 'partition extent to volume identity binding'
    '\bcapture_partition_snapshot_v1\s*\(' = 'canonical source layout binding'
}
foreach ($pattern in $windowsNtfsShrinkCaptureRequiredPatterns.Keys) {
    if ($windowsNtfsShrinkCaptureText -notmatch $pattern) {
        $failures += "Windows NTFS shrink capture must retain $($windowsNtfsShrinkCaptureRequiredPatterns[$pattern])"
    }
}
$windowsNtfsShrinkCaptureForbiddenPatterns = [ordered]@{
    '\bGENERIC_WRITE\b' = 'file-content write access'
    '\bWriteFile\s*\(' = 'file-content write call'
    '\bDeleteFileW\s*\(' = 'path-only file deletion'
    '\bRemoveDirectoryW\s*\(' = 'path-only directory deletion'
    '\bMoveFileExW\s*\(' = 'path-only move'
    '\bCREATE_ALWAYS\b' = 'overwriting file creation'
    '\bTRUNCATE_EXISTING\b' = 'file truncation'
}
foreach ($pattern in $windowsNtfsShrinkCaptureForbiddenPatterns.Keys) {
    if ($windowsNtfsShrinkCaptureText -match $pattern) {
        $failures += "Windows NTFS shrink capture contains forbidden $($windowsNtfsShrinkCaptureForbiddenPatterns[$pattern])"
    }
}
$windowsNtfsShrinkCaptureDispositionCount = ([regex]::Matches(
        $windowsNtfsShrinkCaptureText,
        '\bSetFileInformationByHandle\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$windowsNtfsShrinkCaptureDeleteAccessCount = ([regex]::Matches(
        $windowsNtfsShrinkCaptureText,
        '\bDELETE\b',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
if ($windowsNtfsShrinkCaptureDispositionCount -ne 2 -or
    $windowsNtfsShrinkCaptureDeleteAccessCount -ne 2) {
    $failures += 'Windows NTFS shrink capture must retain exactly two handle disposition calls and two DELETE accesses'
}

$windowsShrinkRestorePlatformText = Get-Content `
    -LiteralPath $auditedWindowsShrinkRestorePlatformPath `
    -Raw
$windowsShrinkRestorePlatformRequiredPatterns = [ordered]@{
    '\bhash_tsumugi_physical_restore_target_layout_v1\s*\(' = 'reviewed target-layout hash recheck'
    '\bopen_verified_physical_target_with_windows_apis\s*\(' = 'stable reidentified offline target writer'
    '\bTsumugiShrinkRestoreLayoutTransactionV1\b' = 'transactional temporary and final metadata writer'
    '\bpublish_construction\s*\(' = 'one temporary construction publication'
    '\bretire_construction\s*\(' = 'temporary construction retirement'
    '\bcommit_final\s*\(' = 'final metadata commit last'
    '\bIOCTL_DISK_UPDATE_PROPERTIES\b' = 'mount-manager layout notification'
    '\bIOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS\b' = 'same physical target and offset volume binding'
    '\bvalidate_exact_binding\s*\(' = 'repeated exact volume extent revalidation'
    '\bIVdsVolumeMF2\b' = 'official Microsoft native volume formatter'
    '\bFormatEx\s*\(' = 'non-shell quick format submission'
    '\bVDS_ASYNCOUT_FORMAT\b' = 'asynchronous format completion evidence'
    '\bverify_microsoft_signed\s*\(' = 'WIMGAPI and DISM dependency trust revalidation'
    '\bexecute_dism_apply\s*\(' = 'fixed WindowsDism apply boundary'
    '\bverify_wim_image_count\s*\(' = 'single-image WIM verification'
    '\bhash_file_handle\s*\(' = 'same-handle WIM hash preservation'
    '\bverify_regular_single_link\s*\(' = 'non-reparse single-link WIM enforcement'
    'GENERIC_READ\s*\|\s*GENERIC_WRITE\s*\|\s*DELETE,\s*\r?\n\s*FILE_SHARE_READ' = 'held owned WIM handle without write or delete sharing'
    '\bCREATE_NEW\b' = 'non-overwriting owned WIM creation'
    '\bFILE_DISPOSITION_INFO\b' = 'handle-bound owned WIM cleanup'
    '\bread_regular_file_to_stable_eof\s*\(' = 'bounded full ordinary-file EOF readback'
    '\bnamespace_fully_enumerated\b' = 'complete target namespace evidence'
    '\bevery_regular_file_read_to_eof\b' = 'all ordinary files EOF evidence'
    '\bextend_ntfs_volume_to_exact_extent_and_verify\s*\(' = 'exact reviewed NTFS extent extension boundary'
    '\bFSCTL_EXTEND_VOLUME\b' = 'audited NTFS filesystem extension control'
    '\bSE_BACKUP_NAME\b' = 'ACL-independent target readback privilege'
    'typed_token\s*!=\s*L"OK"' = 'uppercase OK destructive confirmation'
}
foreach ($pattern in $windowsShrinkRestorePlatformRequiredPatterns.Keys) {
    if ($windowsShrinkRestorePlatformText -notmatch $pattern) {
        $failures += "Windows shrink restore platform must retain $($windowsShrinkRestorePlatformRequiredPatterns[$pattern])"
    }
}
$windowsShrinkRestorePlatformForbiddenPatterns = [ordered]@{
    '\bDeleteFileW\s*\(' = 'path-only WIM cleanup'
    '\bMoveFileExW\s*\(' = 'path-only move'
    '\bReplaceFileW\s*\(' = 'path-only replacement'
    '\bCREATE_ALWAYS\b' = 'overwriting WIM creation'
    '\bOPEN_ALWAYS\b' = 'existing WIM reuse'
    '\bTRUNCATE_EXISTING\b' = 'existing WIM truncation'
    '\bShellExecute(?:Ex)?W?\s*\(' = 'shell execution'
    '\bCreateProcessW\s*\(' = 'unreviewed process launch'
    '\bWinExec\s*\(' = 'legacy process launch'
    '\bIOCTL_VOLUME_ONLINE\b' = 'direct volume online without disk reidentification'
    '\bIOCTL_DISK_SET_[A-Z0-9_]+\b' = 'unreviewed disk layout setter'
    '\bIOCTL_DISK_CREATE_DISK\b' = 'unreviewed disk creation ioctl'
    '\bIOCTL_DISK_DELETE_DRIVE_LAYOUT\b' = 'unreviewed disk layout deletion ioctl'
    '\bdiskpart(?:\.exe)?\b' = 'DiskPart execution'
    '\bpowershell(?:\.exe)?\b' = 'PowerShell execution'
}
foreach ($pattern in $windowsShrinkRestorePlatformForbiddenPatterns.Keys) {
    if ($windowsShrinkRestorePlatformText -match $pattern) {
        $failures += "Windows shrink restore platform contains forbidden $($windowsShrinkRestorePlatformForbiddenPatterns[$pattern])"
    }
}
$windowsShrinkRestoreWriteFileCount = ([regex]::Matches(
        $windowsShrinkRestorePlatformText,
        '\bWriteFile\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$windowsShrinkRestoreGenericWriteCount = ([regex]::Matches(
        $windowsShrinkRestorePlatformText,
        '\bGENERIC_WRITE\b',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$windowsShrinkRestoreSetEndCount = ([regex]::Matches(
        $windowsShrinkRestorePlatformText,
        '\bSetEndOfFile\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$windowsShrinkRestoreDispositionCount = ([regex]::Matches(
        $windowsShrinkRestorePlatformText,
        '\bSetFileInformationByHandle\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$windowsShrinkRestoreDeleteAccessCount = ([regex]::Matches(
        $windowsShrinkRestorePlatformText,
        '\bDELETE\b',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$windowsShrinkRestoreLockCount = ([regex]::Matches(
        $windowsShrinkRestorePlatformText,
        '\bFSCTL_LOCK_VOLUME\b',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$windowsShrinkRestoreDismountCount = ([regex]::Matches(
        $windowsShrinkRestorePlatformText,
        '\bFSCTL_DISMOUNT_VOLUME\b',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$windowsShrinkRestoreVolumeOfflineCount = ([regex]::Matches(
        $windowsShrinkRestorePlatformText,
        '\bIOCTL_VOLUME_OFFLINE\b',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$windowsShrinkRestoreExtendVolumeCount = ([regex]::Matches(
        $windowsShrinkRestorePlatformText,
        '\bFSCTL_EXTEND_VOLUME\b',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
if ($windowsShrinkRestoreWriteFileCount -ne 1 -or
    $windowsShrinkRestoreGenericWriteCount -ne 4 -or
    $windowsShrinkRestoreSetEndCount -ne 1 -or
    $windowsShrinkRestoreDispositionCount -ne 1 -or
    $windowsShrinkRestoreDeleteAccessCount -ne 1 -or
    $windowsShrinkRestoreLockCount -ne 1 -or
    $windowsShrinkRestoreDismountCount -ne 1 -or
    $windowsShrinkRestoreVolumeOfflineCount -ne 1 -or
    $windowsShrinkRestoreExtendVolumeCount -ne 1) {
    $failures += 'Windows shrink restore platform writer tokens must remain fixed at 1 WriteFile, 4 GENERIC_WRITE, 1 SetEndOfFile, 1 handle disposition/DELETE, and one each lock/dismount/extend/offline control'
}

# FS-RECREATE-001: the FAT32/exFAT target adapter is a deliberately narrow
# whole-disk transaction.  A physical target is freshly reidentified and kept
# offline for metadata mutation; only one exact construction Volume GUID may
# be online for handle-relative no-replace population and full opened-handle
# readback.  Final metadata publication is commit-last and never rolls back
# after the publication latch.  Product reachability remains false until an
# outer Windows/WinPE reviewed whole-disk UI route actually supplies the plan.
$windowsFileSystemRecreateControllerPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'MigrationEngine\src\windows_file_system_recreate.cpp'))
$windowsFileSystemRecreateHeaderPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'MigrationEngine\include\ytec\migrationengine\windows_file_system_recreate.h'))
$windowsFileSystemRecreateTestPath = [IO.Path]::GetFullPath(
    (Join-Path $repoRoot 'tests\Unit\windows_file_system_recreate_tests.cpp'))
$migrationEngineCmakePath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'MigrationEngine\CMakeLists.txt'))
$windowsFileSystemRecreateTargetText = Get-Content `
    -LiteralPath $auditedWindowsFileSystemRecreateTargetPath `
    -Raw
$windowsFileSystemRecreateControllerText = Get-Content `
    -LiteralPath $windowsFileSystemRecreateControllerPath `
    -Raw
$windowsFileSystemRecreateHeaderText = Get-Content `
    -LiteralPath $windowsFileSystemRecreateHeaderPath `
    -Raw
$windowsFileSystemRecreateTestText = Get-Content `
    -LiteralPath $windowsFileSystemRecreateTestPath `
    -Raw
$migrationEngineCmakeText = Get-Content `
    -LiteralPath $migrationEngineCmakePath `
    -Raw
$windowsFileSystemRecreateTargetRequiredPatterns = [ordered]@{
    '\breidentify_physical_target\s*\(' = 'fresh stable target reidentification'
    '\bvalidate_tsumugi_physical_restore_target\s*\(' = 'system, removable, read-only, sector, and rescue target class gate'
    '\bhash_tsumugi_physical_restore_target_layout_v1\s*\(' = 'fresh physical layout hash observation'
    '\bopen_verified_physical_target_with_windows_apis\s*\(' = 'verified offline exact physical target writer'
    '\bIOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS\b' = 'exact single-disk construction Volume extent'
    '\bvalidate_exact_binding\s*\(' = 'repeated Volume GUID extent revalidation'
    '\bIVdsVolumeMF2\b' = 'official Microsoft native formatter'
    '\bFormatEx\s*\(' = 'fixed non-shell format submission'
    '\bVDS_ASYNCOUT_FORMAT\b' = 'exact asynchronous format completion evidence'
    '\bempty_label\b' = 'fixed empty target volume label'
    '\bGetVolumeInformationByHandleW\s*\(' = 'opened-root filesystem and label readback'
    '\bGetDiskFreeSpaceW\s*\(' = 'actual allocation geometry readback'
    '\bkFileCreate\b' = 'handle-relative no-replace object creation'
    '\bkFileOpenReparsePoint\b' = 'reparse non-following namespace creation'
    '\bkFileWriteThrough\b' = 'write-through retained child handles'
    '\bFILE_ID_INFO\b' = 'opened-handle file identity'
    '\bFileIdInfo\b' = 'volume serial and file ID query'
    'standard\.NumberOfLinks\s*!=\s*1U' = 'single hard-link gate'
    '\bopen_windows_file_system_recreate_source_session_read_only\s*\(' = 'shared full opened-handle target enumeration'
    '\bFlushFileBuffers\s*\(' = 'retained file and exact Volume flush evidence'
    '\bFSCTL_LOCK_VOLUME\b' = 'exact construction Volume lock'
    '\bFSCTL_DISMOUNT_VOLUME\b' = 'construction Volume dismount'
    '\bIOCTL_VOLUME_OFFLINE\b' = 'construction Volume offline transition'
}
foreach ($pattern in $windowsFileSystemRecreateTargetRequiredPatterns.Keys) {
    if ($windowsFileSystemRecreateTargetText -notmatch $pattern) {
        $failures += "Windows filesystem-recreate target adapter must retain $($windowsFileSystemRecreateTargetRequiredPatterns[$pattern])"
    }
}
$windowsFileSystemRecreateTargetForbiddenPatterns = [ordered]@{
    '\bDeleteFileW\s*\(' = 'path-only deletion'
    '\bRemoveDirectoryW\s*\(' = 'path-only directory deletion'
    '\bMoveFile(?:Ex)?W\s*\(' = 'path-only move'
    '\bReplaceFileW\s*\(' = 'path-only replacement'
    '\bCREATE_ALWAYS\b' = 'overwriting file creation'
    '\bOPEN_ALWAYS\b' = 'existing file reuse'
    '\bTRUNCATE_EXISTING\b' = 'existing file truncation'
    '\bIOCTL_VOLUME_ONLINE\b' = 'direct volume online without physical target reidentification'
    '\bIOCTL_DISK_SET_[A-Z0-9_]+\b' = 'private disk layout setter bypass'
    '\bIOCTL_DISK_CREATE_DISK\b' = 'private disk initialization bypass'
    '\bIOCTL_DISK_DELETE_DRIVE_LAYOUT\b' = 'private layout deletion bypass'
    '\bShellExecute(?:Ex)?W?\s*\(' = 'shell execution'
    '\bCreateProcessW\s*\(' = 'process execution'
    '\bdiskpart(?:\.exe)?\b' = 'DiskPart execution'
    '\bpowershell(?:\.exe)?\b' = 'PowerShell execution'
}
foreach ($pattern in $windowsFileSystemRecreateTargetForbiddenPatterns.Keys) {
    if ($windowsFileSystemRecreateTargetText -match $pattern) {
        $failures += "Windows filesystem-recreate target adapter contains forbidden $($windowsFileSystemRecreateTargetForbiddenPatterns[$pattern])"
    }
}
$windowsFileSystemRecreateControllerRequiredPatterns = [ordered]@{
    'logical_sector_size\s*!=\s*512U' = 'current reviewed 512-byte target gate'
    'expected_source_disk\.logical_sector_size\s*!=\s*4096U' = 'source logical-sector gate limited to 512-byte or 4096-byte media'
    'target_partitions\.size\(\)\s*!=\s*1U' = 'complete one-partition whole-disk restriction'
    '\bTsumugiShrinkRestoreLayoutTransactionV1\b' = 'transactional construction and final metadata writer'
    '\bpublish_construction\s*\(' = 'temporary construction layout publication'
    '\bretire_construction\s*\(' = 'temporary construction layout retirement'
    '\bclose_namespace_dismount_and_offline\s*\(' = 'namespace release before final metadata'
    'offline_proven_before_publication_\s*=\s*true' = 'fresh offline proof before publication'
    'state_\s*=\s*WindowsFileSystemRecreateTargetLifecycleState::[\s\r\n]*publication_attempted' = 'one-way publication state transition'
    '\bpartial_publication\s*\(' = 'typed post-publication failure classification'
    'incomplete_use_prohibited\s*=\s*true' = 'partial target use prohibition'
    'const WindowsFileSystemRecreateExecutionPlan plan_' = 'immutable execution plan binding'
    'const WindowsFileSystemRecreateProductionTargetRequest request_' = 'immutable reviewed whole-disk request binding'
}
foreach ($pattern in $windowsFileSystemRecreateControllerRequiredPatterns.Keys) {
    if ($windowsFileSystemRecreateControllerText -notmatch $pattern) {
        $failures += "Windows filesystem-recreate controller must retain $($windowsFileSystemRecreateControllerRequiredPatterns[$pattern])"
    }
}
$windowsFileSystemRecreateHeaderRequiredPatterns = [ordered]@{
    'kWindowsFileSystemRecreateProductionTargetAdapterConnected\s*=\s*false' = 'honest disconnected product reachability flag'
    '\bconstruction_volume_online_exclusive\b' = 'typed limited online isolation state'
    '\bpartial_publication_use_prohibited\b' = 'typed partial publication terminal state'
    '\bmake_windows_file_system_recreate_target_platform\s*\(' = 'production Win32 factory declaration'
}
foreach ($pattern in $windowsFileSystemRecreateHeaderRequiredPatterns.Keys) {
    if ($windowsFileSystemRecreateHeaderText -notmatch $pattern) {
        $failures += "Windows filesystem-recreate contract must retain $($windowsFileSystemRecreateHeaderRequiredPatterns[$pattern])"
    }
}
$windowsFileSystemRecreateTestRequiredPatterns = [ordered]@{
    '\btest_4kn_source_to_512_target_uses_file_level_recreation\s*\(' = '4Kn-source to 512-target file-only execution coverage'
    '\btest_production_state_machine_commits_only_after_offline_retirement\s*\(' = 'commit-last typed lifecycle coverage'
    '\btest_production_partial_publication_never_rolls_back\s*\(' = 'one-way publication failure coverage'
    '\btest_production_exfat_uses_same_typed_lifecycle\s*\(' = 'exFAT target lifecycle coverage'
    '\btest_partial_publication_does_not_borrow_stale_offline_proof\s*\(' = 'fresh post-failure offline evidence coverage'
    '\btest_production_factory_rejects_unreviewed_layout_before_io\s*\(' = 'zero-I/O invalid reviewed-layout coverage'
    '\btest_source_factory_rejects_nonstandard_logical_sector_before_io\s*\(' = 'zero-I/O non-512/4096 source-sector rejection coverage'
    '\btest_win32_production_factory_constructs_without_target_io\s*\(' = 'real factory allocation-only coverage'
}
foreach ($pattern in $windowsFileSystemRecreateTestRequiredPatterns.Keys) {
    if ($windowsFileSystemRecreateTestText -notmatch $pattern) {
        $failures += "Windows filesystem-recreate tests must retain $($windowsFileSystemRecreateTestRequiredPatterns[$pattern])"
    }
}
foreach ($pattern in @(
        'src/windows_file_system_recreate_target_io\.cpp',
        '\bOle32\b',
        '\bUuid\b')) {
    if ($migrationEngineCmakeText -notmatch $pattern) {
        $failures += "MigrationEngine CMake must retain production filesystem-recreate target dependency $pattern"
    }
}
$windowsFileSystemRecreateWriteFileCount = ([regex]::Matches(
        $windowsFileSystemRecreateTargetText,
        '\bWriteFile\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$windowsFileSystemRecreateGenericWriteCount = ([regex]::Matches(
        $windowsFileSystemRecreateTargetText,
        '\bGENERIC_WRITE\b',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$windowsFileSystemRecreateSetEndCount = ([regex]::Matches(
        $windowsFileSystemRecreateTargetText,
        '\bSetEndOfFile\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$windowsFileSystemRecreateSetInfoCount = ([regex]::Matches(
        $windowsFileSystemRecreateTargetText,
        '\bSetFileInformationByHandle\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$windowsFileSystemRecreateLockCount = ([regex]::Matches(
        $windowsFileSystemRecreateTargetText,
        '\bFSCTL_LOCK_VOLUME\b',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$windowsFileSystemRecreateDismountCount = ([regex]::Matches(
        $windowsFileSystemRecreateTargetText,
        '\bFSCTL_DISMOUNT_VOLUME\b',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$windowsFileSystemRecreateOfflineCount = ([regex]::Matches(
        $windowsFileSystemRecreateTargetText,
        '\bIOCTL_VOLUME_OFFLINE\b',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
if ($windowsFileSystemRecreateWriteFileCount -ne 1 -or
    $windowsFileSystemRecreateGenericWriteCount -ne 2 -or
    $windowsFileSystemRecreateSetEndCount -ne 1 -or
    $windowsFileSystemRecreateSetInfoCount -ne 1 -or
    $windowsFileSystemRecreateLockCount -ne 1 -or
    $windowsFileSystemRecreateDismountCount -ne 1 -or
    $windowsFileSystemRecreateOfflineCount -ne 1) {
    $failures += 'Windows filesystem-recreate target writer tokens must remain fixed at 1 WriteFile, 2 GENERIC_WRITE, 1 SetEndOfFile, 1 SetFileInformationByHandle, and one each lock/dismount/offline control'
}

# Online FAT32/exFAT exact copy is a source-only consistency adapter. It may
# obtain one retained exclusive Volume lock, but it must never dismount,
# offline, or write that source. The exact single extent and filesystem are
# reidentified through the retained read handle before target mutation.
$windowsOnlineDirectCloneSourceText = Get-Content `
    -LiteralPath $auditedWindowsOnlineDirectCloneSourcePath `
    -Raw
$windowsOnlineDirectCloneTestPath = [IO.Path]::GetFullPath(
    (Join-Path $repoRoot 'tests\Unit\windows_online_direct_clone_tests.cpp'))
$windowsOnlineDirectCloneTestText = Get-Content `
    -LiteralPath $windowsOnlineDirectCloneTestPath `
    -Raw
$windowsOnlineDirectCloneLockRequiredPatterns = [ordered]@{
    '\bIOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS\b' = 'single physical extent query'
    'NumberOfDiskExtents\s*!=\s*1U' = 'multi-extent fail-closed gate'
    '\bGENERIC_READ\b' = 'read-only source handle access'
    '\bGetVolumeInformationByHandleW\s*\(' = 'locked-handle filesystem reidentification'
    '\bclassify_basic_data_file_system\s*\(' = 'locked boot-sector signature reidentification'
    'declared_volume_bytes\s*!=\s*request\.length' = 'complete locked partition coverage'
    'build_online_direct_source_layout\s*\(\s*final_observation\.value\(\)\.source,\s*\*composite\.value\(\)' = 'post-lock layout recheck through the retained consistency routes'
    '\bsource_consistency_verified\s*=\s*true' = 'typed consistency completion evidence'
}
foreach ($pattern in $windowsOnlineDirectCloneLockRequiredPatterns.Keys) {
    if ($windowsOnlineDirectCloneSourceText -notmatch $pattern) {
        $failures += "Windows online FAT/exFAT source adapter must retain $($windowsOnlineDirectCloneLockRequiredPatterns[$pattern])"
    }
}
$windowsOnlineDirectCloneLockForbiddenPatterns = [ordered]@{
    '\bGENERIC_WRITE\b' = 'source write access'
    '\bWriteFile\s*\(' = 'source write call'
    '\bFSCTL_DISMOUNT_VOLUME\b' = 'source volume dismount'
    '\bIOCTL_VOLUME_OFFLINE\b' = 'source volume offline transition'
    '\bIOCTL_VOLUME_ONLINE\b' = 'source volume online transition'
}
foreach ($pattern in $windowsOnlineDirectCloneLockForbiddenPatterns.Keys) {
    if ($windowsOnlineDirectCloneSourceText -match $pattern) {
        $failures += "Windows online FAT/exFAT source adapter contains forbidden $($windowsOnlineDirectCloneLockForbiddenPatterns[$pattern])"
    }
}
$windowsOnlineDirectCloneLockCount = ([regex]::Matches(
        $windowsOnlineDirectCloneSourceText,
        '\bFSCTL_LOCK_VOLUME\b',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$windowsOnlineDirectCloneExtentQueryCount = ([regex]::Matches(
        $windowsOnlineDirectCloneSourceText,
        '\bquery_exact_volume_extent\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
if ($windowsOnlineDirectCloneLockCount -ne 1 -or
    $windowsOnlineDirectCloneExtentQueryCount -ne 3) {
    $failures += "Windows online FAT/exFAT source adapter must retain exactly 1 lock and 2 calls plus 1 definition of exact-extent query (found lock=$windowsOnlineDirectCloneLockCount extent=$windowsOnlineDirectCloneExtentQueryCount)"
}
$windowsOnlineDirectCloneLockTestRequiredPatterns = [ordered]@{
    '\btest_exfat_data_disk_clones_without_vss_under_retained_lock\s*\(' = 'retained-lock success and lifetime coverage'
    '\btest_fat32_data_disk_clones_without_vss_under_retained_lock\s*\(' = 'FAT32 retained-lock success and lifetime coverage'
    '\btest_exfat_lock_failure_stops_before_target_mutation\s*\(' = 'lock failure zero-target-mutation coverage'
    '\btest_online_fat32_and_exfat_require_exact_locked_volume_extent\s*\(' = 'partition-slack fail-closed coverage'
    'MemoryReader retained Volume lock' = 'test double rejects physical partition reads after lock acquisition'
    'locked\s*<\s*offline' = 'lock-before-target-offline ordering assertion'
}
foreach ($pattern in $windowsOnlineDirectCloneLockTestRequiredPatterns.Keys) {
    if ($windowsOnlineDirectCloneTestText -notmatch $pattern) {
        $failures += "Windows online FAT/exFAT source tests must retain $($windowsOnlineDirectCloneLockTestRequiredPatterns[$pattern])"
    }
}

$windowsDirectShrinkClonePlatformText = Get-Content `
    -LiteralPath $auditedWindowsDirectShrinkClonePlatformPath `
    -Raw
$windowsOnlineDirectShrinkCloneText = Get-Content `
    -LiteralPath $windowsOnlineDirectShrinkClonePath `
    -Raw
$windowsDirectShrinkClonePlatformTestText = Get-Content `
    -LiteralPath $windowsDirectShrinkClonePlatformTestPath `
    -Raw
$windowsOnlineDirectShrinkCloneTestText = Get-Content `
    -LiteralPath $windowsOnlineDirectShrinkCloneTestPath `
    -Raw
$windowsDirectShrinkProductUiText = Get-Content `
    -LiteralPath $windowsDirectShrinkProductUiPath `
    -Raw
$windowsDirectShrinkClonePlatformRequiredPatterns = [ordered]@{
    '\bmake_windows_tsumugi_shrink_restore_platform_io\s*\(' = 'shared reidentified target writer, VDS format, DISM apply, and filesystem readback boundary'
    '\bopen_offline_target\s*\(' = 'verified offline target writer acquisition'
    '\bvalidate_stable_identity\s*\(' = 'locked-target stable identity recheck'
    '\binvalidate_initial_partition_metadata\s*\(' = 'pre-checkpoint broad target metadata withholding'
    '\binvalidate_exact_gpt_metadata\s*\(' = 'post-checkpoint exact GPT-only invalidation'
    '\bpublish_gpt_plan\s*\(' = 'ordered GPT metadata publication'
    '\bverify_gpt_plan\s*\(' = 'complete GPT readback verification'
    '\bexecute_dism_capture\s*\(' = 'signed System32 DISM snapshot capture'
    '\bexecute_dism_apply\s*\(' = 'signed System32 DISM target apply'
    '\bhash_file_handle\s*\(' = 'same-handle complete WIM hash'
    'GENERIC_READ\s*\|\s*GENERIC_WRITE\s*\|\s*DELETE,\s*\r?\n\s*FILE_SHARE_READ' = 'owned WIM seal without write or delete sharing'
    'reopen_current\(\s*source_table_index,\s*expected_hash,\s*false\)' = 'read-only identity lock compatible with DISM apply'
    'request_delete_access\s*\?\s*DELETE\s*:\s*0U' = 'DELETE access limited to exact owned-WIM cleanup'
    'FILE_READ_ATTRIBUTES\s*\|\s*DELETE,\s*\r?\n\s*FILE_SHARE_READ' = 'failed-capture partial WIM handle-bound cleanup access'
    '\bdiscard_partial_wim_handle_bound\s*\(' = 'failed or capacity-exhausted DISM capture cleanup'
    '\bFILE_DISPOSITION_INFO\b' = 'handle-bound exact WIM deletion'
    '\bCreateDirectoryW\s*\(' = 'non-overwriting target-owned directory creation'
    'ERROR_FILE_NOT_FOUND' = 'WIM nonexistence proof before DISM capture'
    '\bcheckpoint_record\s*\(' = 'fixed target-owned durable checkpoint record'
    'YTEC-WINDOWS-DIRECT-SHRINK-STAGING-V2' = 'checkpoint offset and record size bound into staging identity'
    'YTEC-DSC-CHK-V2' = 'current fixed-gap checkpoint serialization domain'
    '\bprepare_final_extents_keep_incomplete_and_verify\s*\(' = 'nonboot hidden-final extent preparation before boot and final publication'
    '\brevalidate_before_final_commit\s*\(' = 'post-VSS-cleanup identity, staging, and checkpoint revalidation'
    '\bbuild_hidden_final_gpt\s*\(' = 'non-visible final extents before filesystem extension'
    '\bbuild_final_mbr\s*\(' = 'reviewed primary-only MBR extents and fresh target signature'
    '\bbuild_hidden_final_mbr_sector\s*\(' = 'inactive zero-bootstrap MBR construction publication'
    '\bverify_native_construction_mbr\s*\(' = 'native exact inactive MBR extent and type revalidation'
    '\brevalidate_mbr_source_and_signature\s*\(' = 'fresh raw source sector and all-connected signature collision recheck'
    'BcdBootFirmware::bios' = 'explicit signed BCDBoot legacy BIOS route'
    '\bgpt_type_windows_recovery\s*\(' = 'Windows recovery GPT role preservation'
    '\bkRecoveryGptAttributes\b' = 'recovery required and no-drive-letter attributes'
    '\bexecute_winre_registration_transaction\s*\(' = 'shared reviewed WinRE registration transaction'
    'WinReRegistrationPriorStateOrigin::cloned_source_stale' = 'cloned source registration classification'
    'allow_mismatched_registered_location_as_cloned_source_stale\s*=\s*true' = 'foreign registered path non-opening diagnostic mode'
    'WinReRegisteredPathKind::recovery_windows_re' = 'expected Recovery WindowsRE registration path kind'
    '\bTemporarySystemVolumeMount::acquire\s*\(' = 'exact Windows and Recovery Volume GUID temporary mounts'
    '\bfinalize_winre_with_windows_apis\b' = 'production WinRE transaction adapter'
    '\.expected_partition_style\s*=\s*plan_\.partition_style\(\)' = 'WinRE revalidation remains bound to final GPT or MBR style'
    '\.expected_mbr_disk_signature\s*=\s*legacy_bios' = 'WinRE MBR revalidation remains bound to the fresh target signature'
    '\brevalidate_direct_construction_boot_target\s*\(' = 'native construction GPT and exact boot-volume revalidation'
    '\befi_boot_ownership_allows_microsoft_rebuild\s*\(' = 'safe-only direct construction EFI ownership policy'
    '\bexecute_bcdboot_with_windows_apis\s*\(' = 'signed explicit-system-root construction BCDBoot boundary'
    '\bextend_ntfs_volume_to_exact_extent_and_verify\s*\(' = 'shared exact NTFS extension boundary'
    '\bvalidate_mbr_to_gpt_source_bindings\s*\(' = 'target-only MBR source style, snapshot, and complete mapping validation'
    '\bvalidate_mbr_preserve_source_bindings\s*\(' = 'MBR preserve primary type, mapping, Active, and role validation'
    '\bsupported_initial_target_partition_table\s*\(' = 'raw, GPT, or basic-primary-only MBR target pre-I/O validation'
    '\bsource_partition_snapshot_hash\s*\(\s*\)' = 'immutable raw MBR partition snapshot binding'
    '\bsource_reidentified\s*=\s*true' = 'post-VSS source stable-identity evidence'
    '\bsource_layout_unchanged\s*=\s*true' = 'post-VSS source layout readback evidence'
    '\bhidden_final_layout_published_and_read_back\s*=\s*true' = 'hidden final GPT readback evidence'
    '\bprimary_layout_committed_last\s*=\s*true' = 'final primary GPT commit-last evidence'
    '\bfinal_mbr_sector0_read_back_verified\s*=\s*is_mbr_mode\(\)' = 'final MBR sector0 readback evidence'
    '\bfinal_mbr_active_partition_count\b' = 'exact final MBR Active count evidence'
    '\bcheckpoint_retirement_pending\s*=\s*!checkpoint_retired' = 'non-destructive cleanup-pending completion evidence'
    'ShrinkSurplusAllocation::selected_data_partition' = 'reviewed selected NTFS surplus production route'
    '\btargeted_surplus_source_table_index_\b' = 'exact reviewed source-table growth owner evidence'
    '\btargeted_surplus_exact_size_verified_\b' = 'targeted NTFS exact size evidence'
    '\btargeted_surplus_readback_verified_\b' = 'targeted NTFS namespace readback evidence'
    'typed_token\s*!=\s*L"OK"' = 'uppercase OK destructive confirmation'
}
foreach ($pattern in $windowsDirectShrinkClonePlatformRequiredPatterns.Keys) {
    if ($windowsDirectShrinkClonePlatformText -notmatch $pattern) {
        $failures += "Windows direct shrink clone platform must retain $($windowsDirectShrinkClonePlatformRequiredPatterns[$pattern])"
    }
}
$windowsOnlineDirectShrinkCloneRequiredPatterns = [ordered]@{
    '\bplan_windows_direct_shrink_clone_with_windows_apis\s*\(' = 'read-only product planning entry point'
    '\binspect_windows_direct_shrink_partition_capacity_with_windows_apis\s*\(' = 'read-only partition review inspection entry point'
    '\bplan_windows_direct_shrink_clone_after_partition_review_with_windows_apis\s*\(' = 'freshly reanalyzed reviewed planning entry point'
    '\bhash_windows_direct_shrink_source_analysis_v1\s*\(' = 'complete canonical source analysis digest'
    '\brequire_exact_partition_capacity_binding\s*\(' = 'stable identity plus exact layout and analysis revalidation'
    '\bhas_valid_targeted_surplus_evidence\s*\(' = 'selected NTFS owner, size, and readback final evidence gate'
    '\bopen_verified_read_only_physical_disk_with_windows_apis\s*\(' = 'read-only stable source handle acquisition'
    '\banalyze_gpt_shrink_source_with_windows_apis\s*\(' = 'reviewed GPT NTFS and role analysis'
    '\banalyze_mbr_shrink_source_with_windows_apis\s*\(' = 'reviewed read-only MBR NTFS and role analysis'
    '\bbuild_product_plan_with_fresh_mbr_binding\s*\(' = 'fresh read-only MBR signature selection before immutable planning'
    '\bobserve_windows_direct_shrink_mbr_safety_with_windows_apis\s*\(' = 'raw source MBR and all-connected signature observation'
    '\bmake_windows_mbr_signature_generator\s*\(' = 'CNG fresh nonzero target MBR signature generation'
    'DirectClonePartitionStyleChoice::mbr_to_gpt' = 'explicit target-only MBR-to-GPT style choice'
    '\bsource_partition_snapshot_hash_\b' = 'immutable raw source partition-table digest'
    '\bsource_partition_mappings_\b' = 'complete source-to-target partition disposition mapping'
    '\bsource_reidentified\b' = 'post-VSS source reidentification proof'
    '\bsource_layout_unchanged\b' = 'post-VSS source layout immutability proof'
    '\bvalidate_checkpoint_exactly_unchanged\s*\(' = 'complete checkpoint evidence equality after extent preparation and final revalidation'
    '\bprecomputed_retired_completion_hash\b' = 'retired canonical completion hash computed before final publication'
    '\bprecomputed_pending_completion_hash\b' = 'cleanup-pending canonical completion hash computed before final publication'
    '\bselected_completion_hash\b' = 'allocation-free post-publication completion digest selection'
    '\bhas_valid_windows_direct_shrink_precomputed_completion_evidence\s*\(' = 'fixed-size selected outcome evidence validation'
    '\bbuild_windows_direct_shrink_clone_plan_from_analysis\s*\(' = 'pure analyzed product-plan conversion seam'
    '\bmake_windows_direct_shrink_clone_dependencies\s*\(' = 'product dependency factory'
    'reidentify_physical_clone_selection\s*\([\s\S]{0,160}?false\s*\)' = 'smaller-target selection reidentification'
    'reidentify_physical_clone\s*\([\s\S]{0,180}?false\s*\)' = 'smaller-target confirmed reidentification'
    '\bWindowsVssBackend\b' = 'production VSS snapshot backend'
    '\bexecute_backup_workflow\s*\(' = 'production VSS workflow'
    '\bmake_windows_direct_shrink_clone_platform\s*\(' = 'audited production platform factory'
    '\bexecute_windows_direct_shrink_clone_with_windows_apis\s*\(' = 'product convenience entry point'
}
foreach ($pattern in $windowsOnlineDirectShrinkCloneRequiredPatterns.Keys) {
    if ($windowsOnlineDirectShrinkCloneText -notmatch $pattern) {
        $failures += "Windows online direct shrink clone must retain $($windowsOnlineDirectShrinkCloneRequiredPatterns[$pattern])"
    }
}
$windowsOnlineDirectShrinkCloneForbiddenPatterns = [ordered]@{
    '\bWriteFile\s*\(' = 'direct raw or file write'
    '\bGENERIC_WRITE\b' = 'direct write handle access'
    '\bDeleteFileW\s*\(' = 'path-only deletion'
    '\bMoveFileExW\s*\(' = 'path-only publication'
    '\bReplaceFileW\s*\(' = 'path-only replacement'
    '\bSetFileInformationByHandle\s*\(' = 'private mutation bypass'
    '\bDeviceIoControl\s*\(' = 'private disk or volume control bypass'
    '\bCreateProcessW\s*\(' = 'unreviewed process execution'
    '\bmbr2gpt(?:\.exe)?\b' = 'Microsoft full-OS MBR2GPT execution or coupling'
    '/allowFullOS' = 'source-mutating full-OS MBR2GPT mode'
    '\bMountedDevices\b' = 'host registry mutation coupling'
}
foreach ($pattern in $windowsOnlineDirectShrinkCloneForbiddenPatterns.Keys) {
    if ($windowsOnlineDirectShrinkCloneText -match $pattern) {
        $failures += "Windows online direct shrink clone contains forbidden $($windowsOnlineDirectShrinkCloneForbiddenPatterns[$pattern])"
    }
}
$windowsOnlineDirectShrinkCloneTestRequiredPatterns = [ordered]@{
    '\btest_product_analysis_builds_representative_gpt_windows_plan\s*\(' = 'representative Windows 10/11 GPT product-plan coverage'
    '\btest_product_analysis_builds_target_only_mbr_to_gpt_plan\s*\(' = 'representative target-only MBR-to-GPT product-plan coverage'
    '\btest_product_analysis_builds_mbr_preserve_with_raw_binding\s*\(' = 'representative MBR preserve immutable raw-sector and signature binding coverage'
    '\btest_mbr_active_windows_maps_without_separate_bios_payload\s*\(' = 'active-Windows MBR mapping coverage'
    '\btest_mbr_to_gpt_rejects_unknown_type_snapshot_and_unsafe_selection\s*\(' = 'MBR unknown type, unbound snapshot, and unsafe selection rejection coverage'
    '\btest_source_layout_drift_after_snapshot_cleanup_blocks_final_commit\s*\(' = 'post-VSS source layout drift containment coverage'
    '\btest_product_analysis_fails_closed_before_execution\s*\(' = 'product pre-I/O layout, capacity, and MBR failure coverage'
    '\btest_target_owned_archive_capacity_failure_cleans_up_without_commit\s*\(' = 'target-owned WIM capacity failure cleanup coverage'
    '\btest_source_analysis_hash_binds_complete_review_payload\s*\(' = 'complete review analysis digest mutation coverage'
    'MBR preserve without a fresh raw/source/signature binding must be rejected by the pure planner before VSS or target I/O' = 'missing MBR raw/signature binding pre-I/O rejection'
    '\btest_mbr_signature_rechecks_fail_closed_before_publication\s*\(' = 'planned signature collision and post-VSS collision containment coverage'
    'zero used-byte counters are advisory and must not discard an analyzed NTFS archive' = 'advisory used-byte counter never drops NTFS content coverage'
    'a zero selected completion hash must fail the allocation-free post-publication check' = 'zero precomputed completion hash rejection coverage'
    'a selected completion hash from the opposite retirement outcome must fail closed' = 'opposite checkpoint outcome hash rejection coverage'
}
foreach ($pattern in $windowsOnlineDirectShrinkCloneTestRequiredPatterns.Keys) {
    if ($windowsOnlineDirectShrinkCloneTestText -notmatch $pattern) {
        $failures += "Windows online direct shrink clone tests must retain $($windowsOnlineDirectShrinkCloneTestRequiredPatterns[$pattern])"
    }
}
$windowsDirectShrinkProductUiRequiredPatterns = [ordered]@{
    '#include\s+"ytec/windowsapp/online_direct_shrink_clone\.h"' = 'direct shrink product header wiring'
    '\bstart_windows_direct_shrink_clone_flow\s*\(' = 'dedicated shrink-mode product route'
    '\bplan_windows_direct_shrink_clone_after_partition_review_with_windows_apis\s*\(' = 'fresh reanalysis immediately before immutable planning'
    '\breview_clone_partition_capacity_settings\s*\(' = 'clone-only partition and capacity review route'
    '\bshow_clone_partition_capacity_review_dialog\s*\(' = 'scrollable modal review UI'
    '\bcomplete_windows_clone_partition_capacity_review\s*\(' = 'pure exact-binding choice completion'
    'パーティション・容量設定…' = 'dedicated clone settings action'
    '\bWC_LISTVIEWW\b' = 'native scrollable partition ListView'
    '\bLVS_EX_CHECKBOXES\b' = 'default-visible partition selection checkboxes'
    '\bLVN_ITEMCHANGING\b' = 'mouse required-row uncheck veto'
    '\bLVN_KEYDOWN\b' = 'keyboard required-row gate'
    '\bVK_SPACE\b' = 'Space required-row uncheck gate'
    '\bLVIF_PARAM\b' = 'source-table identity recovered from ListView lParam'
    '\.lParam\s*=\s*static_cast<LPARAM>\([\s\S]{0,80}?source_table_index' = 'ListView lParam stores source_table_index'
    '\bCB_SETITEMDATA\b' = 'surplus policy and NTFS target item-data binding'
    '\bDialogBoxIndirectParamW\b' = 'modal keyboard navigation and owner interlock'
    '\bSetFocus\(state->partition_list\)' = 'predictable initial keyboard focus'
    '\bpartition_capacity_visible\b' = 'clone shrink-only settings visibility interlock'
    'ShrinkSurplusAllocation::automatic_proportional' = 'automatic recommended surplus option'
    'ShrinkSurplusAllocation::selected_data_partition' = 'selected NTFS surplus option'
    'ShrinkSurplusAllocation::leave_unallocated' = 'leave-unallocated surplus option'
    '一時WIMがコピー先の専用領域に収まらない場合は安全に中止' = 'pre-execution bounded-staging disclosure'
    'IDD_CLONE_CONFIRMATION' = 'shared uppercase OK confirmation dialog'
    '\bexecute_windows_direct_shrink_clone_with_windows_apis\s*\(' = 'audited product executor wiring'
    '\bactive_clone_mode\b' = 'explicit running transfer-mode UI state retention'
    'WindowsPartitionStyleChoice::mbr_to_gpt' = 'clone-only MBR-to-GPT style selection'
    '\.partition_style_choice\s*=' = 'reviewed style choice passed into the immutable product plan'
    'MBR2GPT\.exeと/allowFullOSは使用せず' = 'source-no-write target-only reconstruction disclosure'
    'MBR形式とLegacy BIOS構成を維持' = 'MBR preserve and Legacy BIOS first confirmation disclosure'
    'BCDBoot /f BIOS' = 'exact target-only BIOS boot reconstruction second confirmation disclosure'
    'Active付きsector0は最後まで公開せず' = 'progress keeps bootable MBR withheld until final publication'
    '\bWindowsDirectShrinkCloneExecutionReport\b' = 'dedicated completion evidence inspection'
    'source_reidentified' = 'completion requires final source reidentification'
    'source_layout_unchanged' = 'completion requires final source layout immutability'
    '\btake_completion_power_operation_binding\s*\(' = 'operation-bound SAFE-007 completion proof'
    '\bmake_direct_shrink_clone_completion_power_proof\s*\(' = 'direct shrink mandatory completion proof'
    'mbr_to_gpt\s*\?\s*L"MBR→GPT縮小移行クローン"[\s\S]{0,260}?mbr_preserve[\s\S]{0,260}?L"縮小移行クローン"' = 'verified style-specific direct shrink completion action prompt'
    '実機での起動成功を確認した表示ではありません' = 'real-boot evidence boundary disclosure'
}
foreach ($pattern in $windowsDirectShrinkProductUiRequiredPatterns.Keys) {
    if ($windowsDirectShrinkProductUiText -notmatch $pattern) {
        $failures += "Windows direct shrink product UI must retain $($windowsDirectShrinkProductUiRequiredPatterns[$pattern])"
    }
}
$windowsDirectShrinkProductUiForbiddenPatterns = [ordered]@{
    '縮小移行（直接クローン準備中）' = 'obsolete disabled transfer-mode label'
    '2台のディスクだけを使う縮小移行の直接クローンは開始できません' = 'obsolete product route stop'
}
foreach ($pattern in $windowsDirectShrinkProductUiForbiddenPatterns.Keys) {
    if ($windowsDirectShrinkProductUiText -match $pattern) {
        $failures += "Windows direct shrink product UI contains forbidden $($windowsDirectShrinkProductUiForbiddenPatterns[$pattern])"
    }
}
$windowsDirectShrinkUiPlannerCount = ([regex]::Matches(
        $windowsDirectShrinkProductUiText,
        '\bplan_windows_direct_shrink_clone_after_partition_review_with_windows_apis\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$windowsDirectShrinkUiInspectionCount = ([regex]::Matches(
        $windowsDirectShrinkProductUiText,
        '\binspect_windows_direct_shrink_partition_capacity_with_windows_apis\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$windowsDirectShrinkUiExecutorCount = ([regex]::Matches(
        $windowsDirectShrinkProductUiText,
        '\bexecute_windows_direct_shrink_clone_with_windows_apis\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$windowsDirectShrinkUiCompletionProofCount = ([regex]::Matches(
        $windowsDirectShrinkProductUiText,
        '\bmake_direct_shrink_clone_completion_power_proof\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
if ($windowsDirectShrinkUiPlannerCount -ne 1 -or
    $windowsDirectShrinkUiInspectionCount -ne 2 -or
    $windowsDirectShrinkUiExecutorCount -ne 1 -or
    $windowsDirectShrinkUiCompletionProofCount -ne 1) {
    $failures += 'Windows direct shrink product UI must retain exactly two review inspections, one fresh reviewed planner, audited executor, and mandatory completion proof call'
}
$windowsDirectShrinkClonePlatformTestRequiredPatterns = [ordered]@{
    '\btest_gpt_system_leave_unallocated_finalizes_boot_and_winre\s*\(' = 'representative GPT system leave-unallocated coverage'
    '\btest_gpt_system_automatic_extends_every_planned_ntfs_before_visibility\s*\(' = 'representative GPT system automatic-extension coverage'
    '\btest_selected_data_extension_verifies_exact_owner_size_and_readback\s*\(' = 'selected NTFS owner, exact-size, and readback coverage'
    '\btest_data_only_gpt_preserves_one_selected_microsoft_reserved_partition\s*\(' = 'data-only GPT selected MSR production coverage'
    '\btest_system_boot_or_winre_failure_aborts_before_commit_ready\s*\(' = 'BCDBoot and WinRE failure injection coverage'
    '\btest_automatic_extension_failure_invalidates_and_keeps_offline\s*\(' = 'NTFS extension failure injection coverage'
    '\btest_factory_accepts_explicit_mbr_to_gpt_and_four_primary_layout\s*\(' = 'four-primary target-only MBR-to-GPT factory coverage'
    '\btest_factory_accepts_reviewed_mbr_preserve_before_any_io\s*\(' = 'reviewed MBR preserve pure factory coverage'
    '\btest_mbr_data_only_uses_hidden_then_sector0_last\s*\(' = 'data-only hidden MBR to inactive sector0-last coverage'
    '\btest_mbr_system_uses_exact_legacy_bios_target_and_one_active\s*\(' = 'exact BIOS BCDBoot and exactly-one-Active final MBR coverage'
    '\btest_mbr_publication_failures_preserve_latch_contract\s*\(' = 'pre-latch failure and post-latch cleanup-pending MBR coverage'
    '\btest_mbr_to_gpt_active_windows_source_completes_without_mbr2gpt\s*\(' = 'active-Windows target-only reconstruction coverage'
    '\btest_mbr_to_gpt_source_identity_or_layout_drift_withholds_final_gpt\s*\(' = 'source identity and layout drift containment coverage'
    '\btest_mbr_to_gpt_format_boot_winre_and_commit_failures_stay_incomplete\s*\(' = 'target-only format, boot, WinRE, and final-commit failure coverage'
    '\btest_gpt_phases_reuse_fresh_guids_and_withhold_boot_until_final\s*\(' = 'stable GUID and nonboot construction-to-final publication coverage'
    '\btest_abort_after_hidden_final_preserves_checkpoint_and_invalidates_only_gpt\s*\(' = 'hidden-final abort preserves fixed checkpoint coverage'
    '\btest_data_only_prepare_latches_synthetic_boot_completion\s*\(' = 'data-only commit-order state coverage'
    '\btest_prepare_rejects_nonserialized_checkpoint_evidence_drift\s*\(' = 'complete checkpoint evidence drift rejection coverage'
    '\btest_final_publication_failure_keeps_checkpoint_and_nonboot_target\s*\(' = 'failed final publication remains checkpointed and nonboot coverage'
    '\btest_checkpoint_retirement_failure_reports_cleanup_pending_and_keeps_final\s*\(' = 'post-publication checkpoint cleanup-pending coverage'
    '\btest_boot_finalization_rejects_missing_nonboot_or_ownership_proof\s*\(' = 'construction GPT and EFI ownership proof rejection coverage'
}
foreach ($pattern in $windowsDirectShrinkClonePlatformTestRequiredPatterns.Keys) {
    if ($windowsDirectShrinkClonePlatformTestText -notmatch $pattern) {
        $failures += "Windows direct shrink clone platform tests must retain $($windowsDirectShrinkClonePlatformTestRequiredPatterns[$pattern])"
    }
}
$windowsDirectShrinkClonePlatformForbiddenPatterns = [ordered]@{
    '\bWriteFile\s*\(' = 'private direct file or disk write bypass'
    '\bDeleteFileW\s*\(' = 'path-only WIM cleanup'
    '\bMoveFileExW\s*\(' = 'path-only move'
    '\bReplaceFileW\s*\(' = 'path-only replacement'
    '\bCREATE_ALWAYS\b' = 'overwriting WIM creation'
    '\bOPEN_ALWAYS\b' = 'existing WIM reuse'
    '\bTRUNCATE_EXISTING\b' = 'existing WIM truncation'
    '\bShellExecute(?:Ex)?W?\s*\(' = 'shell execution'
    '\bCreateProcessW\s*\(' = 'unreviewed process launch'
    '\bWinExec\s*\(' = 'legacy process launch'
    '\bmbr2gpt(?:\.exe)?\b' = 'Microsoft full-OS MBR2GPT execution or coupling'
    '/allowFullOS' = 'source-mutating full-OS MBR2GPT mode'
    '\bMountedDevices\b' = 'host registry mutation coupling'
    '\bIOCTL_DISK_SET_[A-Z0-9_]+\b' = 'private disk layout setter'
    '\bIOCTL_DISK_CREATE_DISK\b' = 'private disk creation ioctl'
    '\bIOCTL_DISK_DELETE_DRIVE_LAYOUT\b' = 'private disk layout deletion ioctl'
    '\bdiskpart(?:\.exe)?\b' = 'DiskPart execution'
    '\bpowershell(?:\.exe)?\b' = 'PowerShell execution'
}
foreach ($pattern in $windowsDirectShrinkClonePlatformForbiddenPatterns.Keys) {
    if ($windowsDirectShrinkClonePlatformText -match $pattern) {
        $failures += "Windows direct shrink clone platform contains forbidden $($windowsDirectShrinkClonePlatformForbiddenPatterns[$pattern])"
    }
}
$windowsDirectShrinkGenericWriteCount = ([regex]::Matches(
        $windowsDirectShrinkClonePlatformText,
        '\bGENERIC_WRITE\b',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$windowsDirectShrinkDispositionCount = ([regex]::Matches(
        $windowsDirectShrinkClonePlatformText,
        '\bSetFileInformationByHandle\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$windowsDirectShrinkDeleteAccessCount = ([regex]::Matches(
        $windowsDirectShrinkClonePlatformText,
        '\bDELETE\b',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
if ($windowsDirectShrinkGenericWriteCount -ne 1 -or
    $windowsDirectShrinkDispositionCount -ne 1 -or
    $windowsDirectShrinkDeleteAccessCount -ne 3) {
    $failures += 'Windows direct shrink clone platform writer tokens must remain fixed at 1 GENERIC_WRITE, 1 handle disposition helper, and 3 DELETE accesses (seal, exact cleanup, partial cleanup); all raw target writes stay behind ITargetDiskWriter'
}

$cloneBootFinalizationText = Get-Content `
    -LiteralPath $cloneBootFinalizationPath `
    -Raw
$cloneBootFinalizationTestText = Get-Content `
    -LiteralPath $cloneBootFinalizationTestPath `
    -Raw
$cloneBootFinalizationRequiredPatterns = [ordered]@{
    '\binspect_clone_efi_ownership\s*\(' = 'read-only exact ESP ownership preflight'
    'EfiBootOwnershipState::\s*\r?\n?\s*microsoft_only_or_empty' = 'safe-only cloned ESP ownership policy'
    '\befi_boot_ownership_allows_microsoft_rebuild\s*\(' = 'Microsoft-only rebuild eligibility proof'
    '\.system_volume_identity_root\s*=' = 'exact ESP Volume GUID identity binding'
    '\.require_efi_ownership_recheck\s*=' = 'immediate pre-mutation ownership recheck'
    'BootRepairThirdPartyEfiPolicy::not_applicable' = 'no implicit third-party EFI authorization'
    '\.update_current_pc_nvram\s*=\s*false' = 'explicit NVRAM non-mutation contract'
    '!repaired\.value\(\)\.efi_ownership_revalidated' = 'standalone ownership revalidation evidence gate'
    '!repaired\.value\(\)\.nvram_unchanged' = 'standalone NVRAM non-mutation evidence gate'
    'repaired\.value\(\)\.temporary_mount_released' = 'inner service must not release outer-owned mounts'
    '\.temporary_mounts_released\s*=\s*true' = 'outer temporary mount release evidence'
}
foreach ($pattern in $cloneBootFinalizationRequiredPatterns.Keys) {
    if ($cloneBootFinalizationText -notmatch $pattern) {
        $failures += "Clone boot finalization must retain $($cloneBootFinalizationRequiredPatterns[$pattern])"
    }
}
if ($cloneBootFinalizationText -match
        '\.update_current_pc_nvram\s*=\s*true') {
    $failures += 'Clone boot finalization must never enable current-PC NVRAM mutation'
}
$cloneBootFinalizationTestRequiredPatterns = [ordered]@{
    '\btest_ambiguous_efi_is_rejected_before_mounting\s*\(' = 'ambiguous EFI pre-mutation rejection coverage'
    '\btest_untrusted_efi_is_rejected_before_mounting\s*\(' = 'untrusted EFI pre-mutation rejection coverage'
    '\btest_wrong_esp_identity_is_rejected_before_mounting\s*\(' = 'foreign ESP identity rejection coverage'
    '\btest_efi_ownership_drift_is_rejected_and_mounts_are_released\s*\(' = 'ownership drift and mount cleanup coverage'
    '\btest_bios_carries_not_applicable_efi_policy\s*\(' = 'BIOS not-applicable ownership coverage'
    '\btest_unverified_boot_report_is_rejected\s*\(' = 'signed/fresh/ownership/NVRAM evidence rejection coverage'
}
foreach ($pattern in $cloneBootFinalizationTestRequiredPatterns.Keys) {
    if ($cloneBootFinalizationTestText -notmatch $pattern) {
        $failures += "Clone boot finalization tests must retain $($cloneBootFinalizationTestRequiredPatterns[$pattern])"
    }
}

$logWriterText = Get-Content -LiteralPath $auditedLogFileWriterPath -Raw
$logWriterRequiredPatterns = [ordered]@{
    '\bCREATE_NEW\b'      = 'CREATE_NEW'
    '\bFILE_SHARE_READ\b' = 'FILE_SHARE_READ'
}
foreach ($pattern in $logWriterRequiredPatterns.Keys) {
    if ($logWriterText -notmatch $pattern) {
        $failures += "CloneCore\src\log.cpp must retain $($logWriterRequiredPatterns[$pattern])"
    }
}

$logWriterForbiddenPatterns = [ordered]@{
    '\bCREATE_ALWAYS\b'       = 'CREATE_ALWAYS'
    '\bOPEN_ALWAYS\b'         = 'OPEN_ALWAYS'
    '\bOPEN_EXISTING\b'       = 'OPEN_EXISTING'
    '\bTRUNCATE_EXISTING\b'   = 'TRUNCATE_EXISTING'
    '\bFILE_SHARE_WRITE\b'    = 'FILE_SHARE_WRITE'
    '\bFILE_SHARE_DELETE\b'   = 'FILE_SHARE_DELETE'
    '\bDeviceIoControl\s*\('  = 'DeviceIoControl'
    '\bPhysicalDrive\b'       = 'PhysicalDrive'
    '\bIOCTL_[A-Z0-9_]+\b'    = 'IOCTL_*'
    '\bFSCTL_[A-Z0-9_]+\b'    = 'FSCTL_*'
}
foreach ($pattern in $logWriterForbiddenPatterns.Keys) {
    if ($logWriterText -match $pattern) {
        $failures += "CloneCore\src\log.cpp contains forbidden log-writer token $($logWriterForbiddenPatterns[$pattern])"
    }
}

$logWriteFileCount = ([regex]::Matches(
        $logWriterText,
        '\bWriteFile\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$logGenericWriteCount = ([regex]::Matches(
        $logWriterText,
        '\bGENERIC_WRITE\b',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
if ($logWriteFileCount -ne 1 -or $logGenericWriteCount -ne 1) {
    $failures += 'CloneCore\src\log.cpp may contain exactly one WriteFile call and one GENERIC_WRITE access request'
}

$shrinkBundleWriterText = Get-Content `
    -LiteralPath $auditedShrinkBundleWriterPath `
    -Raw
$shrinkBundleRequiredPatterns = [ordered]@{
    '\bCREATE_NEW\b' = 'non-overwriting manifest creation'
    '\bFILE_FLAG_WRITE_THROUGH\b' = 'write-through manifest creation'
    '\bFlushFileBuffers\s*\(' = 'manifest flush before verification'
    '\bMOVEFILE_WRITE_THROUGH\b' = 'write-through final directory commit'
    '\bverify_shrink_bundle_read_only\s*\(' = 'full bundle verification before and after commit'
    '\bFILE_ATTRIBUTE_REPARSE_POINT\b' = 'scratch reparse-point rejection'
}
foreach ($pattern in $shrinkBundleRequiredPatterns.Keys) {
    if ($shrinkBundleWriterText -notmatch $pattern) {
        $failures += "MigrationEngine\src\bundle_capture.cpp must retain $($shrinkBundleRequiredPatterns[$pattern])"
    }
}
$shrinkBundleWriteFileCount = ([regex]::Matches(
        $shrinkBundleWriterText,
        '\bWriteFile\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$shrinkBundleGenericWriteCount = ([regex]::Matches(
        $shrinkBundleWriterText,
        '\bGENERIC_WRITE\b',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$shrinkBundleMoveCount = ([regex]::Matches(
        $shrinkBundleWriterText,
        '\bMoveFileExW\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$shrinkBundleDeleteCount = ([regex]::Matches(
        $shrinkBundleWriterText,
        '\bDeleteFileW\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
if ($shrinkBundleWriteFileCount -ne 1 -or
    $shrinkBundleGenericWriteCount -ne 1 -or
    $shrinkBundleMoveCount -ne 1 -or
    $shrinkBundleDeleteCount -ne 1) {
    $failures += 'MigrationEngine bundle writer may contain exactly one WriteFile, GENERIC_WRITE, MoveFileExW, and DeleteFileW call'
}

$checkpointWriterText = Get-Content `
    -LiteralPath $auditedCheckpointWriterPath `
    -Raw
$checkpointWriterRequiredPatterns = [ordered]@{
    '\bCREATE_NEW\b' = 'non-overwriting fixed stage creation'
    'path \+ L"\.new"' = 'single fixed adjacent .new stage path'
    '\bFILE_FLAG_WRITE_THROUGH\b' = 'write-through stage creation'
    '\bFlushFileBuffers\s*\(' = 'flush before stage readback'
    '\bread_checkpoint_handle\s*\(file\.get\(\)\)' = 'same-handle stage readback'
    '\bFILE_FLAG_OPEN_REPARSE_POINT\b' = 'reparse-point-safe existing-file open'
    '\bMOVEFILE_WRITE_THROUGH\b' = 'write-through first commit'
    '\bREPLACEFILE_WRITE_THROUGH\b' = 'atomic write-through replacement'
    'expected_current_record_hash' = 'caller-bound current-record hash'
    'expected_record_hash' = 'caller-bound discard hash'
    '\bdigest_equal\s*\(' = 'constant-time record-hash comparison'
}
foreach ($pattern in $checkpointWriterRequiredPatterns.Keys) {
    if ($checkpointWriterText -notmatch $pattern) {
        $failures += "OperationCore\src\checkpoint.cpp must retain $($checkpointWriterRequiredPatterns[$pattern])"
    }
}
$checkpointWriteFileCount = ([regex]::Matches(
        $checkpointWriterText,
        '\bWriteFile\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$checkpointGenericWriteCount = ([regex]::Matches(
        $checkpointWriterText,
        '\bGENERIC_WRITE\b',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$checkpointMoveCount = ([regex]::Matches(
        $checkpointWriterText,
        '\bMoveFileExW\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$checkpointReplaceCount = ([regex]::Matches(
        $checkpointWriterText,
        '\bReplaceFileW\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$checkpointDispositionCount = ([regex]::Matches(
        $checkpointWriterText,
        '\bSetFileInformationByHandle\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$checkpointDeleteAccessCount = ([regex]::Matches(
        $checkpointWriterText,
        '\bDELETE\b',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
if ($checkpointWriteFileCount -ne 1 -or
    $checkpointGenericWriteCount -ne 1 -or
    $checkpointMoveCount -ne 1 -or
    $checkpointReplaceCount -ne 1 -or
    $checkpointDispositionCount -ne 2 -or
    $checkpointDeleteAccessCount -ne 3) {
    $failures += 'OperationCore checkpoint store must retain exactly one WriteFile, GENERIC_WRITE, MoveFileExW, ReplaceFileW; two disposition calls; and three DELETE accesses'
}

$windowsResumeSlotWriterText = Get-Content `
    -LiteralPath $auditedWindowsResumeSlotWriterPath `
    -Raw
$windowsResumeSlotRequiredPatterns = [ordered]@{
    'L"active\.checkpoint\.new"' = 'one fixed EXE-adjacent active.checkpoint.new stage'
    '\bCREATE_NEW\b' = 'non-overwriting owned stage creation'
    '\bFILE_FLAG_WRITE_THROUGH\b' = 'write-through stage handle'
    '\bFlushFileBuffers\s*\(' = 'flush before same-handle readback'
    'read_bounded_file\s*\(\s*stage\.get\(\)' = 'bounded same-handle stage readback'
    '\bkMaximumWindowsResumeSlotBytes\b' = 'fixed maximum slot allocation and read bound'
    '\bFileAttributeTagInfo\b' = 'regular and reparse attribute query'
    '\bFileStandardInfo\b' = 'single-link and bounded-size query'
    '\bFileIdInfo\b' = 'opened file-object identity query'
    '\bFILE_FLAG_OPEN_REPARSE_POINT\b' = 'reparse-safe opens'
    '\bMOVEFILE_WRITE_THROUGH\b' = 'write-through first publication'
    '\bREPLACEFILE_WRITE_THROUGH\b' = 'atomic write-through replacement'
    '\bexpected_checkpoint_record_hash\b' = 'caller-bound replace record Hash'
    '\bmake_resume_slot_binding\s*\(' = 'complete discard binding revalidation'
    '\bFILE_DISPOSITION_INFO\b' = 'handle-bound guarded deletion'
    '\bpartial_identity_hash\s*\(' = 'owned partial opened File ID binding'
    '\bowned_object_identity_hash\s*\(' = 'role-tagged partial/journal opened File ID binding'
    'binding\.owned_object_file_bindings\.size\(\)' = 'exact reviewed multi-object discard cardinality'
    '\brename_open_handle_no_replace\s*\(' = 'opened image-partial no-overwrite publish helper'
    'rename->ReplaceIfExists\s*=\s*FALSE' = 'completed image no-overwrite publication'
    '\bcommit_persistent_pe_exact_image_create\s*\(' = 'capability-8 publish, full verification, and retirement coordinator'
    'FILE_SHARE_READ,\s*\r?\n\s*nullptr,\s*\r?\n\s*OPEN_EXISTING,\s*\r?\n\s*FILE_FLAG_OPEN_REPARSE_POINT\s*\|\s*FILE_FLAG_SEQUENTIAL_SCAN' = 'published image read lock denying write and delete sharing'
    '\bidentity_from_open_handle\b' = 'caller opened-handle backing proof'
    '\bseparated_from_source\b' = 'caller source/data backing separation proof'
}
foreach ($pattern in $windowsResumeSlotRequiredPatterns.Keys) {
    if ($windowsResumeSlotWriterText -notmatch $pattern) {
        $failures += "OperationCore Windows resume slot adapter must retain $($windowsResumeSlotRequiredPatterns[$pattern])"
    }
}
$windowsResumeSlotHeaderText = Get-Content `
    -LiteralPath (Join-Path $sourceRoot 'OperationCore\include\ytec\operationcore\windows_resume_slot_platform.h') `
    -Raw
$windowsResumeSlotTestText = Get-Content `
    -LiteralPath (Join-Path $repoRoot 'tests\Unit\windows_resume_slot_platform_tests.cpp') `
    -Raw
$windowsResumeSlotEnvelopeRequiredPatterns = [ordered]@{
    'kMaximumWindowsResumeSlotBytes\s*=\s*\r?\n\s*384U\s*\*\s*1024U' = '384 KiB authenticated slot envelope bound'
    '\btest_maximum_v2_checkpoint_envelope_survives_restart_and_replace\s*\(' = '4096-sector envelope restart/replace/discard coverage'
    'kMaximumCheckpointPreparationSectors' = 'maximum preparation-sector fixture bound'
}
foreach ($pattern in $windowsResumeSlotEnvelopeRequiredPatterns.Keys) {
    if ($windowsResumeSlotHeaderText -notmatch $pattern -and
        $windowsResumeSlotTestText -notmatch $pattern) {
        $failures += "OperationCore Windows resume slot envelope must retain $($windowsResumeSlotEnvelopeRequiredPatterns[$pattern])"
    }
}
$windowsResumeSlotForbiddenPatterns = [ordered]@{
    '\bCREATE_ALWAYS\b' = 'overwriting file creation'
    '\bOPEN_ALWAYS\b' = 'existing-file reuse'
    '\bTRUNCATE_EXISTING\b' = 'existing-file truncation'
    '\bSetEndOfFile\s*\(' = 'file truncation or extension'
    '\bDeleteFileW\s*\(' = 'path-only deletion'
    '\\\\.\\PhysicalDrive' = 'physical disk path'
    '\bDeviceIoControl\s*\(' = 'disk or volume control I/O'
}
foreach ($pattern in $windowsResumeSlotForbiddenPatterns.Keys) {
    if ($windowsResumeSlotWriterText -match $pattern) {
        $failures += "OperationCore Windows resume slot adapter contains forbidden $($windowsResumeSlotForbiddenPatterns[$pattern])"
    }
}
$windowsResumeSlotWriteFileCount = ([regex]::Matches(
        $windowsResumeSlotWriterText,
        '\bWriteFile\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$windowsResumeSlotGenericWriteCount = ([regex]::Matches(
        $windowsResumeSlotWriterText,
        '\bGENERIC_WRITE\b',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$windowsResumeSlotMoveCount = ([regex]::Matches(
        $windowsResumeSlotWriterText,
        '\bMoveFileExW\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$windowsResumeSlotReplaceCount = ([regex]::Matches(
        $windowsResumeSlotWriterText,
        '\bReplaceFileW\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$windowsResumeSlotDispositionCount = ([regex]::Matches(
        $windowsResumeSlotWriterText,
        '\bSetFileInformationByHandle\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$windowsResumeSlotDeleteAccessCount = ([regex]::Matches(
        $windowsResumeSlotWriterText,
        '\bDELETE\b',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
if ($windowsResumeSlotWriteFileCount -ne 1 -or
    $windowsResumeSlotGenericWriteCount -ne 1 -or
    $windowsResumeSlotMoveCount -ne 1 -or
    $windowsResumeSlotReplaceCount -ne 1 -or
    $windowsResumeSlotDispositionCount -ne 2 -or
    $windowsResumeSlotDeleteAccessCount -ne 9) {
    $failures += 'OperationCore Windows resume slot adapter writer tokens must remain fixed at 1 WriteFile, 1 GENERIC_WRITE, 1 MoveFileExW, 1 ReplaceFileW, 2 handle information calls (guarded disposition plus no-replace image publish), and 9 DELETE accesses (slot stage/create cleanup, generic checkpoint/legacy/multi-object discard, and capability-8 checkpoint/image/journal exact-open retirement)'
}

$supportZipWriterText = Get-Content `
    -LiteralPath $auditedSupportZipWriterPath `
    -Raw
$supportZipHeaderText = Get-Content `
    -LiteralPath (Join-Path $sourceRoot 'WindowsApp\include\ytec\windowsapp\support_zip.h') `
    -Raw
$supportZipTestText = Get-Content `
    -LiteralPath (Join-Path $repoRoot 'tests\Unit\support_zip_tests.cpp') `
    -Raw
$supportZipRequiredPatterns = [ordered]@{
    '\bclassify_product_log_file_name\s*\(' = 'strict product-owned log basename classification'
    '\bMB_ERR_INVALID_CHARS\b' = 'strict UTF-8 decoding'
    '\bsanitize_main_log_message\s*\(' = 'line-by-line product privacy masking'
    '\bFileAttributeTagInfo\b' = 'regular and non-reparse attribute query'
    '\bFileStandardInfo\b' = 'single-link and bounded-size query'
    '\bFileIdInfo\b' = 'opened file-object identity query'
    '\bGetFinalPathNameByHandleW\s*\(' = 'opened-handle path re-identification'
    '\bFILE_FLAG_OPEN_REPARSE_POINT\b' = 'reparse-safe source and directory opens'
    '\bread_bounded_file\s*\(' = 'bounded source and archive reads'
    '\bselect_support_zip_candidates\s*\(' = 'deterministic newest-first bounded subset selection'
    '\bexcluded_count\b' = 'user-visible excluded log count'
    '\bkZipStoredMethod\b' = 'dependency-free stored ZIP method'
    '\bkZipLocalHeaderSignature\b' = 'ZIP local header'
    '\bkZipCentralHeaderSignature\b' = 'ZIP central directory'
    '\bkZipEndSignature\b' = 'ZIP end-of-central-directory record'
    '\bcrc32\s*\(' = 'stored entry CRC-32 generation and verification'
    '\bCREATE_NEW\b' = 'non-overwriting adjacent partial creation'
    '\bFILE_FLAG_WRITE_THROUGH\b' = 'write-through partial handle'
    '\bFlushFileBuffers\s*\(' = 'flush before and after publication'
    'read_bounded_file\s*\(\s*partial\.get\(\)' = 'bounded same-handle full archive readback'
    '\bverify_stored_zip\s*\(' = 'full local, central, EOCD, range, data and CRC verification'
    '\bFILE_RENAME_INFO\b' = 'handle-bound atomic publication'
    '\bReplaceIfExists\s*=\s*FALSE' = 'non-overwriting atomic publication'
    '\bFILE_DISPOSITION_INFO\b' = 'handle-bound owned-partial cleanup'
    '\bOwnedPartialCleanup\b' = 'exception-safe exact owned-partial cleanup'
    '\bsame_published_file\s*\(' = 'published path File-ID and shape re-identification'
    '\bcanonical_local_path\s*\(' = 'canonical local absolute path validation'
    '\bcontains_appdata_component\s*\(' = 'AppData rejection'
    '\bpath_inside_or_equal\s*\(' = 'portable data subtree rejection'
    '\boutput_directory_identity_\b' = 'planned output-directory volume and File-ID binding'
}
foreach ($pattern in $supportZipRequiredPatterns.Keys) {
    if ($supportZipWriterText -notmatch $pattern) {
        $failures += "WindowsApp support ZIP adapter must retain $($supportZipRequiredPatterns[$pattern])"
    }
}
$supportZipHeaderRequiredPatterns = [ordered]@{
    '\bkSupportZipMaximumEntryCount\b' = 'bounded selected entry count'
    '\bkSupportZipMaximumCandidateCount\b' = 'bounded candidate count'
    '\bkSupportZipMaximumSourceFileBytes\b' = 'bounded source file size'
    '\bkSupportZipPreferredSubsetBytes\b' = 'normal newest-first subset byte budget'
    '\bkSupportZipMaximumSourceTotalBytes\b' = 'complete retained-state source ceiling'
    '\bkSupportZipMaximumArchiveBytes\b' = 'bounded complete archive size'
    '\bSupportZipDisplayEntry\b' = 'basename-only user review entries'
    '\bexcluded_log_count\b' = 'user-visible exclusion count'
    '\blocal_only\s*\{\s*true\s*\}' = 'explicit no-auto-send report'
}
foreach ($pattern in $supportZipHeaderRequiredPatterns.Keys) {
    if ($supportZipHeaderText -notmatch $pattern) {
        $failures += "WindowsApp support ZIP contract must retain $($supportZipHeaderRequiredPatterns[$pattern])"
    }
}
$supportZipUiHeaderPath = Join-Path `
    $sourceRoot `
    'WindowsApp\include\ytec\windowsapp\support_zip_ui.h'
$supportZipUiSourcePath = Join-Path `
    $sourceRoot `
    'WindowsApp\src\support_zip_ui.cpp'
$supportZipUiTestPath = Join-Path `
    $repoRoot `
    'tests\Unit\support_zip_ui_tests.cpp'
$supportZipMainPath = Join-Path $sourceRoot 'WindowsApp\src\main.cpp'
foreach ($requiredPath in @(
        $supportZipUiHeaderPath,
        $supportZipUiSourcePath,
        $supportZipUiTestPath)) {
    if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
        $failures += "Support ZIP product UI contract is missing $requiredPath"
    }
}
if ((Test-Path -LiteralPath $supportZipUiHeaderPath -PathType Leaf) -and
    (Test-Path -LiteralPath $supportZipUiSourcePath -PathType Leaf) -and
    (Test-Path -LiteralPath $supportZipUiTestPath -PathType Leaf)) {
    $supportZipUiHeaderText = Get-Content -LiteralPath $supportZipUiHeaderPath -Raw
    $supportZipUiSourceText = Get-Content -LiteralPath $supportZipUiSourcePath -Raw
    $supportZipUiTestText = Get-Content -LiteralPath $supportZipUiTestPath -Raw
    $supportZipMainText = Get-Content -LiteralPath $supportZipMainPath -Raw
    $supportZipMainRequiredPatterns = [ordered]@{
        '\bGetSaveFileNameW\s*\(' = 'local Save As selection'
        '\bplan_current_executable_support_zip\s*\(' = 'bounded background planning'
        '\bbuild_support_zip_review_model\s*\(' = 'pre-creation basename-only review'
        '\bcreate_windows_support_zip\s*\(' = 'audited local ZIP creation'
        '\bkSupportZipPlanCompleteMessage\b' = 'asynchronous plan completion'
        '\bkSupportZipCreationCompleteMessage\b' = 'asynchronous creation completion'
        '\bsupport_zip_ui_contract\s*\(' = 'single UI label/keyboard contract'
    }
    foreach ($pattern in $supportZipMainRequiredPatterns.Keys) {
        if ($supportZipMainText -notmatch $pattern) {
            $failures += "WindowsApp support ZIP product route must retain $($supportZipMainRequiredPatterns[$pattern])"
        }
    }
    $supportZipUiHeaderRequiredPatterns = [ordered]@{
        'L"サポートZIP保存"' = 'explicit diagnostics action label'
        'L"ローカルZIPを作成"' = 'explicit local-only creation label'
        '\.escape_cancels_review\s*=\s*true' = 'Esc cancellation contract'
        '\.automatic_send\s*=\s*false' = 'no automatic transmission contract'
    }
    foreach ($pattern in $supportZipUiHeaderRequiredPatterns.Keys) {
        if ($supportZipUiHeaderText -notmatch $pattern) {
            $failures += "WindowsApp support ZIP UI contract must retain $($supportZipUiHeaderRequiredPatterns[$pattern])"
        }
    }
    $supportZipUiTestRequiredPatterns = [ordered]@{
        '\bverify_layout\s*\(\s*960\s*,\s*516\s*\)' = '1024x600 at 200 percent logical layout coverage'
        '\bescape_cancels_review\b' = 'Esc cancellation assertion'
        '!contract\.automatic_send' = 'automatic-send false assertion'
    }
    foreach ($pattern in $supportZipUiTestRequiredPatterns.Keys) {
        if ($supportZipUiTestText -notmatch $pattern) {
            $failures += "WindowsApp support ZIP UI tests must retain $($supportZipUiTestRequiredPatterns[$pattern])"
        }
    }
    if ($supportZipUiSourceText -match
            '\b(?:ShellExecute|WinHttp)[A-Za-z0-9_]*\s*\(') {
        $failures += 'WindowsApp support ZIP UI model must not launch a shell or access a network API'
    }
}
$supportZipForbiddenPatterns = [ordered]@{
    '\bCREATE_ALWAYS\b' = 'overwriting file creation'
    '\bOPEN_ALWAYS\b' = 'existing-file reuse'
    '\bTRUNCATE_EXISTING\b' = 'existing-file truncation'
    '\bFILE_FLAG_DELETE_ON_CLOSE\b' = 'path-unbound implicit deletion'
    '\bSetEndOfFile\s*\(' = 'file truncation or extension'
    '\bDeleteFileW\s*\(' = 'path-only deletion'
    '\bMoveFileExW\s*\(' = 'path-only publication'
    '\bReplaceFileW\s*\(' = 'overwriting publication'
    '\\\\.\\PhysicalDrive' = 'physical disk path'
    '\bDeviceIoControl\s*\(' = 'disk or volume control I/O'
    '\bCreateProcess(?:A|W)?\s*\(' = 'external tool launch'
    '\bShellExecute(?:Ex)?(?:A|W)?\s*\(' = 'shell or external tool launch'
    '\bWinHttp(?:Open|Connect|OpenRequest|SendRequest|ReceiveResponse|ReadData)\s*\(' = 'network transmission'
}
foreach ($pattern in $supportZipForbiddenPatterns.Keys) {
    if ($supportZipWriterText -match $pattern) {
        $failures += "WindowsApp support ZIP adapter contains forbidden $($supportZipForbiddenPatterns[$pattern])"
    }
}
$supportZipWriteFileCount = ([regex]::Matches(
        $supportZipWriterText,
        '\bWriteFile\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$supportZipGenericWriteCount = ([regex]::Matches(
        $supportZipWriterText,
        '\bGENERIC_WRITE\b',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$supportZipCreateNewCount = ([regex]::Matches(
        $supportZipWriterText,
        '\bCREATE_NEW\b',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$supportZipDispositionCount = ([regex]::Matches(
        $supportZipWriterText,
        '\bSetFileInformationByHandle\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$supportZipDeleteAccessCount = ([regex]::Matches(
        $supportZipWriterText,
        '\bDELETE\b',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
if ($supportZipWriteFileCount -ne 1 -or
    $supportZipGenericWriteCount -ne 1 -or
    $supportZipCreateNewCount -ne 1 -or
    $supportZipDispositionCount -ne 2 -or
    $supportZipDeleteAccessCount -ne 1) {
    $failures += 'WindowsApp support ZIP writer tokens must remain fixed at 1 WriteFile, 1 GENERIC_WRITE, 1 CREATE_NEW, 2 handle disposition/rename calls, and 1 DELETE access'
}
$supportZipTestRequiredPatterns = [ordered]@{
    '200U\s*\*\s*kMiB' = '200 MiB selection boundary without a real allocation'
    '\bFSCTL_SET_SPARSE\b' = 'sparse oversized source fixture'
    '\bCreateHardLinkW\s*\(' = 'hardlink rejection fixture'
    '\bFSCTL_SET_REPARSE_POINT\b' = 'reparse rejection fixture'
    'tampered-and-longer' = 'post-review tamper fixture'
    'stage=replacement' = 'post-review File-ID replacement fixture'
    'publish-collision' = 'foreign final preservation fixture'
    'partial-collision' = 'foreign partial preservation fixture'
    '\[PRIVATE\]' = 'credential masking assertion'
    '\[PATH\]' = 'path masking assertion'
    '\\\\\\\\server\\\\share' = 'UNC rejection fixture'
}
foreach ($pattern in $supportZipTestRequiredPatterns.Keys) {
    if ($supportZipTestText -notmatch $pattern) {
        $failures += "WindowsApp support ZIP tests must retain $($supportZipTestRequiredPatterns[$pattern])"
    }
}

$tsumugiStreamWriterText = Get-Content `
    -LiteralPath $auditedTsumugiStreamWriterPath `
    -Raw
$tsumugiStreamRequiredPatterns = [ordered]@{
    '\bCREATE_NEW\b' = 'non-overwriting adjacent .partial creation'
    '\bFlushFileBuffers\s*\(' = 'flush before complete same-handle verification'
    '\bverify_open_handle\s*\(\s*partial\.get\(\)' = 'complete same-handle verification before commit'
    '\bReplaceIfExists\s*=\s*FALSE' = 'handle rename without implicit overwrite'
    '\bsame_file_id\s*\(' = 'stable file identity before owned cleanup and commit'
    '\bFILE_FLAG_OPEN_REPARSE_POINT\b' = 'reparse-safe existing image observation'
    'GENERIC_READ\s*\|\s*GENERIC_WRITE,\s*\r?\n\s*FILE_SHARE_READ' = 'bound resume reopen with content write access but no delete access or write/delete sharing'
    'callbacks_started_after_complete_verification' = 'restore callbacks after complete validation only'
}
foreach ($pattern in $tsumugiStreamRequiredPatterns.Keys) {
    if ($tsumugiStreamWriterText -notmatch $pattern) {
        $failures += "ImageFormat\src\tsumugi_stream.cpp must retain $($tsumugiStreamRequiredPatterns[$pattern])"
    }
}
$tsumugiStreamWriteCount = ([regex]::Matches(
        $tsumugiStreamWriterText,
        '\bWriteFile\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$tsumugiStreamGenericWriteCount = ([regex]::Matches(
        $tsumugiStreamWriterText,
        '\bGENERIC_WRITE\b',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$tsumugiStreamDeleteCount = ([regex]::Matches(
        $tsumugiStreamWriterText,
        '\bDeleteFileW\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$tsumugiStreamSetLengthCount = ([regex]::Matches(
        $tsumugiStreamWriterText,
        '\bSetEndOfFile\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$tsumugiStreamDispositionCount = ([regex]::Matches(
        $tsumugiStreamWriterText,
        '\bSetFileInformationByHandle\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$tsumugiStreamDeleteAccessCount = ([regex]::Matches(
        $tsumugiStreamWriterText,
        '\bDELETE\b',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
if ($tsumugiStreamWriteCount -ne 1 -or
    $tsumugiStreamGenericWriteCount -ne 2 -or
    $tsumugiStreamDeleteCount -ne 2 -or
    $tsumugiStreamSetLengthCount -ne 1 -or
    $tsumugiStreamDispositionCount -ne 2 -or
    $tsumugiStreamDeleteAccessCount -ne 2) {
    $failures += 'Tsumugi stream writer must retain exactly one WriteFile and SetEndOfFile; two GENERIC_WRITE accesses (CREATE_NEW owned partial and exact bound resume reopen); and two DeleteFileW, SetFileInformationByHandle, and DELETE accesses'
}

$tsumugiRescueStagingText = Get-Content `
    -LiteralPath $auditedTsumugiRescueStagingPath `
    -Raw
$tsumugiRescueStagingRequiredPatterns = [ordered]@{
    '\bCREATE_NEW\b' = 'non-overwriting adjacent rescue staging creation'
    'D:P\(A;;FA;;;SY\)\(A;;FA;;;BA\)\(A;;FA;;;OW\)' = 'restricted staging DACL'
    '\bFILE_SHARE_READ\b' = 'write and delete sharing denial'
    '\bFILE_FLAG_OPEN_REPARSE_POINT\b' = 'reparse-safe read-only reopen'
    '\bGetFinalPathNameByHandleW\s*\(' = 'opened staging path reidentification'
    '\bFileIdInfo\b' = 'stable staging File ID capture'
    'NumberOfLinks\s*!=\s*1U' = 'hard-link rejection'
    '\bFlushFileBuffers\s*\(' = 'flush before read-only sealing'
    '\bsame_sealed_observation\s*\(' = 'read-only reopen identity comparison'
    '\bFileDispositionInfo\b' = 'exact owned-handle cleanup'
    'required_available_bytes\s*-\s*request\.source_disk_size' = 'staging plus final-image capacity reservation'
    '\bvalidate_active_destination\s*\(' = 'destination identity retention after source loss'
    '\bvalidate_image_destination_before_commit\s*\(' = 'post-discard final-image destination revalidation'
}
foreach ($pattern in $tsumugiRescueStagingRequiredPatterns.Keys) {
    if ($tsumugiRescueStagingText -notmatch $pattern) {
        $failures += "ImageFormat\src\windows_tsumugi_rescue_staging.cpp must retain $($tsumugiRescueStagingRequiredPatterns[$pattern])"
    }
}
$tsumugiRescueStagingForbiddenPatterns = [ordered]@{
    '\bDeleteFileW\s*\(' = 'path-based cleanup'
    '\bCREATE_ALWAYS\b' = 'CREATE_ALWAYS overwrite'
    '\bOPEN_ALWAYS\b' = 'OPEN_ALWAYS existing-file reuse'
    '\bTRUNCATE_EXISTING\b' = 'TRUNCATE_EXISTING overwrite'
    '\bFILE_SHARE_WRITE\b' = 'write sharing during staging ownership'
    '\bFILE_SHARE_DELETE\b' = 'delete sharing during staging ownership'
}
foreach ($pattern in $tsumugiRescueStagingForbiddenPatterns.Keys) {
    if ($tsumugiRescueStagingText -match $pattern) {
        $failures += "ImageFormat\src\windows_tsumugi_rescue_staging.cpp contains forbidden $($tsumugiRescueStagingForbiddenPatterns[$pattern])"
    }
}
$tsumugiRescueStagingWriteCount = ([regex]::Matches(
        $tsumugiRescueStagingText,
        '\bWriteFile\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$tsumugiRescueStagingGenericWriteCount = ([regex]::Matches(
        $tsumugiRescueStagingText,
        '\bGENERIC_WRITE\b',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$tsumugiRescueStagingSetLengthCount = ([regex]::Matches(
        $tsumugiRescueStagingText,
        '\bSetEndOfFile\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$tsumugiRescueStagingDispositionCount = ([regex]::Matches(
        $tsumugiRescueStagingText,
        '\bSetFileInformationByHandle\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$tsumugiRescueStagingDeleteAccessCount = ([regex]::Matches(
        $tsumugiRescueStagingText,
        '\bDELETE\b',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
if ($tsumugiRescueStagingWriteCount -ne 1 -or
    $tsumugiRescueStagingGenericWriteCount -ne 1 -or
    $tsumugiRescueStagingSetLengthCount -ne 1 -or
    $tsumugiRescueStagingDispositionCount -ne 1 -or
    $tsumugiRescueStagingDeleteAccessCount -ne 3) {
    $failures += 'Tsumugi rescue staging must retain exactly one WriteFile, GENERIC_WRITE, SetEndOfFile, and disposition call; and three DELETE accesses'
}

$windowsDataRescueImagePath = Join-Path `
    $sourceRoot `
    'WindowsApp\src\online_image_create.cpp'
$windowsDataRescueImageText = Get-Content `
    -LiteralPath $windowsDataRescueImagePath `
    -Raw
$windowsDataRescueImageRequiredPatterns = [ordered]@{
    '\bexecute_windows_data_rescue_image_create\s*\(' = 'dedicated Windows data rescue image controller'
    'source\.is_system_disk' = 'running-system-disk rejection'
    'source\.logical_sector_size\s*!=\s*512U' = '512-byte logical-sector gate'
    '\bis_supported_sector_size_pair\s*\(' = 'supported physical-sector pair gate'
    '\bobserved_preprotected\b' = 'read-only or offline source revalidation'
    '\bsame_reviewed_layout\s*\(' = 'reviewed source layout revalidation'
    'source\.value\(\)\.observed\.identity\.size_bytes[\s\S]*?sizing\.value\(\)\.maximum_image_bytes' = 'staging plus maximum image capacity reservation'
    '\bmake_rescue_staging\s*\(' = 'owned rescue staging construction'
    'RescueExecutionEnvironment::windows' = 'Windows rescue classification'
    'RescueSourceKind::data_disk' = 'data-disk-only rescue classification'
    '\bstaging_sealed_for_image_read\b' = 'sealed staging completion gate'
    '\bstaging_discarded_before_final_commit\b' = 'staging discard completion gate'
    '\bstaging_destination_revalidated_before_final_commit\b' = 'post-discard destination revalidation gate'
}
foreach ($pattern in $windowsDataRescueImageRequiredPatterns.Keys) {
    if ($windowsDataRescueImageText -notmatch $pattern) {
        $failures += "WindowsApp\src\online_image_create.cpp must retain $($windowsDataRescueImageRequiredPatterns[$pattern])"
    }
}

# The current .tsumugi destination trust boundary is read-only and must stay
# independent from the temporary legacy .dcimg staging compatibility layer.
$tsumugiDestinationPath = Join-Path `
    $sourceRoot `
    'ImageFormat\src\windows_tsumugi_destination.cpp'
$tsumugiDestinationText = Get-Content `
    -LiteralPath $tsumugiDestinationPath `
    -Raw
$tsumugiDestinationRequiredPatterns = [ordered]@{
    '\bGetFullPathNameW\s*\(' = 'canonical local-drive path resolution'
    '\bFILE_FLAG_OPEN_REPARSE_POINT\b' = 'reparse-safe parent and file observation'
    '\bGetFinalPathNameByHandleW\s*\(' = 'opened parent and file path reidentification'
    '\bFileIdInfo\b' = 'stable parent and file identity observation'
    '\bIOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS\b' = 'single physical-disk extent validation'
    '\bGetVolumeInformationW\s*\(' = 'NTFS/exFAT validation'
    '\bvalidate_stable_identity\s*\(' = 'source and destination disk reidentification'
    'expected_owned_partial_bytes' = 'verified owned partial length recheck'
}
foreach ($pattern in $tsumugiDestinationRequiredPatterns.Keys) {
    if ($tsumugiDestinationText -notmatch $pattern) {
        $failures += "ImageFormat\src\windows_tsumugi_destination.cpp must retain $($tsumugiDestinationRequiredPatterns[$pattern])"
    }
}
$currentTsumugiCreateBoundaryFiles = @(
    'ImageFormat\include\ytec\imageformat\windows_tsumugi_destination.h'
    'ImageFormat\src\windows_tsumugi_destination.cpp'
    'ImageFormat\include\ytec\imageformat\windows_tsumugi_rescue_staging.h'
    'ImageFormat\src\windows_tsumugi_rescue_staging.cpp'
    'ImageFormat\include\ytec\imageformat\tsumugi.h'
    'ImageFormat\include\ytec\imageformat\tsumugi_stream.h'
    'ImageFormat\include\ytec\imageformat\tsumugi_image_service.h'
    'ImageFormat\src\tsumugi.cpp'
    'ImageFormat\src\tsumugi_stream.cpp'
    'ImageFormat\src\tsumugi_image_service.cpp'
    'VssRequester\include\ytec\vssrequester\tsumugi_snapshot.h'
    'VssRequester\src\tsumugi_snapshot.cpp'
    'VssRequester\include\ytec\vssrequester\online_tsumugi_backup.h'
    'VssRequester\src\online_tsumugi_backup.cpp'
    'WindowsApp\include\ytec\windowsapp\online_image_create.h'
    'WindowsApp\src\online_image_create.cpp'
    'WinPEApp\include\ytec\winpeapp\direct_image_create.h'
    'WinPEApp\src\direct_image_create.cpp'
)
$legacyStagingTokens = @(
    'windows_file_staging'
    'WindowsFileDestinationObservation'
    'IDcimg'
    'Dcimg'
    '.dcimg'
)
foreach ($relativePath in $currentTsumugiCreateBoundaryFiles) {
    $path = Join-Path $sourceRoot $relativePath
    if (-not (Test-Path -LiteralPath $path)) {
        $failures += "Current Tsumugi create boundary file is missing: $relativePath"
        continue
    }
    $text = Get-Content -LiteralPath $path -Raw
    foreach ($token in $legacyStagingTokens) {
        if ($text.IndexOf($token, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
            $failures += "$relativePath must not depend on legacy staging token $token"
        }
    }
}

$legacyShrinkExecutionServicePath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'WinPEApp\src\shrink_execution_service.cpp'))
if (Test-Path -LiteralPath $legacyShrinkExecutionServicePath) {
    $failures += 'Legacy .dcmig WinPE shrink execution service must remain removed from the product source tree'
}

# Pre-1.0 image and VSS implementations remain only as quarantined regression
# targets.  The current Windows and WinPE product targets must never acquire a
# direct dependency on them.  MigrationEngine still contains the quarantined
# .dcmig implementation, so it must also be split before a product target may
# link it again.
$productTargetCmakePaths = @(
    (Join-Path $sourceRoot 'WindowsApp\CMakeLists.txt')
    (Join-Path $sourceRoot 'WinPEApp\CMakeLists.txt')
)
$forbiddenLegacyProductTargets = @(
    'ytec::legacy_image_format'
    'ytec::legacy_vss_image'
    'ytec::migration_engine'
)
foreach ($cmakePath in $productTargetCmakePaths) {
    $cmakeText = Get-Content -LiteralPath $cmakePath -Raw
    foreach ($target in $forbiddenLegacyProductTargets) {
        if ($cmakeText.IndexOf(
                $target,
                [StringComparison]::OrdinalIgnoreCase) -ge 0) {
            $relative = $cmakePath.Substring($repoRoot.Length).TrimStart('\')
            $failures += "$relative must not link quarantined product target $target"
        }
    }
}

$shrinkVolumeApplyPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'MigrationEngine\src\volume_apply.cpp'))
$shrinkVolumeApplyText = Get-Content -LiteralPath $shrinkVolumeApplyPath -Raw
$shrinkVolumeApplyRequiredPatterns = [ordered]@{
    '\bQueryDosDeviceW\b' = 'target Volume GUID to NT device resolution'
    '\bDefineDosDeviceW\b' = 'temporary exact Volume GUID discovery link'
    '\bGetVolumeNameForVolumeMountPointW\b' = 'Volume GUID identity verification'
    '\bSetVolumeMountPointW\b' = 'Mount Manager drive-letter assignment'
    '\bDeleteVolumeMountPointW\b' = 'owned Mount Manager drive-letter cleanup'
    'DDD_EXACT_MATCH_ON_REMOVE' = 'exact temporary DOS-device cleanup'
    'status = execute_format\(\s*mount\.root\(\)' = 'verified Mount Manager drive root FORMAT target'
    '\bdecode_process_diagnostic\b' = 'bounded Microsoft FORMAT diagnostics'
}
foreach ($pattern in $shrinkVolumeApplyRequiredPatterns.Keys) {
    if ($shrinkVolumeApplyText -notmatch $pattern) {
        $failures += "MigrationEngine\src\volume_apply.cpp must retain $($shrinkVolumeApplyRequiredPatterns[$pattern])"
    }
}

$winPeDiskPartWriterText = Get-Content `
    -LiteralPath $auditedWinPeDiskPartScriptWriterPath `
    -Raw
$winPeDiskPartWriterRequiredPatterns = [ordered]@{
    '\brun_verified_diskpart_script\b' = 'single audited DiskPart writer function'
    '\brun_diskpart_partition_letter_command\b' = 'audited drive-letter command builder'
    '\brun_diskpart_rescan_command\b' = 'separate post-MBR2GPT DiskPart rescan process'
    '\brun_diskpart_create_msr_command\b' = 'audited MSR command builder'
    'trusted_system_directory\.substr\(0U, 3U\) \+ L"Windows\\\\Temp\\\\"' = 'WinPE system-drive temp path'
    '\bFILE_ATTRIBUTE_REPARSE_POINT\b' = 'temp-directory reparse rejection'
    '\bCREATE_NEW\b' = 'create-new temporary script'
    '\bFILE_ATTRIBUTE_TEMPORARY\b' = 'temporary-file attribute'
    'trusted_system_directory \+ L"\\\\diskpart\.exe"' = 'System32 DiskPart path'
    '\bverify_microsoft_signed\s*\(' = 'Microsoft signature verification'
    '\bselect disk\b' = 'explicit disk selection'
    '\bselect partition\b' = 'explicit partition selection'
    '"rescan\\r\\nexit\\r\\n"' = 'completed DiskPart rescan script before a new MSR process'
    '\brefresh_disk_partition_cache\b' = 'target-only system partition-cache refresh'
    '\bIOCTL_DISK_UPDATE_PROPERTIES\b' = 'documented partition-cache invalidation control'
    'expected_path\.c_str\(\),\s*0,\s*FILE_SHARE_READ \| FILE_SHARE_WRITE' = 'cache refresh handle with zero requested disk access'
    '\brefresh_virtual_disk_service_cache\b' = 'post-conversion VDS cache refresh'
    '\bWaitForServiceReady\s*\(' = 'VDS initialization completion wait'
    '\bReenumerate\s*\(' = 'documented VDS disk reenumeration'
    '\bRefresh\s*\(' = 'documented VDS existing-layout cache refresh'
    '\brestart_virtual_disk_service\b' = 'WinPE-only stale VDS provider restart'
    'L"vds"' = 'exact Microsoft Virtual Disk Service name'
    '\bSERVICE_QUERY_STATUS \| SERVICE_START \| SERVICE_STOP\b' = 'minimal VDS service control access'
    '\bSERVICE_CONTROL_STOP\b' = 'bounded stale VDS provider stop'
    '\bStartServiceW\s*\(' = 'bounded VDS provider restart'
    'create partition msr size=16 offset=' = 'fixed 16 MiB MSR creation command'
    '\bselect_msr_creation_offset\b' = 'bounded MSR free-space selection'
    'partition\.offset_bytes == created_msr_offset\.value\(\)' = 'MSR starting-offset readback'
    'partition\.size_bytes ==\s*kMicrosoftReservedPartitionBytes' = 'MSR size readback'
    '\bverify_direct_partition_handle\s*\(' = 'physical partition readback'
}
foreach ($pattern in $winPeDiskPartWriterRequiredPatterns.Keys) {
    if ($winPeDiskPartWriterText -notmatch $pattern) {
        $failures += "WinPEApp\src\mbr2gpt_direct_execution_service.cpp must retain $($winPeDiskPartWriterRequiredPatterns[$pattern])"
    }
}

$winPeDiskPartWriteFileCount = ([regex]::Matches(
        $winPeDiskPartWriterText,
        '\bWriteFile\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$winPeDiskPartGenericWriteCount = ([regex]::Matches(
        $winPeDiskPartWriterText,
        '\bGENERIC_WRITE\b',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$winPeDiskPartDeleteFileCount = ([regex]::Matches(
        $winPeDiskPartWriterText,
        '\bDeleteFileW\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
if ($winPeDiskPartWriteFileCount -ne 1 -or
    $winPeDiskPartGenericWriteCount -ne 1 -or
    $winPeDiskPartDeleteFileCount -ne 2) {
    $failures += 'WinPEApp MBR2GPT service may contain exactly one WriteFile, one GENERIC_WRITE, and two DeleteFileW calls for the audited RAM-disk script'
}

$rescueMediaBuilderPath = Join-Path $repoRoot `
    'scripts\New-WinPEAppValidationMedia.ps1'
$rescueMediaBuilderText = Get-Content `
    -LiteralPath $rescueMediaBuilderPath `
    -Raw
$rescueUsbInitializationPatterns = [ordered]@{
    'function Initialize-VerifiedUsbTarget' = 'single target-bound USB initialization function'
    '\$script:RescueUsbMinimumBytes = \[UInt64\]\(8GB\)' = '8 GiB minimum rescue USB gate'
    '\$script:RescueUsbBootPartitionBytes = \[UInt64\]\(4GB\)' = 'exact 4 GiB boot partition contract'
    'function Get-CanonicalUsbLayout' = 'canonical full partition layout observation'
    'function Get-CanonicalUsbLayoutDigest' = 'cross-process canonical full-layout digest'
    'function Assert-UsbIdentityAndLayout' = 'stable identity plus full-layout destructive seam gate'
    '\bGet-VerifiedUsbDisk\b' = 'stable USB identity recheck before and after clearing'
    'Clear-Disk\s+`\s*-InputObject \$before\.disk' = 'clear only the already verified USB object'
    'Initialize-Disk\s+`\s*-InputObject \$cleared\.disk\s+`\s*-PartitionStyle MBR' = 'initialize only the reverified RAW USB object as MBR'
    'Set-Disk\s+`\s*-InputObject \$cleared\.disk\s+`\s*-PartitionStyle MBR' = 'convert only the empty reverified GPT USB object to MBR'
    '\$cleared\.partitions\.Count -ne 0' = 'require zero partitions after clearing before any style operation'
    '-SizeBytes \$script:RescueUsbBootPartitionBytes' = 'create the exact 4 GiB active boot partition'
    '-UseMaximumSize' = 'assign all remaining capacity to the data partition'
    'function Format-RescueUsbPartition' = 'single audited dynamic filesystem formatter'
    '-FileSystem FAT32' = 'format the boot partition only as FAT32'
    '-FileSystem \$DataFileSystem' = 'format the remaining partition as explicitly selected NTFS or exFAT'
    '-RemoveData\s+`\s*-RemoveOEM\s+`\s*-Confirm:\$false' = 'explicit whole-target erase after confirmation'
    'function Get-VerifiedOwnedUsbMedia' = 'verified Y-TEC ownership and exact boot-tree gate'
    'function New-RescueUsbOwnershipManifest' = 'bounded boot-only ownership manifest'
    'function Invoke-RescueUsbBootUpdate' = 'non-overwrite staging and verified cutover'
    'ExpectedUsbCanonicalLayoutSha256' = 'reviewed C++ layout binding at the PowerShell boundary'
    'DATA_TREE_SCAN_FAILED' = 'fixed privacy-preserving data scan error'
    'dataPreservation = \[ordered\]@\{' = 'count and total only data-preservation report'
}
foreach ($pattern in $rescueUsbInitializationPatterns.Keys) {
    if ($rescueMediaBuilderText -notmatch $pattern) {
        $failures += "New-WinPEAppValidationMedia.ps1 must retain $($rescueUsbInitializationPatterns[$pattern])"
    }
}
$rescueMediaTokens = $null
$rescueMediaParseErrors = $null
$rescueMediaBuilderAst =
    [Management.Automation.Language.Parser]::ParseFile(
        $rescueMediaBuilderPath,
        [ref]$rescueMediaTokens,
        [ref]$rescueMediaParseErrors)
foreach ($singleWriter in @(
        'Clear-Disk',
        'Initialize-Disk',
        'Set-Disk',
        'New-Partition',
        'Format-Volume')) {
    $writerCount = @($rescueMediaBuilderAst.FindAll(
        {
            param($node)
            $node -is [Management.Automation.Language.CommandAst] -and
                $node.GetCommandName() -eq $singleWriter
        },
        $true)).Count
    if ($writerCount -ne 1) {
        $failures += "New-WinPEAppValidationMedia.ps1 may contain exactly one $singleWriter USB writer"
    }
}
if ($rescueMediaBuilderText -match '(?i)/UFD\s+/F') {
    $failures += 'New-WinPEAppValidationMedia.ps1 must not use destructive MakeWinPEMedia /UFD'
}

$productMbr2GptVerifierPath = Join-Path $repoRoot `
    'tests\VM\product_mbr2gpt_verifier_vm.cpp'
$productMbr2GptVerifierText = Get-Content `
    -LiteralPath $productMbr2GptVerifierPath -Raw
$productMbr2GptVerifierRequiredPatterns = [ordered]@{
    'YTEC_VM_DESTRUCTIVE_TEST_ONLY' = 'destructive VM-only compile guard'
    'kTargetBytes' = 'fixed 64 GiB target identity'
    'YDC_TARGET_PROBE_STAGED_V1' = 'probe staging completion marker'
    'YDC_TARGET_SECURE_BOOT_PASS_V1' = 'target startup PASS marker'
    'program_data = windows_root \+ L"ProgramData\\\\' = 'fixed target ProgramData root'
    'Microsoft\\\\Windows\\\\Start Menu\\\\Programs\\\\Startup' = 'fixed target-only startup directory'
    '%ProgramData%\\\\YdcMbr2GptValidation\.ps1' = 'non-Startup probe script location'
    '\bCREATE_NEW\b' = 'non-overwriting probe file creation'
    '\bquery_windows_volume_bitmap_bindings\b' = 'physical target volume mapping'
    '\bopen_verified_read_only_physical_disk_with_windows_apis\b' = 'reverified target disk identity'
}
foreach ($pattern in $productMbr2GptVerifierRequiredPatterns.Keys) {
    if ($productMbr2GptVerifierText -notmatch $pattern) {
        $failures += "product_mbr2gpt_verifier_vm.cpp must retain $($productMbr2GptVerifierRequiredPatterns[$pattern])"
    }
}

# EXE-005: persistent exact/rescue whole-disk restore resumes only through the
# audited abstract target writer and SingleResumeSlot boundaries. The durable
# cursor describes authenticated payload segments after delayed GPT/MBR
# metadata is subtracted; neither this backend nor its synthetic tests may open
# a real physical disk.
$tsumugiResumeEnginePath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'ImageFormat\src\tsumugi_physical_restore_resume.cpp'))
$winPeResumeControllerPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'WinPEApp\src\direct_image_restore_resume.cpp'))
$winPeResumeBackendPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'WinPEApp\src\direct_image_restore_resume_backend.cpp'))
$winPeResumeStoragePath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'WinPEApp\src\direct_image_restore_resume_storage.cpp'))
$winPeResumeStorageHeaderPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'WinPEApp\include\ytec\winpeapp\direct_image_restore_resume_storage.h'))
$winPeResumeGuiPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'WinPEApp\src\gui_main.cpp'))
$winPeImageCreateResumeHeaderPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'WinPEApp\include\ytec\winpeapp\direct_image_create_resume.h'))
$winPeImageCreateResumeControllerPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'WinPEApp\src\direct_image_create_resume.cpp'))
$winPeImageCreateControllerPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'WinPEApp\src\direct_image_create.cpp'))
$winPeImageCreateTestPath = [IO.Path]::GetFullPath(
    (Join-Path $repoRoot 'tests\Unit\winpe_direct_image_create_tests.cpp'))
$tsumugiResumeTestPath = [IO.Path]::GetFullPath(
    (Join-Path $repoRoot 'tests\Unit\tsumugi_physical_restore_resume_tests.cpp'))
$winPeResumeTestPath = [IO.Path]::GetFullPath(
    (Join-Path $repoRoot 'tests\Unit\winpe_direct_image_restore_resume_tests.cpp'))
$tsumugiResumeEngineText = Get-Content `
    -LiteralPath $tsumugiResumeEnginePath `
    -Raw
$winPeResumeControllerText = Get-Content `
    -LiteralPath $winPeResumeControllerPath `
    -Raw
$winPeResumeBackendText = Get-Content `
    -LiteralPath $winPeResumeBackendPath `
    -Raw
$winPeResumeStorageText = Get-Content `
    -LiteralPath $winPeResumeStoragePath `
    -Raw
$winPeResumeStorageHeaderText = Get-Content `
    -LiteralPath $winPeResumeStorageHeaderPath `
    -Raw
$winPeResumeGuiText = Get-Content `
    -LiteralPath $winPeResumeGuiPath `
    -Raw
$winPeImageCreateResumeText =
    (Get-Content -LiteralPath $winPeImageCreateResumeHeaderPath -Raw) + "`n" +
    (Get-Content -LiteralPath $winPeImageCreateResumeControllerPath -Raw) + "`n" +
    (Get-Content -LiteralPath $winPeImageCreateControllerPath -Raw)
$winPeImageCreateTestText = Get-Content `
    -LiteralPath $winPeImageCreateTestPath `
    -Raw
$tsumugiResumeTestText = Get-Content `
    -LiteralPath $tsumugiResumeTestPath `
    -Raw
$winPeResumeTestText = Get-Content `
    -LiteralPath $winPeResumeTestPath `
    -Raw

$tsumugiResumeEngineRequiredPatterns = [ordered]@{
    '\bmake_tsumugi_physical_resume_payload_segments_v1\s*\(' = 'metadata-subtracted authenticated payload mapping'
    '\bvalidate_whole_disk_mode_and_loss_evidence\s*\(' = 'exact/rescue mode and partial-loss evidence gate'
    '\bresume_prepared\s*\(' = 'restart-only withheld metadata inspection'
    '\bsame_inspection\s*\(' = 'opened image identity, size, time, and authenticated container recheck'
    '\bvalidate_nonce_sequence\s*\(' = 'nonce replay and counter rejection'
    '\brevalidate_locked_target\s*\(' = 'connection-bound target reidentification'
    '\btarget\.write_target\s*\(' = 'abstract target payload write only'
    '\btarget\.flush_target\s*\(' = 'per-block target flush'
    '\btarget\.read_back\s*\(' = 'same-handle prefix and suffix verification'
    '\bcheckpoint_commit\s*\(\s*next_segment\s*,\s*segment\s*\)' = 'post-readback monotonic segment checkpoint'
    '\blayout_transaction\.commit\s*\(' = 'layout publication after complete payload verification'
    '\.read_verified_image\s*=\s*read_verified_tsumugi_file_v1' = 'production strict two-pass image reader binding'
    'target_identity\.logical_sector_size\s*!=\s*512U' = 'explicit 4Kn exclusion'
}
foreach ($pattern in $tsumugiResumeEngineRequiredPatterns.Keys) {
    if ($tsumugiResumeEngineText -notmatch $pattern) {
        $failures += "ImageFormat persistent resume engine must retain $($tsumugiResumeEngineRequiredPatterns[$pattern])"
    }
}

$winPeResumeControllerRequiredPatterns = [ordered]@{
    '\bSingleResumeSlot\s+slot\s*\(' = 'single persistent resume slot controller'
    '\bhash_immutable_payload\s*\(' = 'authenticated segment-boundary plan material'
    '\bauthenticated_unreadable_ranges\s*\(' = 'fully verified rescue bad-range reconstruction'
    'append_u64\(bytes,\s*static_cast<std::uint64_t>\(unreadable_ranges\.size\(\)\)\)' = 'bad-range count in immutable plan material'
    'report\.partial_loss\s*!=\s*derived\.value\(\)\.partial_loss' = 'partial-loss completion classification proof'
    'storage\.checkpoint_storage_identity_hash' = 'checkpoint backing identity in immutable plan material'
    'storage\.image_storage_identity_hash' = 'image backing identity in immutable plan material'
    'storage\.target_storage_identity_hash' = 'target backing identity in immutable plan material'
    'storage\.active_rescue_storage_identity_hash' = 'active rescue backing identity in immutable plan material'
    'proof\.image_storage_identity_hash\s*==[\s\S]*?proof\.target_storage_identity_hash' = 'image backing and target storage separation gate'
    'proof\.target_storage_identity_hash\s*==[\s\S]*?proof\.active_rescue_storage_identity_hash' = 'target and active rescue backing separation gate'
    '\bvalidate_verified_prefix\s*\(' = 'contiguous verified segment prefix validation'
    '\bzero_cursor_on_reviewed_initial\b' = 'zero-cursor reviewed-layout retry gate'
    '\bexact_incomplete_layout\b' = 'operation-bound incomplete-layout resume gate'
    '\bslot\.replace\s*\(' = 'atomic checkpoint advance only after readback callback'
    '\bslot\.discard\s*\(' = 'binding-checked cleanup after mandatory success proof'
    '\bformat_direct_image_restore_resume_startup_review_v1\s*\(' = 'bounded path-free startup review model'
    'evidence\.value\(\)\.image_file_object_identity_hash\s*!=' = 'independent immutable image-handle binding'
    'evidence\.value\(\)\.target_storage_identity_hash\s*!=' = 'independent locked-target storage binding'
}

$winPeResumeStorageRequiredPatterns = [ordered]@{
    '\bmake_direct_image_restore_resume_storage_platform_v1\s*\(' = 'shared data/selection proof binding'
    '\bmake_direct_image_restore_windows_storage_platform_v1\s*\(' = 'production opened-storage factory'
    '\bquery_active_rescue_media_storage_with_windows_apis\s*\(' = 'active rescue marker storage observer'
    '\bquery_single_disk_number_for_local_path\s*\(' = 'opened local volume to single physical disk mapping'
    '\bopen_verified_read_only_physical_disk_with_windows_apis\s*\(' = 'raw physical domain reidentification'
    '\bhash_tsumugi_physical_restore_target_identity_v1\s*\(' = 'common target/storage-domain normalization'
    '\bhash_opened_image_file_object\s*\(' = 'selected image file-object identity'
    '\bGetFinalPathNameByHandleW\s*\(' = 'opened path reidentification'
    '\bmounted_volume_guid_for_path\s*\(' = 'held file volume and mount-point binding'
    'opened/mounted volume recheck' = 'post-mapping mount substitution rejection'
    '\bFileIdInfo\b' = 'opened image File ID observation'
    '\bFileStandardInfo\b' = 'opened image size observation'
    '\bFileBasicInfo\b' = 'opened image last-write observation'
    'persistent_checkpoint_backing\s*&&' = 'nonpersistent X or CDROM slot-mutation rejection'
    '!data\.value\(\)\.persistent_checkpoint_backing' = 'nonpersistent data rejection before image or target observation'
    '\bquery_active_rescue_media_storage_with_windows_apis\s*\(\)[\s\S]*?\bquery_active_rescue_media_storage_with_windows_apis\s*\(\)' = 'active rescue marker and storage recheck around domain observation'
    'data\.value\(\)\.storage_identity_hash\s*!=[\s\S]*?image_storage_identity_hash' = 'checkpoint/image storage separation'
    'data\.value\(\)\.storage_identity_hash\s*!=[\s\S]*?target_storage_identity_hash' = 'checkpoint/target storage separation'
}
foreach ($pattern in $winPeResumeStorageRequiredPatterns.Keys) {
    if (($winPeResumeStorageText + "`n" + $winPeResumeStorageHeaderText) -notmatch $pattern) {
        $failures += "WinPE persistent resume storage platform must retain $($winPeResumeStorageRequiredPatterns[$pattern])"
    }
}

$winPeResumeGuiRequiredPatterns = [ordered]@{
    '\binspect_restore_resume_on_startup\s*\(' = 'startup fixed-slot inspection'
    '\bformat_direct_image_restore_resume_startup_review_v1\s*\(' = 'bounded public summary rendering'
    'MB_YESNOCANCEL\s*\|\s*MB_ICONWARNING\s*\|\s*MB_DEFBUTTON3' = 'safe-default resume/discard/keep prompt'
    'MB_YESNO\s*\|\s*MB_ICONWARNING\s*\|\s*MB_DEFBUTTON2' = 'second discard confirmation'
    '\.action\s*=\s*ytec::winpeapp::[\s\S]*?DirectImageRestoreResumeAction::inspect_only' = 'startup inspect controller action'
    'DirectImageRestoreResumeAction::discard_existing' = 'owned binding-checked discard action'
    '\.reviewed_existing_slot\s*=\s*inspected\.value\(\)\.existing_slot' = 'displayed slot binding passed to discard'
    '\bmake_direct_image_restore_windows_storage_platform_v1\s*\(' = 'production four-storage proof factory'
    '\bmake_current_executable_windows_resume_slot_platform\s*\(' = 'EXE-adjacent data fixed-slot factory'
    '\bmake_direct_image_restore_windows_resume_backend_v1\s*\(' = 'production persistent transfer backend'
    '\bcontrol_direct_image_restore_resume_v1\s*\(' = 'product persistent resume controller wiring'
    'DirectImageRestoreResumeAction::[\s\S]*?resume_existing' = 'reselection then resume action'
    'DirectImageRestoreResumeAction::start_new' = 'new persistent exact/rescue action'
}
foreach ($pattern in $winPeResumeGuiRequiredPatterns.Keys) {
    if ($winPeResumeGuiText -notmatch $pattern) {
        $failures += "WinPE persistent resume GUI must retain $($winPeResumeGuiRequiredPatterns[$pattern])"
    }
}

$winPeImageCreateResumeRequiredPatterns = [ordered]@{
    'kMaximumContinuityCharacters\s*=\s*512U' = 'bounded canonical non-secret continuity metadata'
    '\bparse_direct_image_create_resume_continuity_v1\s*\(' = 'strict continuity parser'
    '\bprepare_resumable_tsumugi_file_v1\s*\(' = 'private journal-backed resumable stream'
    '\bslot\.create\s*\(' = 'schema-v3 slot publication before first owned-object byte'
    '\bslot\.replace\s*\(' = 'verified-boundary atomic checkpoint replacement'
    '\.prove_existing_before_resume\s*=' = 'existing prefix full source and journal reproof hook'
    '\bcommit_persistent_pe_exact_image_create\s*\(' = 'no-overwrite publish, full verification, and exact retirement'
    'continuity\.created_utc\s*=\s*request\.created_utc' = 'first-run timestamp continuity'
    'effective\.created_utc\s*=\s*continuity\.created_utc' = 'resume reuses the initial timestamp'
    'request\.rescue_mode\s*\|\|\s*request\.replace_existing' = 'persistent path remains exact and create-new only'
}
foreach ($pattern in $winPeImageCreateResumeRequiredPatterns.Keys) {
    if ($winPeImageCreateResumeText -notmatch $pattern) {
        $failures += "WinPE persistent exact image-create controller must retain $($winPeImageCreateResumeRequiredPatterns[$pattern])"
    }
}

$winPeImageCreateResumeGuiRequiredPatterns = [ordered]@{
    '\binspect_image_create_resume_on_startup\s*\(' = 'capability-8 startup routing before restore delegation'
    '\bformat_direct_image_create_resume_startup_review_v1\s*\(' = 'source, output, progress, capability startup summary'
    '\bdiscard_direct_image_create_resume_v1\s*\(' = 'staged-only exact-bound two-object discard'
    '\bexecute_direct_image_create_resume_with_windows_apis_v1\s*\(' = 'product exact start/resume executor'
    'DirectImageCreateResumeAction::resume_existing' = 'reviewed binding resume action'
    'DirectImageCreateResumeAction::start_new' = 'new schema-v3 exact action'
    'パスワードと鍵はcheckpointへ保存していません' = 'password re-entry and non-persistence disclosure'
    'PersistentPeExactImageCreateObjectState::retirement_pending' = 'post-publish retirement recovery routing'
}
foreach ($pattern in $winPeImageCreateResumeGuiRequiredPatterns.Keys) {
    if ($winPeResumeGuiText -notmatch $pattern) {
        $failures += "WinPE persistent exact image-create GUI must retain $($winPeImageCreateResumeGuiRequiredPatterns[$pattern])"
    }
}

$winPeImageCreateResumeTestRequiredPatterns = [ordered]@{
    '\bresume_continuity_roundtrips_without_secret_material\s*\(' = 'encrypted and unencrypted canonical continuity roundtrip'
    '\bresume_continuity_rejects_noncanonical_unknown_and_oversized_input\s*\(' = 'unknown version, noncanonical, secret-shaped, and oversize rejection'
}
foreach ($pattern in $winPeImageCreateResumeTestRequiredPatterns.Keys) {
    if ($winPeImageCreateTestText -notmatch $pattern) {
        $failures += "WinPE persistent exact image-create tests must retain $($winPeImageCreateResumeTestRequiredPatterns[$pattern])"
    }
}
foreach ($pattern in $winPeResumeControllerRequiredPatterns.Keys) {
    if ($winPeResumeControllerText -notmatch $pattern) {
        $failures += "WinPE persistent resume controller must retain $($winPeResumeControllerRequiredPatterns[$pattern])"
    }
}

$winPeResumeBackendRequiredPatterns = [ordered]@{
    '\bphysical_\.physical\.verify_image\s*\(' = 'full image verification before target mutation'
    '\bhash_opened_image_identity\s*\(' = 'opened image file-object binding'
    '\bvalidate_source_target_and_geometry\s*\(' = 'source, target, layout, and sector gate'
    'target_class\.usb_attached\s*&&\s*target\.target\.serial_suffix\.empty\(\)' = 'serialless fixed USB persistent-resume rejection'
    '\bmake_connection_token\s*\(' = 'USB connection-session binding'
    '\bverify_tsumugi_physical_resume_layout_withheld_v1\s*\(' = 'incomplete layout read-only proof'
    '\bmake_tsumugi_physical_resume_payload_segments_v1\s*\(' = 'authenticated payload mapping construction'
    '\bexecute_tsumugi_physical_whole_disk_resume_engine_v1\s*\(' = 'low-level persistent transfer engine'
    '\bERROR_UNHANDLED_EXCEPTION\b' = 'exception-to-failure checkpoint retention path'
    '\bmake_direct_image_restore_windows_resume_backend_v1\s*\(' = 'production Windows dependency factory'
    'const bool exact[\s\S]*?TsumugiManifestMode::exact[\s\S]*?TsumugiPayloadKind::exact_disk' = 'exact whole-disk product gate'
    'const bool rescue[\s\S]*?TsumugiManifestMode::rescue[\s\S]*?TsumugiPayloadKind::rescue_disk' = 'rescue whole-disk product gate'
    '\(exact\s*&&[\s\S]*?image\.partial_loss[\s\S]*?image\.unreadable_ranges\.empty\(\)\)' = 'exact result cannot carry rescue loss evidence'
    'target\.target_identity\.logical_sector_size\s*!=\s*512U' = '512-byte logical-sector product gate'
    'executed[\s\S]*?set_target_offline\s*\(' = 'mandatory final offline reassertion'
}
foreach ($pattern in $winPeResumeBackendRequiredPatterns.Keys) {
    if ($winPeResumeBackendText -notmatch $pattern) {
        $failures += "WinPE persistent resume backend must retain $($winPeResumeBackendRequiredPatterns[$pattern])"
    }
}

$persistentResumeForbiddenPatterns = [ordered]@{
    '\\\\.\\PhysicalDrive' = 'real physical disk path'
    '\bCreateFileW\s*\(' = 'direct file or disk open'
    '\bWriteFile\s*\(' = 'direct Win32 write'
    '\bDeviceIoControl\s*\(' = 'direct disk or volume control I/O'
    '\bGENERIC_WRITE\b' = 'direct write-capable Win32 handle'
}
foreach ($pathAndText in @(
        @($tsumugiResumeEnginePath, $tsumugiResumeEngineText),
        @($winPeResumeControllerPath, $winPeResumeControllerText),
        @($winPeResumeBackendPath, $winPeResumeBackendText),
        @($tsumugiResumeTestPath, $tsumugiResumeTestText),
        @($winPeResumeTestPath, $winPeResumeTestText))) {
    foreach ($pattern in $persistentResumeForbiddenPatterns.Keys) {
        if ($pathAndText[1] -match $pattern) {
            $relative = $pathAndText[0].Substring($repoRoot.Length).TrimStart('\')
            $failures += "$relative contains forbidden $($persistentResumeForbiddenPatterns[$pattern])"
        }
    }
}

$tsumugiResumeTestRequiredPatterns = [ordered]@{
    '\bmetadata_ranges_are_removed_from_authenticated_cursor\s*\(' = 'delayed metadata mapping coverage'
    '\brestart_verifies_prefix_without_rewrite_and_commits_last\s*\(' = 'restart prefix and final commit ordering coverage'
    '\binvalidated_zero_cursor_resumes_without_repeating_prepare\s*\(' = 'post-invalidation zero-cursor restart coverage'
    '\bpreparing_wal_recovers_each_original_or_zero_sector_only\s*\(' = 'per-sector original-or-zero WAL interruption coverage'
    '\bgpt_commit_ready_restart_recovers_every_known_publication_window\s*\(' = 'known GPT publication-prefix recovery coverage'
    '\bcommit_ready_foreign_metadata_is_read_only_rejected\s*\(' = 'foreign publication metadata zero-write rejection coverage'
    '\bprefix_tamper_and_handle_change_fail_before_suffix_write\s*\(' = 'target tamper and opened-image drift coverage'
    '\brescue_zero_filled_prefix_is_reverified_and_never_upgraded\s*\(' = 'rescue zero-prefix revalidation and partial-loss coverage'
    '\bmode_and_partial_loss_evidence_are_rejected_before_io\s*\(' = 'mode and loss-evidence mismatch zero-I/O coverage'
    '\bnonce_cursor_and_4kn_are_fail_closed\s*\(' = 'nonce, cursor overflow, and 4Kn exclusion coverage'
    '\bfull_verification_failure_performs_zero_target_io\s*\(' = 'complete verification failure zero-target-I/O coverage'
}
foreach ($pattern in $tsumugiResumeTestRequiredPatterns.Keys) {
    if ($tsumugiResumeTestText -notmatch $pattern) {
        $failures += "ImageFormat persistent resume tests must retain $($tsumugiResumeTestRequiredPatterns[$pattern])"
    }
}
$winPeResumeTestRequiredPatterns = [ordered]@{
    '\bproduction_backend_verify_failure_performs_zero_target_calls\s*\(' = 'production factory verify-before-target coverage'
    '\bproduction_backend_serialless_fixed_usb_is_reselection_required\s*\(' = 'serialless fixed USB zero-write rejection coverage'
    '\bzero_cursor_can_retry_initial_layout_but_positive_cursor_cannot\s*\(' = 'zero versus positive cursor layout-state coverage'
    '\bauthenticated_segment_boundaries_cannot_change_on_resume\s*\(' = 'durable mapping-boundary authentication coverage'
    '\brescue_partial_loss_resumes_without_classification_upgrade\s*\(' = 'persistent rescue partial-loss result coverage'
    '\brescue_bad_range_substitution_is_rejected_before_transfer\s*\(' = 'authenticated bad-range replacement rejection coverage'
    '\boperation_target_and_completed_checkpoint_cleanup_are_bound\s*\(' = 'operation-bound cleanup failure coverage'
    '\bcheckpoint_storage_identity_is_bound_across_restart\s*\(' = 'persistent storage identity replacement coverage'
    '\blegacy_partial_invalidation_state_cannot_upgrade_to_v2\s*\(' = 'v1 partial invalidation zero-transfer rejection coverage'
    '\bproduction_storage_composition_binds_slot_after_four_opened_domains\s*\(' = 'data image target active opened-domain composition coverage'
    '\bstartup_review_is_bounded_and_path_free\s*\(' = 'bounded startup UI summary coverage'
    '\bram_backed_checkpoint_never_authorizes_persistent_slot_mutation\s*\(' = 'ISO X RAM persistent-resume rejection coverage'
}
foreach ($pattern in $winPeResumeTestRequiredPatterns.Keys) {
    if ($winPeResumeTestText -notmatch $pattern) {
        $failures += "WinPE persistent resume tests must retain $($winPeResumeTestRequiredPatterns[$pattern])"
    }
}

# SAFE-007: completion power actions stay behind one audited CloneCore adapter.
# WindowsApp may query that adapter and dispatch only after a verified worker
# release, an operation-bound selection, and an immediate second confirmation.
$completionPowerCorePath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'CloneCore\src\completion_power_action.cpp'))
$completionPowerMainPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'WindowsApp\src\main.cpp'))
$completionPowerUiPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'WindowsApp\src\completion_power_ui.cpp'))
$completionPowerUiTestPath = [IO.Path]::GetFullPath(
    (Join-Path $repoRoot 'tests\Unit\windows_completion_power_ui_tests.cpp'))
$completionPowerCoreText = Get-Content `
    -LiteralPath $completionPowerCorePath `
    -Raw
$completionPowerMainText = Get-Content `
    -LiteralPath $completionPowerMainPath `
    -Raw
$completionPowerUiText = Get-Content `
    -LiteralPath $completionPowerUiPath `
    -Raw
$completionPowerUiTestText = Get-Content `
    -LiteralPath $completionPowerUiTestPath `
    -Raw

$completionPowerRealApiPatterns = @(
    '\bSetSuspendState\s*\('
    '\bInitiateSystemShutdownExW\s*\('
)
foreach ($pattern in $completionPowerRealApiPatterns) {
    $matches = $sourceFiles |
        Select-String -Pattern $pattern -CaseSensitive
    foreach ($match in $matches) {
        $matchedPath = [IO.Path]::GetFullPath($match.Path)
        if (-not $matchedPath.Equals(
                $completionPowerCorePath,
                [StringComparison]::OrdinalIgnoreCase)) {
            $relative = $match.Path.Substring($repoRoot.Length).TrimStart('\')
            $failures += "$relative`:$($match.LineNumber) contains a completion power API outside the audited CloneCore adapter"
        }
    }
}
$completionPowerSleepApiCount = ([regex]::Matches(
        $completionPowerCoreText,
        '\bSetSuspendState\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$completionPowerShutdownApiCount = ([regex]::Matches(
        $completionPowerCoreText,
        '\bInitiateSystemShutdownExW\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
if ($completionPowerSleepApiCount -ne 1 -or
    $completionPowerShutdownApiCount -ne 1) {
    $failures += 'CloneCore completion power adapter must retain exactly one sleep API call and one restart/shutdown API call'
}

$completionPowerMainRequiredPatterns = [ordered]@{
    '\bclass\s+ThreadSleepPrevention\s+final\b' = 'worker sleep-prevention owner'
    '\bSleepPreventionReleaseState\s+release\s*\(' = 'explicit worker release result'
    '\brelease_attempted_\b' = 'single-attempt release state'
    '\bSleepPreventionReleaseState::release_failed\b' = 'failed release classification'
    'payload->sleep_prevention_release\s*=\s*sleep_prevention\.release\s*\(' = 'release proof published by each worker'
    '\bmake_windows_completion_power_platform\s*\(' = 'single production adapter factory seam'
    '\bquery_completion_power_availability\s*\(' = 'pre-prompt capability query'
    '\bmake_windows_completion_power_execution_request\s*\(' = 'operation-bound execution request'
    '\bexecute_completion_power_action\s*\(' = 'central core execution gate'
    'nDefaultRadioButton\s*=\s*kCompletionPowerNoneId' = 'default no-action radio selection'
    '\bTDF_ALLOW_DIALOG_CANCELLATION\b' = 'Esc-capable native selection dialog'
    'pressed_button\s*!=\s*IDOK' = 'selection cancellation fail-closed branch'
    'MB_YESNO\s*\|\s*MB_ICONWARNING\s*\|\s*MB_DEFBUTTON2' = 'second confirmation with safe default'
    'confirmed\s*!=\s*IDYES' = 'second confirmation fail-closed branch'
    '\bcompletion_power_action_expects_ui_session_end\s*\(' = 'sleep-resume UI continuation classification'
    '\brestore_error_dialog_focus\s*\(' = 'focus restoration after native dialogs'
}
foreach ($pattern in $completionPowerMainRequiredPatterns.Keys) {
    if ($completionPowerMainText -notmatch $pattern) {
        $failures += "WindowsApp completion power UI must retain $($completionPowerMainRequiredPatterns[$pattern])"
    }
}
$completionPowerWorkerCount = ([regex]::Matches(
        $completionPowerMainText,
        '\bThreadSleepPrevention\s+sleep_prevention\s*;',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$completionPowerReleaseCount = ([regex]::Matches(
        $completionPowerMainText,
        '\bsleep_prevention\.release\s*\(\s*\)',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$completionPowerFactoryCount = ([regex]::Matches(
        $completionPowerMainText,
        '\bmake_windows_completion_power_platform\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$completionPowerExecuteCount = ([regex]::Matches(
        $completionPowerMainText,
        '\bexecute_completion_power_action\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
if ($completionPowerWorkerCount -ne 6 -or
    $completionPowerReleaseCount -ne 6) {
    $failures += 'WindowsApp completion power proof must retain exactly six guarded workers and six explicit release results, including direct shrink'
}
if ($completionPowerFactoryCount -ne 1 -or
    $completionPowerExecuteCount -ne 1) {
    $failures += 'WindowsApp completion power UI must retain one production adapter factory and one central execution call'
}

$completionPowerUiRequiredPatterns = [ordered]@{
    '\bplan\.schema_version\s*==\s*operationcore::kOperationPlanSchemaVersion\b' = 'current operation plan schema gate'
    '!all_zero\(plan\.operation_id\)' = 'non-zero operation identity gate'
    '!all_zero\(plan\.immutable_payload_hash\)' = 'non-zero immutable plan hash gate'
    '!all_zero\(lifecycle\.plan_hash\)' = 'non-zero lifecycle plan hash gate'
    '\boperation_binding\s*!=\s*0U\b' = 'non-zero UI operation binding gate'
    '\bSleepPreventionReleaseState::released\b' = 'proven sleep-prevention release gate'
    '\bsnapshot_backup_completed\b' = 'clone snapshot backup completion gate'
    '\bsnapshots_deleted\b' = 'snapshot cleanup gate'
    '\btarget_left_offline\b' = 'offline target completion gate'
    '\bmake_direct_shrink_clone_completion_power_proof\s*\(' = 'direct shrink clone mandatory verification proof'
    '\bhas_valid_windows_direct_shrink_precomputed_completion_evidence\s*\(' = 'allocation-free canonical completion outcome binding gate'
    '\bprimary_layout_committed_last\b' = 'direct shrink final GPT commit-last gate'
    '\bsource_reidentified\b' = 'direct shrink final source re-identification gate'
    '\bsource_layout_unchanged\b' = 'direct shrink final source layout immutability gate'
    '\bconstruction_layout_non_bootable\b' = 'direct shrink pre-publication non-bootable GPT gate'
    '\bcheckpoint_retained_through_extensions_and_boot\b' = 'direct shrink durable incomplete marker gate'
    '\bboot_completed_before_final_layout_publication\b' = 'direct shrink boot-before-visible-GPT gate'
    '\bfinal_layout_published_before_checkpoint_retirement\b' = 'direct shrink publish-before-cleanup gate'
    '\bcheckpoint_retirement_pending\b' = 'verified final layout with honest cleanup-pending evidence'
    '\bfinal_partition_style\b' = 'direct shrink final GPT or MBR style gate'
    '\bsource_mbr_bootstrap_unchanged\b' = 'direct shrink source MBR bootstrap immutability gate'
    '\bfinal_mbr_sector0_read_back_verified\b' = 'direct shrink final MBR sector0 readback gate'
    '\bevery_payload_captured_and_applied_inside_snapshot_callback\b' = 'direct shrink callback-bound payload gate'
    '\bbackup_completed\b' = 'VSS BackupComplete gate'
    '\bselected_tsumugi_creation_verification_passed\b' =
        'complete-or-fast image verification evidence gate'
    '\bfinal_file_committed_after_vss\b' = 'post-VSS image publication gate'
    '\binitial_image_verification_completed\b' = 'initial exact-restore verification gate'
    '\btarget_handle_reidentified\b' = 'exact target handle re-identification gate'
    '\ball_writes_read_back_verified\b' = 'exact restore read-back gate'
    '\bimage_completely_reverified\b' = 'shrink image re-verification gate'
    '\bwork_placement_reidentified_before_write\b' = 'shrink work-placement gate'
    '\ball_payloads_verified_by_adapter\b' = 'shrink payload verification gate'
    '\bfinal_layout_committed\b' = 'restore layout commit gate'
    '\bcomplete_usb_verified\b' = 'complete USB media verification gate'
    '\bcomplete_iso_verified\b' = 'complete ISO verification gate'
    '\bpublished_without_overwrite\b' = 'non-overwriting ISO publication gate'
    '\bCompletionOperationOutcome::partial\b' = 'partial-result exclusion'
    '\bcompletion_power_action_expects_ui_session_end\s*\(' = 'sleep-resume continuation helper'
}
foreach ($pattern in $completionPowerUiRequiredPatterns.Keys) {
    if ($completionPowerUiText -notmatch $pattern) {
        $failures += "WindowsApp completion power proof must retain $($completionPowerUiRequiredPatterns[$pattern])"
    }
}
$completionPowerUiForbiddenPatterns = [ordered]@{
    '\bmake_windows_completion_power_platform\s*\(' = 'production platform factory'
    '\bexecute_completion_power_action\s*\(' = 'power-action execution'
    '\bSetSuspendState\s*\(' = 'real sleep API'
    '\bInitiateSystemShutdownExW\s*\(' = 'real restart/shutdown API'
    '\bSetThreadExecutionState\s*\(' = 'worker sleep-prevention API'
}
foreach ($pattern in $completionPowerUiForbiddenPatterns.Keys) {
    if ($completionPowerUiText -match $pattern) {
        $failures += "WindowsApp pure completion power proof contains forbidden $($completionPowerUiForbiddenPatterns[$pattern])"
    }
}

$completionPowerUiTestRequiredPatterns = [ordered]@{
    '\bclass\s+MockPlatform\s+final\s*:\s*public\s+ICompletionPowerPlatform\b' = 'mock-only platform seam'
    '\brelease_and_binding_fail_closed_before_any_platform_call\s*\(' = 'release and binding fail-closed coverage'
    '\bselection_and_reconfirmation_reach_only_the_mock_seam\s*\(' = 'two-stage confirmation mock coverage'
    '\bonly_restart_and_shutdown_skip_the_continuing_ui_refresh\s*\(' = 'sleep-resume UI refresh coverage'
    '\bdirect_shrink_completion_requires_full_mandatory_evidence\s*\(' = 'direct shrink commit, offline, lifecycle, release, and binding proof coverage'
    'A verified final layout with only checkpoint cleanup pending must remain eligible' = 'checkpoint-cleanup pending success coverage'
    'Conflicting retired and cleanup-pending evidence must fail closed' = 'exclusive checkpoint outcome coverage'
    'A zero precomputed completion hash must fail the power prompt closed' = 'zero canonical completion hash rejection coverage'
    'A completion hash for the opposite checkpoint outcome must fail the power prompt closed' = 'opposite cleanup outcome hash rejection coverage'
    'pre-publication construction layout was non-bootable' = 'non-boot construction negative coverage'
    'finish boot preparation before final GPT publication' = 'boot-before-publication negative coverage'
    'A verified MBR/Legacy BIOS sector0-last report must be eligible' = 'positive MBR completion power proof coverage'
    'MBR completion without unchanged source bootstrap must fail closed' = 'negative MBR completion power proof coverage'
}
foreach ($pattern in $completionPowerUiTestRequiredPatterns.Keys) {
    if ($completionPowerUiTestText -notmatch $pattern) {
        $failures += "WindowsApp completion power tests must retain $($completionPowerUiTestRequiredPatterns[$pattern])"
    }
}
$completionPowerUiTestForbiddenPatterns = [ordered]@{
    '\bmake_windows_completion_power_platform\s*\(' = 'production platform factory'
    '\bSetSuspendState\s*\(' = 'real sleep API'
    '\bInitiateSystemShutdownExW\s*\(' = 'real restart/shutdown API'
    '\bSetThreadExecutionState\s*\(' = 'worker sleep-prevention API'
}
foreach ($pattern in $completionPowerUiTestForbiddenPatterns.Keys) {
    if ($completionPowerUiTestText -match $pattern) {
        $failures += "WindowsApp completion power tests contain forbidden $($completionPowerUiTestForbiddenPatterns[$pattern])"
    }
}

# UI-001/DAT-001: the first-run acknowledgement is one fixed bounded binary
# setting, stored only in the verified current-EXE adjacent existing data
# directory. This is a separate audited writer with no fallback or migration.
$firstRunGuidanceHeaderPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'WindowsApp\include\ytec\windowsapp\first_run_guidance.h'))
$firstRunGuidanceMainPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'WindowsApp\src\main.cpp'))
$firstRunGuidanceTestPath = [IO.Path]::GetFullPath(
    (Join-Path $repoRoot 'tests\Unit\windows_first_run_guidance_tests.cpp'))
$firstRunGuidanceHeaderText = Get-Content `
    -LiteralPath $firstRunGuidanceHeaderPath `
    -Raw
$firstRunGuidanceWriterText = Get-Content `
    -LiteralPath $auditedFirstRunGuidanceWriterPath `
    -Raw
$firstRunGuidanceMainText = Get-Content `
    -LiteralPath $firstRunGuidanceMainPath `
    -Raw
$firstRunGuidanceTestText = Get-Content `
    -LiteralPath $firstRunGuidanceTestPath `
    -Raw

$firstRunGuidanceWriterRequiredPatterns = [ordered]@{
    '\bkFirstRunGuidanceMagic\b' = 'fixed binary magic'
    '\bkFirstRunGuidanceSchemaVersion\b' = 'fixed schema version'
    '\bkMaximumSettingsAllocationBytes\b' = 'bounded allocation gate'
    '\bcontains_appdata_component\s*\(' = 'explicit AppData rejection'
    '\bconfigure_paths\s*\(' = 'fixed data and settings path configuration'
    '\bbind_directory_chain\s*\(' = 'full ancestor handle pinning'
    '\bFILE_FLAG_OPEN_REPARSE_POINT\b' = 'reparse-point refusal'
    '\bFileIdInfo\b' = 'File ID reidentification'
    '\bFileStandardInfo\b' = 'link and length reidentification'
    '\bNumberOfLinks\s*!=\s*1U' = 'single-link requirement'
    '\bFILE_ATTRIBUTE_READONLY\b' = 'read-only setting refusal'
    'GENERIC_READ\s*\|\s*DELETE,\s*FILE_SHARE_READ\s*\|\s*FILE_SHARE_DELETE' = 'write-sharing refusal on the fixed setting'
    '\bCREATE_NEW\b' = 'non-overwriting owned stage creation'
    '\bFILE_FLAG_WRITE_THROUGH\b' = 'write-through owned files'
    '\bFlushFileBuffers\s*\(' = 'durability flush'
    '\bread_bounded_file\s*\(\s*stage\.get\s*\(' = 'same-handle stage readback'
    'publish後同一handle全byte読戻し' = 'same-handle post-publish full readback'
    '\breidentify_directory_chain\s*\(' = 'ancestor reidentification around publish'
    '\bReplaceIfExists\s*=\s*FALSE' = 'missing-final non-overwrite publish'
    'rename_no_replace\s*\(\s*current\.handle\.get\s*\(\s*\),\s*bound\.value\(\)\.paths\.backup_path' = 'handle-bound original-to-backup move'
    'rename_no_replace\s*\(\s*stage\.get\s*\(\s*\),\s*bound\.value\(\)\.paths\.final_path' = 'non-overwriting stage publication'
    'rename_no_replace\s*\(\s*current\.handle\.get\s*\(\s*\),\s*bound\.value\(\)\.paths\.final_path' = 'handle-bound original restoration'
    '\bOwnedPathCleanup\b' = 'handle-bound owned temporary cleanup'
    '\binspect_windows_startup_data_backing\s*\(' = 'current executable data observation'
    '\bsame_backing_observation\s*\(' = 'current executable data reobservation'
    '\bdata_directory_exists\b' = 'existing-data-only gate'
}
foreach ($pattern in $firstRunGuidanceWriterRequiredPatterns.Keys) {
    if ($firstRunGuidanceWriterText -notmatch $pattern) {
        $failures += "Windows first-run settings writer must retain $($firstRunGuidanceWriterRequiredPatterns[$pattern])"
    }
}
$firstRunGuidanceWriterForbiddenPatterns = [ordered]@{
    '\bCreateDirectoryW\s*\(' = 'data directory creation'
    '\bCREATE_ALWAYS\b' = 'overwrite creation'
    '\bTRUNCATE_EXISTING\b' = 'truncating open'
    '\bMoveFileExW\s*\(' = 'path-based replacement'
    '\bReplaceFileW\s*\(' = 'path-based replacement with a TOCTOU backup name'
    '\bDeleteFileW\s*\(' = 'path-based deletion'
    '\bSHGetKnownFolderPath\s*\(' = 'known-folder fallback'
    '\bFOLDERID_(Local)?AppData\b' = 'AppData fallback'
    '\bReg(Open|Create|Set|Delete)[A-Za-z0-9_]*\s*\(' = 'registry persistence'
    '\bWinHttp[A-Za-z0-9_]*\s*\(' = 'network access'
    '\bGetEnvironmentVariableW\s*\(' = 'environment-path fallback'
    '\\\\.\\PhysicalDrive' = 'physical disk path'
    '\bDeviceIoControl\s*\(' = 'device control'
}
foreach ($pattern in $firstRunGuidanceWriterForbiddenPatterns.Keys) {
    if ($firstRunGuidanceWriterText -match $pattern) {
        $failures += "Windows first-run settings writer contains forbidden $($firstRunGuidanceWriterForbiddenPatterns[$pattern])"
    }
}
$firstRunGuidanceWriteFileCount = ([regex]::Matches(
        $firstRunGuidanceWriterText,
        '\bWriteFile\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$firstRunGuidanceGenericWriteCount = ([regex]::Matches(
        $firstRunGuidanceWriterText,
        '\bGENERIC_WRITE\b',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$firstRunGuidanceRenameCount = ([regex]::Matches(
        $firstRunGuidanceWriterText,
        '\brename_no_replace\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$firstRunGuidanceDispositionCount = ([regex]::Matches(
        $firstRunGuidanceWriterText,
        '\bSetFileInformationByHandle\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$firstRunGuidanceDeleteAccessCount = ([regex]::Matches(
        $firstRunGuidanceWriterText,
        '\bDELETE\b',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
if ($firstRunGuidanceWriteFileCount -ne 1 -or
    $firstRunGuidanceGenericWriteCount -ne 1 -or
    $firstRunGuidanceRenameCount -ne 6 -or
    $firstRunGuidanceDispositionCount -ne 2 -or
    $firstRunGuidanceDeleteAccessCount -ne 2) {
    $failures += 'Windows first-run settings writer must retain exactly one WriteFile, one GENERIC_WRITE, one rename helper plus five bounded calls, two handle dispositions, and two DELETE accesses'
}

$firstRunGuidanceHeaderRequiredPatterns = [ordered]@{
    'kFirstRunGuidanceSettingsFileName\{\s*L"tsumugi-ui-settings\.bin"\}' = 'fixed owned filename'
    'kFirstRunGuidanceSchemaVersion\s*=\s*1U' = 'schema v1 declaration'
    'kFirstRunGuidanceDocumentBytes\s*=\s*16U' = 'fixed 16-byte document'
    'std::array<FirstRunGuidanceItem,\s*3U>' = 'exactly three shared guidance items'
    'L"対象確認"' = 'target confirmation wording'
    'L"元ディスク保護"' = 'source protection wording'
    'L"検証結果の意味"' = 'verification meaning wording'
    'モデル・容量・接続方式・シリアル末尾' = 'stable target identity wording'
    'L"アプリはコピー元へ直接書き込みません' = 'accurate live-Windows source wording'
    '完全無変更が必要ならレスキューメディア' = 'offline source guidance'
}
foreach ($pattern in $firstRunGuidanceHeaderRequiredPatterns.Keys) {
    if ($firstRunGuidanceHeaderText -notmatch $pattern) {
        $failures += "Windows first-run guidance model must retain $($firstRunGuidanceHeaderRequiredPatterns[$pattern])"
    }
}
if ($firstRunGuidanceHeaderText -match 'コピー元は読み取り専用') {
    $failures += 'Windows first-run guidance must not claim that the live Windows source is read-only'
}

$firstRunGuidanceMainRequiredPatterns = [ordered]@{
    'kFirstRunGuidanceActionId\s*=\s*214' = 'dedicated diagnostics action ID'
    '\bshow_first_run_guidance_if_needed\s*\(' = 'first-run startup decision'
    '\binspect_windows_first_run_guidance\s*\(' = 'current-EXE inspection factory'
    '\bsave_windows_first_run_guidance_acknowledgement\s*\(' = 'current-EXE save factory'
    'TDCBF_OK_BUTTON\s*\|\s*TDCBF_CLOSE_BUTTON' = 'OK and Close native buttons'
    '\bTDF_ALLOW_DIALOG_CANCELLATION\b' = 'Esc-capable guidance dialog'
    'nDefaultButton\s*=\s*IDOK' = 'keyboard default OK action'
    '\brestore_error_dialog_focus\s*\(' = 'focus restoration'
    'show_first_run_guidance_dialog\s*\(\s*\*state,\s*false\s*\)' = 'diagnostics redisplay'
    '\bcalculate_diagnostics_action_layout\s*\(' = 'non-overlapping four-action diagnostics layout'
    'UpdateWindow\(window\);\s*show_startup_resume_slot_review\(state\);\s*show_first_run_guidance_if_needed\(state\);' = 'main-window-first resume review then first-run startup order'
}
foreach ($pattern in $firstRunGuidanceMainRequiredPatterns.Keys) {
    if ($firstRunGuidanceMainText -notmatch $pattern) {
        $failures += "Windows first-run guidance product wiring must retain $($firstRunGuidanceMainRequiredPatterns[$pattern])"
    }
}
$firstRunGuidanceInspectFactoryCount = ([regex]::Matches(
        $firstRunGuidanceMainText,
        '\binspect_windows_first_run_guidance\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$firstRunGuidanceSaveFactoryCount = ([regex]::Matches(
        $firstRunGuidanceMainText,
        '\bsave_windows_first_run_guidance_acknowledgement\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
if ($firstRunGuidanceInspectFactoryCount -ne 1 -or
    $firstRunGuidanceSaveFactoryCount -ne 1) {
    $failures += 'Windows first-run product UI must retain one current-EXE inspect call and one current-EXE save call'
}

$firstRunGuidanceTestRequiredPatterns = [ordered]@{
    '\bthree_items_schema_and_compact_layout_are_fixed\s*\(' = 'three-item wording and compact layout coverage'
    '\bfirst_launch_acknowledgement_and_saved_launch_are_real\s*\(' = 'first and saved launch coverage'
    '\bknown_pending_document_uses_recoverable_replace\s*\(' = 'recoverable replace coverage'
    '\bmalformed_newer_and_foreign_stage_are_preserved\s*\(' = 'malformed, newer and collision coverage'
    '\bhardlink_read_only_and_locked_files_fail_closed\s*\(' = 'hardlink, read-only and lock coverage'
    '\breplace_failure_preserves_original_and_cleans_owned_files\s*\(' = 'real replace failure coverage'
    '\bmissing_reparse_and_appdata_storage_never_fall_back\s*\(' = 'missing, reparse and AppData-zero coverage'
    '\bproduct_source_connects_current_exe_factory_and_both_ui_paths\s*\(' = 'product static wiring coverage'
}
foreach ($pattern in $firstRunGuidanceTestRequiredPatterns.Keys) {
    if ($firstRunGuidanceTestText -notmatch $pattern) {
        $failures += "Windows first-run guidance tests must retain $($firstRunGuidanceTestRequiredPatterns[$pattern])"
    }
}
$firstRunGuidanceTestForbiddenPatterns = [ordered]@{
    'ytec::windowsapp::inspect_windows_first_run_guidance\s*\(' = 'production current-EXE inspection execution'
    'ytec::windowsapp::save_windows_first_run_guidance_acknowledgement\s*\(' = 'production current-EXE write execution'
    '\bSHGetKnownFolderPath\s*\(' = 'AppData lookup'
    '\bRegSetValue[A-Za-z0-9_]*\s*\(' = 'registry write'
    '\bWinHttp[A-Za-z0-9_]*\s*\(' = 'network access'
}
foreach ($pattern in $firstRunGuidanceTestForbiddenPatterns.Keys) {
    if ($firstRunGuidanceTestText -match $pattern) {
        $failures += "Windows first-run guidance tests contain forbidden $($firstRunGuidanceTestForbiddenPatterns[$pattern])"
    }
}

$allowedCreateProcessPaths = @(
    [IO.Path]::GetFullPath(
        (Join-Path $sourceRoot 'BootRepair\src\bcdboot.cpp'))
    $auditedAdkAcquisitionPlatformPath
    [IO.Path]::GetFullPath(
        (Join-Path $sourceRoot 'WinPEApp\src\main.cpp'))
)
$processLaunchMatches = $sourceFiles |
    Select-String -Pattern '\bCreateProcessW\s*\(' -CaseSensitive
foreach ($match in $processLaunchMatches) {
    $matchedPath = [IO.Path]::GetFullPath($match.Path)
    $isApprovedProcessLauncher = $allowedCreateProcessPaths |
        Where-Object {
            $matchedPath.Equals(
                $_,
                [StringComparison]::OrdinalIgnoreCase)
        }
    if (-not $isApprovedProcessLauncher) {
        $relative = $match.Path.Substring($repoRoot.Length).TrimStart('\')
        $failures += "$relative`:$($match.LineNumber) contains an unapproved process launch"
    }
}

$winPeLauncherText = Get-Content `
    -LiteralPath (Join-Path $sourceRoot 'WinPEApp\src\main.cpp') `
    -Raw
$activeRescueMediaText = Get-Content `
    -LiteralPath (Join-Path $sourceRoot 'WinPEApp\src\active_rescue_media.cpp') `
    -Raw
$winPeLauncherRequiredPatterns = [ordered]@{
    '--launch-gui-from-media' = 'explicit media GUI launch command'
    '\bquery_active_rescue_media_storage_with_windows_apis\s*\(' = 'fresh active-media storage resolution before launch'
    '\bread_marker_from_held_handle\s*\(' = 'held marker handle reread'
    '\bGetFinalPathNameByHandleW\s*\(' = 'opened payload path reidentification'
    '\bFILE_FLAG_OPEN_REPARSE_POINT\b' = 'non-following payload opens'
    '\bCreateProcessW\s*\(' = 'single approved GUI process launch'
    '\bWaitForSingleObject\s*\(' = 'bounded child lifecycle ownership'
    '\bGetExitCodeProcess\s*\(' = 'child result propagation'
}
foreach ($pattern in $winPeLauncherRequiredPatterns.Keys) {
    if ($winPeLauncherText -notmatch $pattern) {
        $failures += "WinPE media GUI launcher must retain $($winPeLauncherRequiredPatterns[$pattern])"
    }
}
$activeRescueStorageRequiredPatterns = [ordered]@{
    '\bresolve_active_rescue_media_storage\s*\(' = 'single active storage resolver'
    '\bquery_active_rescue_media_storage_with_windows_apis\s*\(' = 'production active storage API'
    'matching_paths\.size\(\)\s*!=\s*1U' = 'marker-root uniqueness gate'
    '\bmarker_identity_from_open_handle\s*=\s*true' = 'opened marker identity proof'
    '\bquery_single_disk_number_for_path\s*\(' = 'single physical backing extent gate'
    '\bmake_stable_disk_identity\s*\(' = 'active physical storage stable identity'
}
foreach ($pattern in $activeRescueStorageRequiredPatterns.Keys) {
    if ($activeRescueMediaText -notmatch $pattern) {
        $failures += "WinPE active rescue storage resolver must retain $($activeRescueStorageRequiredPatterns[$pattern])"
    }
}

# Specification v2.0 section 6.5/7/13: WinPE shrink-image restore is a
# same-session whole-disk controller. It must bind the authenticated image,
# destructive target, and every work artifact to three stable physical
# identities before the dedicated transaction receives control.
$winPeDirectShrinkRestorePath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'WinPEApp\src\direct_shrink_image_restore.cpp'))
$windowsShrinkWorkPlacementPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'WindowsApp\src\shrink_work_placement.cpp'))
$winPeDirectShrinkGuiPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'WinPEApp\src\gui_main.cpp'))
$winPeDirectShrinkRestoreTestPath = [IO.Path]::GetFullPath(
    (Join-Path $repoRoot 'tests\Unit\winpe_direct_shrink_image_restore_tests.cpp'))
foreach ($requiredPath in @(
        $winPeDirectShrinkRestorePath,
        $windowsShrinkWorkPlacementPath,
        $winPeDirectShrinkGuiPath,
        $winPeDirectShrinkRestoreTestPath)) {
    if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
        $failures += "WinPE direct shrink restore contract is missing $requiredPath"
    }
}
$winPeDirectShrinkRestoreText = Get-Content `
    -LiteralPath $winPeDirectShrinkRestorePath `
    -Raw
$windowsShrinkWorkPlacementText = Get-Content `
    -LiteralPath $windowsShrinkWorkPlacementPath `
    -Raw
$winPeDirectShrinkGuiText = Get-Content `
    -LiteralPath $winPeDirectShrinkGuiPath `
    -Raw
$winPeDirectShrinkRestoreTestText = Get-Content `
    -LiteralPath $winPeDirectShrinkRestoreTestPath `
    -Raw

$winPeDirectShrinkRestoreRequiredPatterns = [ordered]@{
    'request\.confirmation\.typed_token\s*!=\s*L"OK"' = 'exact uppercase OK gate'
    '\breviewed_original_source_target\b' = 'stable original-source target review binding'
    '\bvalidate_active_rescue_work_paths\s*\(' = 'active rescue data path gate'
    'active\.drive_type\s*!=\s*DRIVE_FIXED' = 'fixed physical work media allow-list'
    'active\.drive_type\s*!=\s*DRIVE_REMOVABLE' = 'removable physical work media allow-list'
    'L"rescue-media-id\.txt"' = 'active rescue marker filename binding'
    'L"shrink-restore-incomplete\.checkpoint"' = 'active rescue checkpoint path binding'
    'L"shrink-restore\.log"' = 'active rescue log path binding'
    '\bvalidate_windows_shrink_work_placement_observation\s*\(' = 'canonical local non-reparse work placement gate'
    '\bprepared\.value\(\)\.image\(\)\.partial_loss\b' = 'same-handle partial image rejection'
    'manifest\.logical_sector_size\s*!=\s*512U' = '512-byte source sector product boundary'
    'target\.logical_sector_size\s*!=\s*512U' = '512-byte target sector product boundary'
    '\bexecute_tsumugi_shrink_restore_plan_v1\s*\(' = 'dedicated shrink restore transaction execution'
    '\breport\.restore\.all_payloads_verified_by_adapter\b' = 'all payload flush and readback evidence'
    '\breport\.restore\.final_layout_committed\b' = 'final metadata commit evidence'
    '\breport\.direct_execution_only\b' = 'same-session direct execution evidence'
}
foreach ($pattern in $winPeDirectShrinkRestoreRequiredPatterns.Keys) {
    if ($winPeDirectShrinkRestoreText -notmatch $pattern) {
        $failures += "WinPE direct shrink restore must retain $($winPeDirectShrinkRestoreRequiredPatterns[$pattern])"
    }
}
$winPeDirectShrinkRestoreForbiddenPatterns = [ordered]@{
    '\bcontrol_direct_image_restore_resume_v1\b' = 'persistent resume controller'
    '\bDirectImageRestoreResume(Action|Outcome|Dependencies)\b' = 'persistent resume type'
    '\bwrite_new_verified_job_file\b' = 'reservation job writer'
    '\bTsumugiPhysicalIndividualPartitionRestoreSelection\b' = 'individual-partition restore selection'
    '\blog_is_ram_only\s*=\s*true' = 'RAM log fallback'
    '\bGetTempPathW?\s*\(' = 'temporary-directory fallback'
    '\bSHGetFolderPathW?\s*\(' = 'AppData fallback'
}
foreach ($pattern in $winPeDirectShrinkRestoreForbiddenPatterns.Keys) {
    if ($winPeDirectShrinkRestoreText -match $pattern) {
        $failures += "WinPE direct shrink restore contains forbidden $($winPeDirectShrinkRestoreForbiddenPatterns[$pattern])"
    }
}

$windowsShrinkWorkPlacementRequiredPatterns = [ordered]@{
    '\bsame_local_path\s*\(requested,\s*observed\.canonical_path\)' = 'requested-to-canonical exact path binding'
    '\bobserved\.parent_is_reparse\b' = 'reparse fail-closed gate'
    '\bquery_single_disk_number_for_local_path\s*\(' = 'single physical backing extent query'
    'scratch_before\.value\(\)\s*!=\s*scratch_after\.value\(\)' = 'scratch before-after reidentification'
    'checkpoint_before\.value\(\)\s*!=\s*checkpoint_after\.value\(\)' = 'checkpoint before-after reidentification'
    'log_before\.value\(\)\s*!=\s*log_after\.value\(\)' = 'log before-after reidentification'
}
foreach ($pattern in $windowsShrinkWorkPlacementRequiredPatterns.Keys) {
    if ($windowsShrinkWorkPlacementText -notmatch $pattern) {
        $failures += "Windows shrink work placement must retain $($windowsShrinkWorkPlacementRequiredPatterns[$pattern])"
    }
}

$winPeDirectShrinkGuiRequiredPatterns = [ordered]@{
    '#include\s+"ytec/winpeapp/direct_shrink_image_restore\.h"' = 'dedicated product controller include'
    '\bmake_direct_shrink_image_restore_layout_with_windows_apis\s*\(' = 'read-only immutable layout review'
    '\breview_direct_shrink_active_rescue_work_with_windows_apis\s*\(' = 'three-disk work review'
    '\bexecute_direct_shrink_image_restore_with_windows_apis\s*\(' = 'same-session shrink execution'
    '\.typed_token\s*=\s*L"OK"' = 'second-stage uppercase OK binding'
    '個別復元先（縮小では使用しません）' = 'whole-disk-only UI disclosure'
    '予約job・永続resume・個別復元は使用しません' = 'no-job/no-individual review disclosure'
}
foreach ($pattern in $winPeDirectShrinkGuiRequiredPatterns.Keys) {
    if ($winPeDirectShrinkGuiText -notmatch $pattern) {
        $failures += "WinPE direct shrink product GUI must retain $($winPeDirectShrinkGuiRequiredPatterns[$pattern])"
    }
}
foreach ($removedMessage in @(
        '縮小移行イメージの配置UIはまだ未接続',
        '縮小移行の復元UIは未接続')) {
    if ($winPeDirectShrinkGuiText.Contains($removedMessage)) {
        $failures += "WinPE direct shrink product GUI still contains disabled-path message $removedMessage"
    }
}

$winPeDirectShrinkRestoreTestRequiredPatterns = [ordered]@{
    '\btest_original_source_requires_bound_review_but_is_allowed\s*\(' = 'reviewed original-source target acceptance'
    '\btest_cd_and_x_work_media_stop_before_executor\s*\(' = 'CD and X work-media rejection'
    '\btest_all_three_work_paths_must_stay_on_active_rescue_disk\s*\(' = 'all work artifacts third-disk enforcement'
    '\btest_requested_and_observed_work_path_must_match\s*\(' = 'canonical requested path test'
    '\btest_4kn_manifest_stops_before_executor\s*\(' = '4Kn pre-I/O rejection'
    '\btest_fat32_archive_stops_before_executor\s*\(' = 'FAT32 archive pre-I/O rejection'
    '\btest_exfat_archive_stops_before_executor\s*\(' = 'exFAT archive pre-I/O rejection'
    '\btest_partial_image_stops_before_executor\s*\(' = 'partial image pre-I/O rejection'
    '\btest_incomplete_evidence_is_not_success\s*\(' = 'incomplete completion evidence rejection'
}
foreach ($pattern in $winPeDirectShrinkRestoreTestRequiredPatterns.Keys) {
    if ($winPeDirectShrinkRestoreTestText -notmatch $pattern) {
        $failures += "WinPE direct shrink restore tests must retain $($winPeDirectShrinkRestoreTestRequiredPatterns[$pattern])"
    }
}

# EXE-008: every currently exposed WinPE destructive product route freshly
# observes the one fixed resume slot after final confirmation. The admission
# plan is observation-only: it is never a persistent capability, checkpoint,
# resume target, or execution permission. A valid persistent restore may use
# only the exact complete binding through open_bound().
$winPeResumeAdmissionHeader = Get-Content -Raw -LiteralPath (
    Join-Path $sourceRoot 'WinPEApp\include\ytec\winpeapp\resume_slot_admission.h')
$winPeResumeAdmissionSource = Get-Content -Raw -LiteralPath (
    Join-Path $sourceRoot 'WinPEApp\src\resume_slot_admission.cpp')
$winPeResumeAdmissionTests = Get-Content -Raw -LiteralPath (
    Join-Path $repoRoot 'tests\Unit\winpe_resume_slot_admission_tests.cpp')
$winPeResumeAdmissionGui = Get-Content -Raw -LiteralPath (
    Join-Path $sourceRoot 'WinPEApp\src\gui_main.cpp')

$winPeResumeAdmissionRequiredPatterns = [ordered]@{
    '\bguard_new_operation_start\s*\(' = 'OperationCore global start-new gate'
    '\bopen_bound\s*\(reviewed_binding\)' = 'exact complete resume binding preflight'
    'kMaximumAdmissionTextFields\s*=\s*16U' = 'bounded immutable text field count'
    'kMaximumAdmissionDigests\s*=\s*16U' = 'bounded immutable digest count'
    'kMaximumAdmissionFieldCharacters\s*=\s*32U\s*\*\s*1024U' = 'bounded per-field UTF-16 length'
    'kMaximumAdmissionCanonicalBytes\s*=\s*256U\s*\*\s*1024U' = 'bounded total canonical bytes'
    'catch\s*\(const std::bad_alloc&\)' = 'allocation failure converted to Result'
}
foreach ($pattern in $winPeResumeAdmissionRequiredPatterns.Keys) {
    if ($winPeResumeAdmissionSource -notmatch $pattern) {
        $failures += "WinPE global resume admission helper must retain $($winPeResumeAdmissionRequiredPatterns[$pattern])"
    }
}
if (($winPeResumeAdmissionHeader + $winPeResumeAdmissionSource) -match
        'ResumeCapability::|\.create\s*\(|\.replace\s*\(|\.discard\s*\(') {
    $failures += 'WinPE admission-only helper must not create/replace/discard a slot or declare a persistent resume capability'
}

$winPeResumeAdmissionTestRequiredPatterns = [ordered]@{
    '\badmission_canonicalization_rejects_empty_huge_and_overflow_shapes\s*\(' = 'empty, huge, and overflow-shaped field coverage'
    '\bactive_unknown_and_orphaned_slot_state_block_every_new_operation\s*\(' = 'active/unknown/orphan unchanged fail-closed coverage'
    '\bactive_slot_also_blocks_starting_the_same_operation_as_new\s*\(' = 'same-operation start-new rejection coverage'
    '\bresume_requires_the_exact_complete_binding_and_never_mutates_preflight\s*\(' = 'exact open_bound and mismatch coverage'
    '\bstartup_review_rejects_every_non_restore_persistent_capability\s*\(' = 'restore-only startup presentation coverage'
}
foreach ($pattern in $winPeResumeAdmissionTestRequiredPatterns.Keys) {
    if ($winPeResumeAdmissionTests -notmatch $pattern) {
        $failures += "WinPE global resume admission tests must retain $($winPeResumeAdmissionTestRequiredPatterns[$pattern])"
    }
}

$newAdmissionGateCount = [regex]::Matches(
    $winPeResumeAdmissionGui,
    '\bguard_new_product_operation_start\s*\(').Count
if ($newAdmissionGateCount -ne 6) {
    $failures += "WinPE GUI must retain one new-operation gate definition plus exactly five current route calls (found $newAdmissionGateCount). If a legitimate destructive route is added or removed, wire it through the same fresh gate and intentionally update this audit."
}
$boundResumeGateCount = [regex]::Matches(
    $winPeResumeAdmissionGui,
    '\bguard_bound_product_restore_resume\s*\(').Count
if ($boundResumeGateCount -ne 2) {
    $failures += "WinPE GUI must retain one exact-bound resume gate definition plus one restore-resume call (found $boundResumeGateCount). If the reviewed resume route changes, preserve open_bound-only admission and intentionally update this audit."
}

$winPeResumeAdmissionRouteRegions = [ordered]@{
    'clone' = @('void start_clone_execute(', 'void request_clone_cancellation(', 1, 0)
    'image create' = @('void start_image_execute(', 'void request_image_cancellation(', 1, 0)
    'image restore' = @('void start_restore_execute(', 'void request_restore_cancellation(', 1, 1)
    'boot/system repair' = @('void start_boot_execute(', 'void cancel_boot_review(', 2, 0)
}
foreach ($route in $winPeResumeAdmissionRouteRegions.Keys) {
    $definition = $winPeResumeAdmissionRouteRegions[$route]
    $startIndex = $winPeResumeAdmissionGui.IndexOf(
        [string]$definition[0], [StringComparison]::Ordinal)
    $endIndex = if ($startIndex -ge 0) {
        $winPeResumeAdmissionGui.IndexOf(
            [string]$definition[1],
            $startIndex + ([string]$definition[0]).Length,
            [StringComparison]::Ordinal)
    } else {
        -1
    }
    if ($startIndex -lt 0 -or $endIndex -le $startIndex) {
        $failures += "WinPE GUI global resume admission audit cannot isolate the $route route"
        continue
    }
    $region = $winPeResumeAdmissionGui.Substring(
        $startIndex, $endIndex - $startIndex)
    $newCount = [regex]::Matches(
        $region, '\bguard_new_product_operation_start\s*\(').Count
    $boundCount = [regex]::Matches(
        $region, '\bguard_bound_product_restore_resume\s*\(').Count
    if ($newCount -ne [int]$definition[2] -or
        $boundCount -ne [int]$definition[3]) {
        $failures += "WinPE $route route must retain $($definition[2]) fresh start-new gate(s) and $($definition[3]) exact-bound resume gate(s); found $newCount and $boundCount. Update this audit only together with a reviewed route change."
    }
}

# EXE-008 Windows product: all currently exposed target/output writers pass a
# fresh observation of the same fixed slot immediately before their reviewed
# executor. Windows has no reviewed persistent-resume controller, so startup
# may only retain the slot or discard its exactly rebound app-owned objects.
$windowsResumeProductHeader = Get-Content -Raw -LiteralPath (
    Join-Path $sourceRoot 'WindowsApp\include\ytec\windowsapp\resume_slot_product.h')
$windowsResumeProductSource = Get-Content -Raw -LiteralPath (
    Join-Path $sourceRoot 'WindowsApp\src\resume_slot_product.cpp')
$windowsResumeProductTests = Get-Content -Raw -LiteralPath (
    Join-Path $repoRoot 'tests\Unit\windows_resume_slot_product_tests.cpp')
$windowsResumeProductUi = Get-Content -Raw -LiteralPath (
    Join-Path $sourceRoot 'WindowsApp\src\main.cpp')

$windowsResumeProductRequiredPatterns = [ordered]@{
    '\bguard_new_operation_start\s*\(' = 'OperationCore global start-new gate'
    '\bslot\.discard\s*\(reviewed_binding\)' = 'fresh exact-bound owned discard'
    'kMaximumAdmissionTextFields\s*=\s*16U' = 'bounded immutable text field count'
    'kMaximumAdmissionDigests\s*=\s*16U' = 'bounded immutable digest count'
    'kMaximumAdmissionFieldCharacters\s*=\s*32U\s*\*\s*1024U' = 'bounded per-field UTF-16 length'
    'kMaximumAdmissionCanonicalBytes\s*=\s*256U\s*\*\s*1024U' = 'bounded total canonical bytes'
    '\bchecked_add_bounded\s*\(' = 'checked canonical-size addition'
    '\bchecked_multiply\s*\(' = 'checked UTF-16 byte multiplication'
    'catch\s*\(const std::bad_alloc&\)' = 'allocation failure converted to Result'
    'catch\s*\(const std::length_error&\)' = 'length failure converted to Result'
    '\brequire_path_absent\s*\(' = 'read-only absent child proof'
    'active\.checkpoint\.new' = 'fixed stage-object absence proof'
    '\.resume_action_available\s*=\s*false' = 'no fake Windows resume action'
}
foreach ($pattern in $windowsResumeProductRequiredPatterns.Keys) {
    if ($windowsResumeProductSource -notmatch $pattern) {
        $failures += "Windows global resume product helper must retain $($windowsResumeProductRequiredPatterns[$pattern])"
    }
}

$windowsAdmissionStart = $windowsResumeProductSource.IndexOf(
    'make_windows_resume_slot_admission_plan(',
    [StringComparison]::Ordinal)
$windowsAdmissionEnd = $windowsResumeProductSource.IndexOf(
    'make_windows_resume_slot_admission_operation_id_with_windows_apis(',
    [StringComparison]::Ordinal)
if ($windowsAdmissionStart -lt 0 -or
    $windowsAdmissionEnd -le $windowsAdmissionStart) {
    $failures += 'Windows admission-only helper region cannot be isolated'
} else {
    $windowsAdmissionRegion = $windowsResumeProductSource.Substring(
        $windowsAdmissionStart,
        $windowsAdmissionEnd - $windowsAdmissionStart)
    if ($windowsAdmissionRegion -match
            'ResumeCapability::|\.create\s*\(|\.replace\s*\(|\.discard\s*\(') {
        $failures += 'Windows admission-only plan must not declare a persistent capability or create/replace/discard a slot'
    }
}

$windowsResumeProductTestRequiredPatterns = [ordered]@{
    '\badmission_plan_is_bounded_and_binds_real_output\s*\(' = 'output binding plus field/digest/canonical bound coverage'
    '\bglobal_admission_freshly_blocks_active_unknown_corrupt_and_orphan_state\s*\(' = 'active, unknown, corrupt, and orphan unchanged fail-closed coverage'
    '\bstartup_view_never_offers_fake_resume_and_discard_is_exact_bound\s*\(' = 'no fake resume and stale/exact discard coverage'
}
foreach ($pattern in $windowsResumeProductTestRequiredPatterns.Keys) {
    if ($windowsResumeProductTests -notmatch $pattern) {
        $failures += "Windows global resume product tests must retain $($windowsResumeProductTestRequiredPatterns[$pattern])"
    }
}

$windowsResumeStartupRequiredPatterns = [ordered]@{
    '\bshow_startup_resume_slot_review\s*\(' = 'startup active-slot review entry point'
    'view\.resume_action_available\s*\|\|' = 'resume-action contract rejection'
    '所有対象の破棄確認へ' = 'first explicit discard review'
    '\.token\s*=\s*L"破棄"' = 'second typed discard confirmation'
    '\bdiscard_current_windows_resume_slot\s*\(' = 'fresh exact-binding discard wrapper'
    'user_confirmed=2stage' = 'two-stage confirmation evidence log'
}
foreach ($pattern in $windowsResumeStartupRequiredPatterns.Keys) {
    if ($windowsResumeProductUi -notmatch $pattern) {
        $failures += "Windows resume startup UI must retain $($windowsResumeStartupRequiredPatterns[$pattern])"
    }
}

$windowsResumeGuardCount = [regex]::Matches(
    $windowsResumeProductUi,
    '\bguard_current_windows_operation_start\s*\(').Count
if ($windowsResumeGuardCount -ne 7) {
    $failures += "Windows product must retain exactly seven fresh resume-slot guards for nine current writer executors (found $windowsResumeGuardCount)"
}
$windowsResumeRoutePatterns = [ordered]@{
    'rescue media' = '\bguard_current_windows_operation_start\s*\([\s\S]{0,480}?\bexecute_rescue_media_creation_with_windows_apis\s*\('
    'raw rescue clone' = '\bguard_current_windows_operation_start\s*\([\s\S]{0,480}?\bexecute_windows_data_rescue_clone\s*\('
    'shrink clone' = '\bguard_current_windows_operation_start\s*\([\s\S]{0,480}?\bexecute_windows_direct_shrink_clone_with_windows_apis\s*\('
    'exact clone' = '\bguard_current_windows_operation_start\s*\([\s\S]{0,480}?\bexecute_online_direct_clone_operation_with_windows_apis\s*\('
    'shrink restore' = '\bguard_current_windows_operation_start\s*\([\s\S]{0,480}?\bexecute_windows_online_shrink_restore_operation_with_windows_apis\s*\('
    'exact restore' = '\bguard_current_windows_operation_start\s*\([\s\S]{0,480}?\bexecute_online_image_restore_operation_with_windows_apis\s*\('
    'rescue image create' = '\bguard_current_windows_operation_start\s*\([\s\S]{0,960}?\bexecute_windows_data_rescue_image_create_with_windows_apis\s*\('
    'shrink image create' = '\bguard_current_windows_operation_start\s*\([\s\S]{0,1280}?\bexecute_windows_online_shrink_image_create_with_windows_apis\s*\('
    'exact image create' = '\bguard_current_windows_operation_start\s*\([\s\S]{0,1440}?\bexecute_online_image_create_with_windows_apis\s*\('
}
foreach ($route in $windowsResumeRoutePatterns.Keys) {
    $count = [regex]::Matches(
        $windowsResumeProductUi,
        $windowsResumeRoutePatterns[$route],
        [Text.RegularExpressions.RegexOptions]::CultureInvariant).Count
    if ($count -ne 1) {
        $failures += "Windows $route writer must retain exactly one fresh resume-slot guard immediately before execution (found $count)"
    }
}

# Reservation-job product and compatibility implementations were removed for
# specification v2.0. User-owned historical JSON is deliberately neither
# searched nor converted; this audit only proves the executable source paths do
# not return.
$legacyJobSourcePaths = @(
    'ImageFormat\include\ytec\imageformat\job_file.h'
    'ImageFormat\include\ytec\imageformat\job_manifest.h'
    'ImageFormat\include\ytec\imageformat\job_result.h'
    'ImageFormat\src\job_file.cpp'
    'ImageFormat\src\job_manifest.cpp'
    'ImageFormat\src\job_result.cpp'
    'WindowsApp\include\ytec\windowsapp\job_creation.h'
    'WindowsApp\include\ytec\windowsapp\job_result_import.h'
    'WindowsApp\include\ytec\windowsapp\reboot_handoff.h'
    'WindowsApp\src\job_creation.cpp'
    'WindowsApp\src\job_result_import.cpp'
    'WindowsApp\src\reboot_handoff.cpp'
    'WinPEApp\include\ytec\winpeapp\job_result.h'
    'WinPEApp\src\job_manifest_candidate_provider.cpp'
    'WinPEApp\src\job_manifest_loader.cpp'
    'WinPEApp\src\job_result.cpp'
)
foreach ($relativePath in $legacyJobSourcePaths) {
    if (Test-Path -LiteralPath (Join-Path $sourceRoot $relativePath)) {
        $failures += "Legacy reservation-job source must remain absent: $relativePath"
    }
}
$legacyJobProductTokens = @(
    '--job-'
    'YTEC_ENABLE_LEGACY_JOB_FIXTURES'
    'write_new_verified_job_file'
    'parse_and_verify_hashed_job_manifest'
)
foreach ($token in $legacyJobProductTokens) {
    $matches = $sourceFiles | Select-String -SimpleMatch -Pattern $token
    foreach ($match in $matches) {
        $relative = $match.Path.Substring($repoRoot.Length).TrimStart('\')
        $failures += "$relative`:$($match.LineNumber) contains removed reservation-job token $token"
    }
}

# Product WinPE CLI is deliberately read-only. Destructive operations remain
# available only from the product GUI after its in-session review and exact OK
# entry; a service-injection parameter or fixed command-line confirmation would
# recreate a bypass even when the GUI itself stayed safe.
$winPeCliRunnerPath = Join-Path $sourceRoot 'WinPEApp\src\app_runner.cpp'
$winPeCliHeaderPath = Join-Path $sourceRoot `
    'WinPEApp\include\ytec\winpeapp\app_runner.h'
$winPeCliMainPath = Join-Path $sourceRoot 'WinPEApp\src\main.cpp'
$winPeCliTestsPath = Join-Path $repoRoot `
    'tests\Unit\winpe_direct_app_tests.cpp'
$winPeMediaBuilderPath = Join-Path $repoRoot `
    'scripts\New-WinPEAppValidationMedia.ps1'
$winPeCliRunnerText = Get-Content -Raw -LiteralPath $winPeCliRunnerPath
$winPeCliHeaderText = Get-Content -Raw -LiteralPath $winPeCliHeaderPath
$winPeCliMainText = Get-Content -Raw -LiteralPath $winPeCliMainPath
$winPeCliTestsText = Get-Content -Raw -LiteralPath $winPeCliTestsPath
$winPeMediaBuilderText = Get-Content -Raw -LiteralPath $winPeMediaBuilderPath

$destructiveWinPeCliTokens = @(
    '--clone-execute'
    '--acknowledge-target-erasure'
    '--boot-repair-execute'
    '--acknowledge-boot-files-change'
)
foreach ($token in $destructiveWinPeCliTokens) {
    foreach ($product in @(
            [ordered]@{ path = $winPeCliRunnerPath; text = $winPeCliRunnerText },
            [ordered]@{ path = $winPeCliHeaderPath; text = $winPeCliHeaderText },
            [ordered]@{ path = $winPeCliMainPath; text = $winPeCliMainText },
            [ordered]@{ path = $winPeMediaBuilderPath; text = $winPeMediaBuilderText })) {
        if ($product.text.Contains($token)) {
            $relative = $product.path.Substring($repoRoot.Length).TrimStart('\')
            $failures += "$relative contains forbidden destructive product CLI token $token"
        }
    }
}
if ($winPeCliRunnerText -notmatch
        'arguments\.front\(\)\s*!=\s*L"--clone-preflight"' -or
    $winPeCliRunnerText -notmatch
        'このCLIは列挙とクローン事前確認だけを行い、ディスクへ書き込みません') {
    $failures += 'WinPE product CLI must retain the read-only clone-preflight-only parser and disclosure'
}
if (($winPeCliHeaderText + $winPeCliRunnerText) -match
        'run_winpe_app[\s\S]{0,320}ICloneExecutionService\s*\*' -or
    $winPeCliMainText -match '\bmake_windows_clone_execution_service\s*\(') {
    $failures += 'WinPE product CLI must not accept or construct a destructive clone execution service'
}
foreach ($requiredTest in @(
        'destructive_cli_arguments_stop_before_all_io',
        'output\.str\(\)\.find\("--clone-execute"\)',
        'provider\.call_count\s*==\s*0U')) {
    if ($winPeCliTestsText -notmatch $requiredTest) {
        $failures += "WinPE read-only CLI regression coverage is missing $requiredTest"
    }
}
if ($winPeMediaBuilderText -match 'StandaloneBootRepair') {
    $failures += 'Product WinPE media builder must not retain the retired destructive CLI boot-repair scenario'
}

$excludedRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot 'out'))
$forbiddenMicrosoftNames = @(
    'mbr2gpt.exe'
    'bcdboot.exe'
    'dism.exe'
    'diskpart.exe'
    'reagentc.exe'
    'copype.cmd'
    'MakeWinPEMedia.cmd'
    'oscdimg.exe'
)
$forbiddenExtensions = @('.wim', '.iso', '.cab')

$repositoryFiles = @(
    Get-ChildItem -LiteralPath $repoRoot -File
)
$topLevelDirectories = Get-ChildItem -LiteralPath $repoRoot -Directory |
    Where-Object {
        -not $_.FullName.Equals(
            $excludedRoot,
            [StringComparison]::OrdinalIgnoreCase)
    }
foreach ($directory in $topLevelDirectories) {
    if ($directory.Name -ne '.validation') {
        $repositoryFiles += Get-ChildItem `
            -LiteralPath $directory.FullName `
            -Recurse `
            -File
        continue
    }

    # VM credentials belong to the shared lab and are deliberately
    # inaccessible to repository checks. Scan every other validation asset,
    # but never request access to .validation\vm-secrets.
    $repositoryFiles += Get-ChildItem -LiteralPath $directory.FullName -File
    $validationDirectories =
        Get-ChildItem -LiteralPath $directory.FullName -Directory |
        Where-Object { $_.Name -ne 'vm-secrets' }
    foreach ($validationDirectory in $validationDirectories) {
        $repositoryFiles += Get-ChildItem `
            -LiteralPath $validationDirectory.FullName `
            -Recurse `
            -File
    }
}
foreach ($file in $repositoryFiles) {
    if ($forbiddenMicrosoftNames -contains $file.Name -or
        $forbiddenExtensions -contains $file.Extension.ToLowerInvariant()) {
        $relative = $file.FullName.Substring($repoRoot.Length).TrimStart('\')
        $failures += "禁止された Microsoft / WinPE 配布物の候補: $relative"
    }
}

if ($failures.Count -gt 0) {
    $failures | ForEach-Object { Write-Error $_ }
    throw 'Safety boundary check: FAIL'
}

Write-Output 'Safety boundary check: PASS (audited writers plus VM-only UART validation and normal-shutdown boundary enforced)'
