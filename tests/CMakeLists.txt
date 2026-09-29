add_executable(
  ytec-operation-core-tests
  Unit/operation_core_tests.cpp
)

target_link_libraries(
  ytec-operation-core-tests
  PRIVATE
    ytec::operation_core
)

ytec_configure_target(ytec-operation-core-tests)
add_test(NAME ytec-operation-core-tests COMMAND ytec-operation-core-tests)

add_executable(
  ytec-resume-slot-tests
  Unit/resume_slot_tests.cpp
)

target_link_libraries(
  ytec-resume-slot-tests
  PRIVATE
    ytec::operation_core
)

ytec_configure_target(ytec-resume-slot-tests)
add_test(NAME ytec-resume-slot-tests COMMAND ytec-resume-slot-tests)

add_executable(
  ytec-windows-resume-slot-platform-tests
  Unit/windows_resume_slot_platform_tests.cpp
)

target_link_libraries(
  ytec-windows-resume-slot-platform-tests
  PRIVATE
    ytec::operation_core
)

ytec_configure_target(ytec-windows-resume-slot-platform-tests)
add_test(
  NAME ytec-windows-resume-slot-platform-tests
  COMMAND ytec-windows-resume-slot-platform-tests
)

add_executable(
  ytec-operation-power-tests
  Unit/operation_power_tests.cpp
)

target_link_libraries(
  ytec-operation-power-tests
  PRIVATE
    ytec::clone_core
)

ytec_configure_target(ytec-operation-power-tests)
add_test(NAME ytec-operation-power-tests COMMAND ytec-operation-power-tests)

add_executable(
  ytec-completion-power-action-tests
  Unit/completion_power_action_tests.cpp
)

target_link_libraries(
  ytec-completion-power-action-tests
  PRIVATE
    ytec::clone_core
)

ytec_configure_target(ytec-completion-power-action-tests)
add_test(
  NAME ytec-completion-power-action-tests
  COMMAND ytec-completion-power-action-tests
)

add_executable(
  ytec-windows-completion-power-ui-tests
  Unit/windows_completion_power_ui_tests.cpp
)

target_link_libraries(
  ytec-windows-completion-power-ui-tests
  PRIVATE
    ytec::windows_app
)

ytec_configure_target(ytec-windows-completion-power-ui-tests)
add_test(
  NAME ytec-windows-completion-power-ui-tests
  COMMAND ytec-windows-completion-power-ui-tests
)

add_executable(
  ytec-post-migration-check-tests
  Unit/post_migration_check_tests.cpp
  ${PROJECT_SOURCE_DIR}/src/WindowsApp/src/post_migration_check.cpp
)

target_include_directories(
  ytec-post-migration-check-tests
  PRIVATE
    ${PROJECT_SOURCE_DIR}/src/WindowsApp/include
)

target_link_libraries(
  ytec-post-migration-check-tests
  PRIVATE
    ole32
    oleaut32
    wbemuuid
    ytec::boot_repair
    ytec::disk_model
)

target_compile_definitions(
  ytec-post-migration-check-tests
  PRIVATE
    YTEC_WINDOWS_APP_MAIN_SOURCE_PATH="${PROJECT_SOURCE_DIR}/src/WindowsApp/src/main.cpp"
)

ytec_configure_target(ytec-post-migration-check-tests)

add_test(
  NAME ytec-post-migration-check-tests
  COMMAND ytec-post-migration-check-tests
)

add_executable(
  ytec-winpe-completion-power-ui-tests
  Unit/winpe_completion_power_ui_tests.cpp
)

target_link_libraries(
  ytec-winpe-completion-power-ui-tests
  PRIVATE
    ytec::winpe_app_logic
)

ytec_configure_target(ytec-winpe-completion-power-ui-tests)
add_test(
  NAME ytec-winpe-completion-power-ui-tests
  COMMAND ytec-winpe-completion-power-ui-tests
)

add_executable(
  ytec-winpe-resume-slot-admission-tests
  Unit/winpe_resume_slot_admission_tests.cpp
)

target_link_libraries(
  ytec-winpe-resume-slot-admission-tests
  PRIVATE
    ytec::winpe_app_logic
)

ytec_configure_target(ytec-winpe-resume-slot-admission-tests)
add_test(
  NAME ytec-winpe-resume-slot-admission-tests
  COMMAND ytec-winpe-resume-slot-admission-tests
)

add_executable(
  ytec-windows-resume-slot-product-tests
  Unit/windows_resume_slot_product_tests.cpp
)

target_link_libraries(
  ytec-windows-resume-slot-product-tests
  PRIVATE
    ytec::windows_app
)

ytec_configure_target(ytec-windows-resume-slot-product-tests)
add_test(
  NAME ytec-windows-resume-slot-product-tests
  COMMAND ytec-windows-resume-slot-product-tests
)

add_executable(
  ytec-manual-pause-tests
  Unit/manual_pause_tests.cpp
)

target_link_libraries(
  ytec-manual-pause-tests
  PRIVATE
    ytec::clone_core
)

ytec_configure_target(ytec-manual-pause-tests)
add_test(NAME ytec-manual-pause-tests COMMAND ytec-manual-pause-tests)

add_executable(
  ytec-tsumugi-crypto-tests
  Unit/tsumugi_crypto_tests.cpp
)

target_link_libraries(
  ytec-tsumugi-crypto-tests
  PRIVATE
    ytec::image_format
    ytec::argon2
)

ytec_configure_target(ytec-tsumugi-crypto-tests)
add_test(NAME ytec-tsumugi-crypto-tests COMMAND ytec-tsumugi-crypto-tests)

add_executable(
  ytec-tsumugi-format-tests
  Unit/tsumugi_format_tests.cpp
)

target_link_libraries(
  ytec-tsumugi-format-tests
  PRIVATE
    ytec::image_format
)

ytec_configure_target(ytec-tsumugi-format-tests)
add_test(NAME ytec-tsumugi-format-tests COMMAND ytec-tsumugi-format-tests)

add_executable(
  ytec-tsumugi-stream-tests
  Unit/tsumugi_stream_tests.cpp
)

target_link_libraries(
  ytec-tsumugi-stream-tests
  PRIVATE
    ytec::image_format
)

ytec_configure_target(ytec-tsumugi-stream-tests)
add_test(NAME ytec-tsumugi-stream-tests COMMAND ytec-tsumugi-stream-tests)

add_executable(
  ytec-tsumugi-v1-golden-corpus-tests
  Unit/tsumugi_v1_golden_corpus_tests.cpp
)

target_link_libraries(
  ytec-tsumugi-v1-golden-corpus-tests
  PRIVATE
    ytec::image_format
)

target_compile_definitions(
  ytec-tsumugi-v1-golden-corpus-tests
  PRIVATE
    YTEC_IMAGE_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/ImageGolden"
)

ytec_configure_target(ytec-tsumugi-v1-golden-corpus-tests)
add_test(
  NAME ytec-tsumugi-v1-golden-corpus-tests
  COMMAND ytec-tsumugi-v1-golden-corpus-tests
)
set_tests_properties(
  ytec-tsumugi-v1-golden-corpus-tests
  PROPERTIES TIMEOUT 120
)

add_executable(
  ytec-tsumugi-manifest-tests
  Unit/tsumugi_manifest_tests.cpp
)

target_link_libraries(
  ytec-tsumugi-manifest-tests
  PRIVATE
    ytec::image_format
)

ytec_configure_target(ytec-tsumugi-manifest-tests)
add_test(NAME ytec-tsumugi-manifest-tests COMMAND ytec-tsumugi-manifest-tests)

add_executable(
  ytec-tsumugi-image-service-tests
  Unit/tsumugi_image_service_tests.cpp
)

target_link_libraries(
  ytec-tsumugi-image-service-tests
  PRIVATE
    ytec::image_format
)

ytec_configure_target(ytec-tsumugi-image-service-tests)
add_test(
  NAME ytec-tsumugi-image-service-tests
  COMMAND ytec-tsumugi-image-service-tests
)

add_executable(
  ytec-tsumugi-restore-transaction-tests
  Unit/tsumugi_restore_transaction_tests.cpp
)

target_link_libraries(
  ytec-tsumugi-restore-transaction-tests
  PRIVATE
    ytec::image_format
)

ytec_configure_target(ytec-tsumugi-restore-transaction-tests)
add_test(
  NAME ytec-tsumugi-restore-transaction-tests
  COMMAND ytec-tsumugi-restore-transaction-tests
)

add_executable(
  ytec-tsumugi-restore-layout-tests
  Unit/tsumugi_restore_layout_tests.cpp
)

target_link_libraries(
  ytec-tsumugi-restore-layout-tests
  PRIVATE
    ytec::image_format
)

ytec_configure_target(ytec-tsumugi-restore-layout-tests)
add_test(
  NAME ytec-tsumugi-restore-layout-tests
  COMMAND ytec-tsumugi-restore-layout-tests
)

add_executable(
  ytec-tsumugi-shrink-construction-layout-tests
  Unit/tsumugi_shrink_construction_layout_tests.cpp
)

target_link_libraries(
  ytec-tsumugi-shrink-construction-layout-tests
  PRIVATE
    ytec::image_format
)

ytec_configure_target(ytec-tsumugi-shrink-construction-layout-tests)
add_test(
  NAME ytec-tsumugi-shrink-construction-layout-tests
  COMMAND ytec-tsumugi-shrink-construction-layout-tests
)

add_executable(
  ytec-tsumugi-restore-layout-io-tests
  Unit/tsumugi_restore_layout_io_tests.cpp
)

target_link_libraries(
  ytec-tsumugi-restore-layout-io-tests
  PRIVATE
    ytec::image_format
)

ytec_configure_target(ytec-tsumugi-restore-layout-io-tests)
add_test(
  NAME ytec-tsumugi-restore-layout-io-tests
  COMMAND ytec-tsumugi-restore-layout-io-tests
)

add_executable(
  ytec-tsumugi-physical-restore-tests
  Unit/tsumugi_physical_restore_tests.cpp
)

target_link_libraries(
  ytec-tsumugi-physical-restore-tests
  PRIVATE
    ytec::image_format
)

ytec_configure_target(ytec-tsumugi-physical-restore-tests)
add_test(
  NAME ytec-tsumugi-physical-restore-tests
  COMMAND ytec-tsumugi-physical-restore-tests
)

add_executable(
  ytec-rescue-copy-tests
  Unit/rescue_copy_tests.cpp
)

target_link_libraries(
  ytec-rescue-copy-tests
  PRIVATE
    ytec::clone_core
)

ytec_configure_target(ytec-rescue-copy-tests)
add_test(NAME ytec-rescue-copy-tests COMMAND ytec-rescue-copy-tests)

add_executable(
  ytec-unit-tests
  Unit/unit_tests.cpp
)

target_link_libraries(
  ytec-unit-tests
  PRIVATE
    ytec::clone_core
    ytec::disk_model
)

ytec_configure_target(ytec-unit-tests)
add_test(NAME ytec-unit-tests COMMAND ytec-unit-tests)

add_executable(
  ytec-log-privacy-tests
  Unit/log_privacy_tests.cpp
)

target_link_libraries(
  ytec-log-privacy-tests
  PRIVATE
    ytec::clone_core
)

ytec_configure_target(ytec-log-privacy-tests)
add_test(NAME ytec-log-privacy-tests COMMAND ytec-log-privacy-tests)

add_executable(
  ytec-shrink-layout-tests
  Unit/shrink_layout_tests.cpp
)

target_link_libraries(
  ytec-shrink-layout-tests
  PRIVATE
    ytec::migration_core
)

ytec_configure_target(ytec-shrink-layout-tests)
add_test(NAME ytec-shrink-layout-tests COMMAND ytec-shrink-layout-tests)

add_executable(
  ytec-direct-clone-plan-tests
  Unit/direct_clone_plan_tests.cpp
)

target_link_libraries(
  ytec-direct-clone-plan-tests
  PRIVATE
    ytec::migration_core
)

ytec_configure_target(ytec-direct-clone-plan-tests)
add_test(
  NAME ytec-direct-clone-plan-tests
  COMMAND ytec-direct-clone-plan-tests
)

add_executable(
  ytec-direct-shrink-target-contract-tests
  Unit/direct_shrink_target_contract_tests.cpp
)

target_link_libraries(
  ytec-direct-shrink-target-contract-tests
  PRIVATE
    ytec::direct_shrink_target
)

ytec_configure_target(ytec-direct-shrink-target-contract-tests)
add_test(
  NAME ytec-direct-shrink-target-contract-tests
  COMMAND ytec-direct-shrink-target-contract-tests
)

add_executable(
  ytec-winpe-offline-ntfs-direct-shrink-product-tests
  Unit/winpe_offline_ntfs_direct_shrink_product_tests.cpp
)

target_link_libraries(
  ytec-winpe-offline-ntfs-direct-shrink-product-tests
  PRIVATE
    ytec::winpe_app_logic
)

target_compile_definitions(
  ytec-winpe-offline-ntfs-direct-shrink-product-tests
  PRIVATE
    YTEC_WINPE_GUI_SOURCE_PATH="${PROJECT_SOURCE_DIR}/src/WinPEApp/src/gui_main.cpp"
    YTEC_WINPE_DIRECT_SHRINK_PRODUCT_SOURCE_PATH="${PROJECT_SOURCE_DIR}/src/WinPEApp/src/offline_ntfs_direct_shrink_product.cpp"
)

ytec_configure_target(ytec-winpe-offline-ntfs-direct-shrink-product-tests)
add_test(
  NAME ytec-winpe-offline-ntfs-direct-shrink-product-tests
  COMMAND ytec-winpe-offline-ntfs-direct-shrink-product-tests
)

add_executable(
  ytec-file-system-recreate-tests
  Unit/file_system_recreate_tests.cpp
)

target_link_libraries(
  ytec-file-system-recreate-tests
  PRIVATE
    ytec::migration_core
)

ytec_configure_target(ytec-file-system-recreate-tests)
add_test(
  NAME ytec-file-system-recreate-tests
  COMMAND ytec-file-system-recreate-tests
)

add_executable(
  ytec-windows-file-system-recreate-tests
  Unit/windows_file_system_recreate_tests.cpp
)

target_link_libraries(
  ytec-windows-file-system-recreate-tests
  PRIVATE
    ytec::migration_engine
    ytec::migration_core
    ytec::image_format
)

ytec_configure_target(ytec-windows-file-system-recreate-tests)
add_test(
  NAME ytec-windows-file-system-recreate-tests
  COMMAND ytec-windows-file-system-recreate-tests
)

add_executable(
  ytec-windows-clone-partition-capacity-review-tests
  Unit/windows_clone_partition_capacity_review_tests.cpp
)

target_link_libraries(
  ytec-windows-clone-partition-capacity-review-tests
  PRIVATE
    ytec::windows_app
)

target_compile_definitions(
  ytec-windows-clone-partition-capacity-review-tests
  PRIVATE
    YTEC_WINDOWS_APP_MAIN_SOURCE_PATH="${PROJECT_SOURCE_DIR}/src/WindowsApp/src/main.cpp"
)

ytec_configure_target(ytec-windows-clone-partition-capacity-review-tests)
add_test(
  NAME ytec-windows-clone-partition-capacity-review-tests
  COMMAND ytec-windows-clone-partition-capacity-review-tests
)

add_executable(
  ytec-windows-app-progress-tests
  Unit/windows_app_progress_tests.cpp
)

target_link_libraries(
  ytec-windows-app-progress-tests
  PRIVATE
    ytec::windows_app
)

ytec_configure_target(ytec-windows-app-progress-tests)
add_test(
  NAME ytec-windows-app-progress-tests
  COMMAND ytec-windows-app-progress-tests
)

add_executable(
  ytec-windows-online-direct-clone-tests
  Unit/windows_online_direct_clone_tests.cpp
)

target_link_libraries(
  ytec-windows-online-direct-clone-tests
  PRIVATE
    ytec::windows_app
)

ytec_configure_target(ytec-windows-online-direct-clone-tests)
add_test(
  NAME ytec-windows-online-direct-clone-tests
  COMMAND ytec-windows-online-direct-clone-tests
)

add_executable(
  ytec-windows-online-direct-shrink-clone-tests
  Unit/windows_online_direct_shrink_clone_tests.cpp
)

target_link_libraries(
  ytec-windows-online-direct-shrink-clone-tests
  PRIVATE
    ytec::windows_app
)

ytec_configure_target(ytec-windows-online-direct-shrink-clone-tests)
add_test(
  NAME ytec-windows-online-direct-shrink-clone-tests
  COMMAND ytec-windows-online-direct-shrink-clone-tests
)

add_executable(
  ytec-windows-direct-shrink-clone-platform-tests
  Unit/windows_direct_shrink_clone_platform_tests.cpp
)

target_link_libraries(
  ytec-windows-direct-shrink-clone-platform-tests
  PRIVATE
    ytec::windows_app
)

ytec_configure_target(ytec-windows-direct-shrink-clone-platform-tests)
add_test(
  NAME ytec-windows-direct-shrink-clone-platform-tests
  COMMAND ytec-windows-direct-shrink-clone-platform-tests
)

add_executable(
  ytec-windows-app-layout-tests
  Unit/windows_app_layout_tests.cpp
  ${PROJECT_SOURCE_DIR}/src/WindowsApp/src/windows_app.rc
)

target_link_libraries(
  ytec-windows-app-layout-tests
  PRIVATE
    ytec::windows_app
)

ytec_configure_target(ytec-windows-app-layout-tests)
add_test(
  NAME ytec-windows-app-layout-tests
  COMMAND ytec-windows-app-layout-tests
)

add_executable(
  ytec-ui-support-font-tests
  Unit/ui_support_font_tests.cpp
  ${PROJECT_SOURCE_DIR}/src/UiSupport/resources/line_seed_fonts.rc
)

target_link_libraries(
  ytec-ui-support-font-tests
  PRIVATE
    ytec::ui_support
)

ytec_configure_target(ytec-ui-support-font-tests)
add_test(
  NAME ytec-ui-support-font-tests
  COMMAND ytec-ui-support-font-tests
)

add_executable(
  ytec-ui-support-error-presentation-tests
  Unit/ui_support_error_presentation_tests.cpp
)

target_link_libraries(
  ytec-ui-support-error-presentation-tests
  PRIVATE
    ytec::ui_support
)

ytec_configure_target(ytec-ui-support-error-presentation-tests)
add_test(
  NAME ytec-ui-support-error-presentation-tests
  COMMAND ytec-ui-support-error-presentation-tests
)

add_executable(
  ytec-windows-online-image-create-tests
  Unit/windows_online_image_create_tests.cpp
)

target_link_libraries(
  ytec-windows-online-image-create-tests
  PRIVATE
    ytec::windows_app
)

ytec_configure_target(ytec-windows-online-image-create-tests)
add_test(
  NAME ytec-windows-online-image-create-tests
  COMMAND ytec-windows-online-image-create-tests
)

add_executable(
  ytec-windows-online-image-restore-tests
  Unit/windows_online_image_restore_tests.cpp
)

target_link_libraries(
  ytec-windows-online-image-restore-tests
  PRIVATE
    ytec::windows_app
)

ytec_configure_target(ytec-windows-online-image-restore-tests)
add_test(
  NAME ytec-windows-online-image-restore-tests
  COMMAND ytec-windows-online-image-restore-tests
)

add_executable(
  ytec-windows-online-image-restore-operation-tests
  Unit/windows_online_image_restore_operation_tests.cpp
)

target_link_libraries(
  ytec-windows-online-image-restore-operation-tests
  PRIVATE
    ytec::windows_app
)

ytec_configure_target(ytec-windows-online-image-restore-operation-tests)
add_test(
  NAME ytec-windows-online-image-restore-operation-tests
  COMMAND ytec-windows-online-image-restore-operation-tests
)

add_executable(
  ytec-windows-system-state-tests
  Unit/windows_system_state_tests.cpp
)

target_link_libraries(
  ytec-windows-system-state-tests
  PRIVATE
    ytec::windows_app
)

ytec_configure_target(ytec-windows-system-state-tests)
add_test(
  NAME ytec-windows-system-state-tests
  COMMAND ytec-windows-system-state-tests
)

add_executable(
  ytec-manual-update-tests
  Unit/manual_update_tests.cpp
)

target_link_libraries(
  ytec-manual-update-tests
  PRIVATE
    ytec::windows_app
)

ytec_configure_target(ytec-manual-update-tests)
add_test(NAME ytec-manual-update-tests COMMAND ytec-manual-update-tests)

add_executable(
  ytec-media-preflight-tests
  Unit/media_preflight_tests.cpp
)

target_link_libraries(
  ytec-media-preflight-tests
  PRIVATE
    ytec::windows_app
)

ytec_configure_target(ytec-media-preflight-tests)
add_test(
  NAME ytec-media-preflight-tests
  COMMAND ytec-media-preflight-tests
)

add_executable(
  ytec-adk-acquisition-tests
  Unit/adk_acquisition_tests.cpp
)

target_link_libraries(
  ytec-adk-acquisition-tests
  PRIVATE
    ytec::windows_app
)

ytec_configure_target(ytec-adk-acquisition-tests)
add_test(
  NAME ytec-adk-acquisition-tests
  COMMAND ytec-adk-acquisition-tests
)

add_executable(
  ytec-adk-consent-review-tests
  Unit/adk_consent_review_tests.cpp
)

target_link_libraries(
  ytec-adk-consent-review-tests
  PRIVATE
    ytec::windows_app
)

ytec_configure_target(ytec-adk-consent-review-tests)
add_test(
  NAME ytec-adk-consent-review-tests
  COMMAND ytec-adk-consent-review-tests
)

add_executable(
  ytec-adk-management-tests
  Unit/adk_management_tests.cpp
)

target_link_libraries(
  ytec-adk-management-tests
  PRIVATE
    ytec::windows_app
)

ytec_configure_target(ytec-adk-management-tests)
add_test(
  NAME ytec-adk-management-tests
  COMMAND ytec-adk-management-tests
)

add_executable(
  ytec-windows-adk-eula-extractor-tests
  Unit/windows_adk_eula_extractor_tests.cpp
)

target_link_libraries(
  ytec-windows-adk-eula-extractor-tests
  PRIVATE
    ytec::windows_app
)

ytec_configure_target(ytec-windows-adk-eula-extractor-tests)
add_test(
  NAME ytec-windows-adk-eula-extractor-tests
  COMMAND ytec-windows-adk-eula-extractor-tests
)

add_executable(
  ytec-windows-data-rescue-clone-tests
  Unit/windows_data_rescue_clone_tests.cpp
)

target_link_libraries(
  ytec-windows-data-rescue-clone-tests
  PRIVATE
    ytec::windows_app
)

ytec_configure_target(ytec-windows-data-rescue-clone-tests)
add_test(
  NAME ytec-windows-data-rescue-clone-tests
  COMMAND ytec-windows-data-rescue-clone-tests
)

add_executable(
  ytec-adk-patch-archive-tests
  Unit/adk_patch_archive_tests.cpp
)

target_link_libraries(
  ytec-adk-patch-archive-tests
  PRIVATE
    ytec::windows_app
)

ytec_configure_target(ytec-adk-patch-archive-tests)
add_test(
  NAME ytec-adk-patch-archive-tests
  COMMAND ytec-adk-patch-archive-tests
)

add_executable(
  ytec-windows-adk-acquisition-platform-tests
  Unit/windows_adk_acquisition_platform_tests.cpp
)

target_link_libraries(
  ytec-windows-adk-acquisition-platform-tests
  PRIVATE
    ytec::windows_app
)

ytec_configure_target(ytec-windows-adk-acquisition-platform-tests)
add_test(
  NAME ytec-windows-adk-acquisition-platform-tests
  COMMAND ytec-windows-adk-acquisition-platform-tests
)

add_executable(
  ytec-media-wizard-tests
  Unit/media_wizard_tests.cpp
)

target_link_libraries(
  ytec-media-wizard-tests
  PRIVATE
    ytec::windows_app
)

ytec_configure_target(ytec-media-wizard-tests)
add_test(
  NAME ytec-media-wizard-tests
  COMMAND ytec-media-wizard-tests
)

add_executable(
  ytec-rescue-media-storage-tests
  Unit/rescue_media_storage_tests.cpp
)

target_link_libraries(
  ytec-rescue-media-storage-tests
  PRIVATE
    ytec::windows_app
)

ytec_configure_target(ytec-rescue-media-storage-tests)
add_test(
  NAME ytec-rescue-media-storage-tests
  COMMAND ytec-rescue-media-storage-tests
)

add_executable(
  ytec-rescue-media-inspection-tests
  Unit/rescue_media_inspection_tests.cpp
)

target_link_libraries(
  ytec-rescue-media-inspection-tests
  PRIVATE
    ytec::windows_app
)

ytec_configure_target(ytec-rescue-media-inspection-tests)
add_test(
  NAME ytec-rescue-media-inspection-tests
  COMMAND ytec-rescue-media-inspection-tests
)

add_executable(
  ytec-rescue-media-ui-tests
  Unit/rescue_media_ui_tests.cpp
)

target_link_libraries(
  ytec-rescue-media-ui-tests
  PRIVATE
    ytec::windows_app
)

ytec_configure_target(ytec-rescue-media-ui-tests)
add_test(
  NAME ytec-rescue-media-ui-tests
  COMMAND ytec-rescue-media-ui-tests
)

add_executable(
  ytec-usb-volume-mapping-tests
  Unit/usb_volume_mapping_tests.cpp
)

target_link_libraries(
  ytec-usb-volume-mapping-tests
  PRIVATE
    ytec::windows_app
)

ytec_configure_target(ytec-usb-volume-mapping-tests)
add_test(
  NAME ytec-usb-volume-mapping-tests
  COMMAND ytec-usb-volume-mapping-tests
)

add_executable(
  ytec-tsumugi-restore-preflight-tests
  Unit/tsumugi_restore_preflight_tests.cpp
)

target_link_libraries(
  ytec-tsumugi-restore-preflight-tests
  PRIVATE
    ytec::windows_app
)

ytec_configure_target(ytec-tsumugi-restore-preflight-tests)
add_test(
  NAME ytec-tsumugi-restore-preflight-tests
  COMMAND ytec-tsumugi-restore-preflight-tests
)

add_executable(
  ytec-mock-tests
  Integration/mock_inventory_tests.cpp
)

target_link_libraries(
  ytec-mock-tests
  PRIVATE
    ytec::cli_tools
)

ytec_configure_target(ytec-mock-tests)
add_test(NAME ytec-mock-tests COMMAND ytec-mock-tests)

add_test(NAME ytec-winpe-app-help COMMAND ytec-winpe-app --help)
set_tests_properties(
  ytec-winpe-app-help
  PROPERTIES
    PASS_REGULAR_EXPRESSION "ytec-winpe-app"
)

add_executable(
  ytec-winpe-app-tests
  Unit/winpe_direct_app_tests.cpp
)

target_link_libraries(
  ytec-winpe-app-tests
  PRIVATE
    ytec::winpe_app_logic
)

ytec_configure_target(ytec-winpe-app-tests)
add_test(NAME ytec-winpe-app-tests COMMAND ytec-winpe-app-tests)

add_executable(
  ytec-winpe-mbr2gpt-direct-app-tests
  Unit/winpe_mbr2gpt_direct_app_tests.cpp
)

target_link_libraries(
  ytec-winpe-mbr2gpt-direct-app-tests
  PRIVATE
    ytec::winpe_app_logic
)

ytec_configure_target(ytec-winpe-mbr2gpt-direct-app-tests)
add_test(
  NAME ytec-winpe-mbr2gpt-direct-app-tests
  COMMAND ytec-winpe-mbr2gpt-direct-app-tests
)

add_executable(
  ytec-winpe-rescue-clone-tests
  Unit/winpe_rescue_clone_tests.cpp
)

target_link_libraries(
  ytec-winpe-rescue-clone-tests
  PRIVATE
    ytec::winpe_app_logic
)

ytec_configure_target(ytec-winpe-rescue-clone-tests)
add_test(
  NAME ytec-winpe-rescue-clone-tests
  COMMAND ytec-winpe-rescue-clone-tests
)

add_executable(
  ytec-winpe-direct-image-create-tests
  Unit/winpe_direct_image_create_tests.cpp
)

target_link_libraries(
  ytec-winpe-direct-image-create-tests
  PRIVATE
    ytec::winpe_app_logic
)

ytec_configure_target(ytec-winpe-direct-image-create-tests)
add_test(
  NAME ytec-winpe-direct-image-create-tests
  COMMAND ytec-winpe-direct-image-create-tests
)

add_executable(
  ytec-winpe-direct-image-restore-tests
  Unit/winpe_direct_image_restore_tests.cpp
)

target_link_libraries(
  ytec-winpe-direct-image-restore-tests
  PRIVATE
    ytec::winpe_app_logic
)

ytec_configure_target(ytec-winpe-direct-image-restore-tests)
add_test(
  NAME ytec-winpe-direct-image-restore-tests
  COMMAND ytec-winpe-direct-image-restore-tests
)

add_executable(
  ytec-winpe-direct-shrink-image-restore-tests
  Unit/winpe_direct_shrink_image_restore_tests.cpp
)

target_link_libraries(
  ytec-winpe-direct-shrink-image-restore-tests
  PRIVATE
    ytec::winpe_app_logic
)

ytec_configure_target(ytec-winpe-direct-shrink-image-restore-tests)
add_test(
  NAME ytec-winpe-direct-shrink-image-restore-tests
  COMMAND ytec-winpe-direct-shrink-image-restore-tests
)

add_executable(
  ytec-winpe-direct-image-restore-resume-tests
  Unit/winpe_direct_image_restore_resume_tests.cpp
)

target_link_libraries(
  ytec-winpe-direct-image-restore-resume-tests
  PRIVATE
    ytec::winpe_app_logic
)

ytec_configure_target(ytec-winpe-direct-image-restore-resume-tests)
add_test(
  NAME ytec-winpe-direct-image-restore-resume-tests
  COMMAND ytec-winpe-direct-image-restore-resume-tests
)

add_executable(
  ytec-tsumugi-physical-restore-resume-tests
  Unit/tsumugi_physical_restore_resume_tests.cpp
)

target_link_libraries(
  ytec-tsumugi-physical-restore-resume-tests
  PRIVATE
    ytec::image_format
)

ytec_configure_target(ytec-tsumugi-physical-restore-resume-tests)
add_test(
  NAME ytec-tsumugi-physical-restore-resume-tests
  COMMAND ytec-tsumugi-physical-restore-resume-tests
)

add_executable(
  ytec-winpe-active-rescue-media-tests
  Unit/winpe_active_rescue_media_tests.cpp
)

target_link_libraries(
  ytec-winpe-active-rescue-media-tests
  PRIVATE
    ytec::winpe_app_logic
)

ytec_configure_target(ytec-winpe-active-rescue-media-tests)
add_test(
  NAME ytec-winpe-active-rescue-media-tests
  COMMAND ytec-winpe-active-rescue-media-tests
)

add_executable(
  ytec-winpe-image-create-ui-tests
  Unit/winpe_image_create_ui_tests.cpp
)

target_link_libraries(
  ytec-winpe-image-create-ui-tests
  PRIVATE
    ytec::winpe_app_logic
)

ytec_configure_target(ytec-winpe-image-create-ui-tests)
add_test(
  NAME ytec-winpe-image-create-ui-tests
  COMMAND ytec-winpe-image-create-ui-tests
)

add_executable(
  ytec-winpe-tsumugi-password-ui-tests
  Unit/winpe_tsumugi_password_ui_tests.cpp
)

target_link_libraries(
  ytec-winpe-tsumugi-password-ui-tests
  PRIVATE
    ytec::winpe_app_logic
)

ytec_configure_target(ytec-winpe-tsumugi-password-ui-tests)
add_test(
  NAME ytec-winpe-tsumugi-password-ui-tests
  COMMAND ytec-winpe-tsumugi-password-ui-tests
)

add_executable(
  ytec-winpe-image-restore-ui-tests
  Unit/winpe_image_restore_ui_tests.cpp
)

target_link_libraries(
  ytec-winpe-image-restore-ui-tests
  PRIVATE
    ytec::winpe_app_logic
)

ytec_configure_target(ytec-winpe-image-restore-ui-tests)
add_test(
  NAME ytec-winpe-image-restore-ui-tests
  COMMAND ytec-winpe-image-restore-ui-tests
)

add_executable(
  ytec-winpe-automatic-boot-repair-ui-tests
  Unit/winpe_automatic_boot_repair_ui_tests.cpp
)

target_link_libraries(
  ytec-winpe-automatic-boot-repair-ui-tests
  PRIVATE
    ytec::winpe_app_logic
)

target_compile_definitions(
  ytec-winpe-automatic-boot-repair-ui-tests
  PRIVATE
    YTEC_WINPE_GUI_SOURCE_PATH="${PROJECT_SOURCE_DIR}/src/WinPEApp/src/gui_main.cpp"
)

ytec_configure_target(ytec-winpe-automatic-boot-repair-ui-tests)
add_test(
  NAME ytec-winpe-automatic-boot-repair-ui-tests
  COMMAND ytec-winpe-automatic-boot-repair-ui-tests
)

add_executable(
  ytec-winpe-dashboard-tests
  Unit/winpe_dashboard_tests.cpp
)

target_link_libraries(
  ytec-winpe-dashboard-tests
  PRIVATE
    ytec::winpe_app_logic
)

ytec_configure_target(ytec-winpe-dashboard-tests)
add_test(NAME ytec-winpe-dashboard-tests COMMAND ytec-winpe-dashboard-tests)

add_executable(
  ytec-phase1-synthetic-tests
  Integration/synthetic_gpt_clone_tests.cpp
)

target_link_libraries(
  ytec-phase1-synthetic-tests
  PRIVATE
    ytec::clone_core
)

ytec_configure_target(ytec-phase1-synthetic-tests)
add_test(NAME ytec-phase1-synthetic-tests COMMAND ytec-phase1-synthetic-tests)

add_executable(
  ytec-boot-repair-tests
  Unit/boot_repair_tests.cpp
)

target_link_libraries(
  ytec-boot-repair-tests
  PRIVATE
    ytec::boot_repair
)

ytec_configure_target(ytec-boot-repair-tests)
add_test(NAME ytec-boot-repair-tests COMMAND ytec-boot-repair-tests)

add_executable(
  ytec-automatic-boot-repair-plan-tests
  Unit/automatic_boot_repair_plan_tests.cpp
)

target_link_libraries(
  ytec-automatic-boot-repair-plan-tests
  PRIVATE
    ytec::boot_repair
)

ytec_configure_target(ytec-automatic-boot-repair-plan-tests)
add_test(
  NAME ytec-automatic-boot-repair-plan-tests
  COMMAND ytec-automatic-boot-repair-plan-tests
)

add_executable(
  ytec-automatic-boot-repair-windows-tests
  Unit/automatic_boot_repair_windows_tests.cpp
)

target_link_libraries(
  ytec-automatic-boot-repair-windows-tests
  PRIVATE
    ytec::boot_repair
)

ytec_configure_target(ytec-automatic-boot-repair-windows-tests)
add_test(
  NAME ytec-automatic-boot-repair-windows-tests
  COMMAND ytec-automatic-boot-repair-windows-tests
)

add_executable(
  ytec-system-partition-creation-tests
  Unit/system_partition_creation_tests.cpp
)

target_link_libraries(
  ytec-system-partition-creation-tests
  PRIVATE
    ytec::boot_repair
)

ytec_configure_target(ytec-system-partition-creation-tests)
add_test(
  NAME ytec-system-partition-creation-tests
  COMMAND ytec-system-partition-creation-tests
)

add_executable(
  ytec-clone-boot-finalization-tests
  Unit/clone_boot_finalization_tests.cpp
)

target_link_libraries(
  ytec-clone-boot-finalization-tests
  PRIVATE
    ytec::boot_repair
)

ytec_configure_target(ytec-clone-boot-finalization-tests)
add_test(
  NAME ytec-clone-boot-finalization-tests
  COMMAND ytec-clone-boot-finalization-tests
)

add_executable(
  ytec-mbr2gpt-tests
  Unit/mbr2gpt_tests.cpp
)

target_link_libraries(
  ytec-mbr2gpt-tests
  PRIVATE
    ytec::boot_repair
)

ytec_configure_target(ytec-mbr2gpt-tests)
add_test(NAME ytec-mbr2gpt-tests COMMAND ytec-mbr2gpt-tests)

add_executable(
  ytec-mbr2gpt-layout-tests
  Unit/mbr2gpt_layout_tests.cpp
)

target_link_libraries(
  ytec-mbr2gpt-layout-tests
  PRIVATE
    ytec::boot_repair
)

ytec_configure_target(ytec-mbr2gpt-layout-tests)
add_test(
  NAME ytec-mbr2gpt-layout-tests
  COMMAND ytec-mbr2gpt-layout-tests
)

add_executable(
  ytec-mbr2gpt-execution-tests
  Unit/mbr2gpt_execution_tests.cpp
)

target_link_libraries(
  ytec-mbr2gpt-execution-tests
  PRIVATE
    ytec::boot_repair
)

ytec_configure_target(ytec-mbr2gpt-execution-tests)
add_test(
  NAME ytec-mbr2gpt-execution-tests
  COMMAND ytec-mbr2gpt-execution-tests
)

add_executable(
  ytec-winre-diagnostic-tests
  Unit/winre_diagnostic_tests.cpp
)

target_link_libraries(
  ytec-winre-diagnostic-tests
  PRIVATE
    ytec::boot_repair
)

ytec_configure_target(ytec-winre-diagnostic-tests)
add_test(
  NAME ytec-winre-diagnostic-tests
  COMMAND ytec-winre-diagnostic-tests
)

add_executable(
  ytec-winre-registration-tests
  Unit/winre_registration_tests.cpp
)

target_link_libraries(
  ytec-winre-registration-tests
  PRIVATE
    ytec::boot_repair
)

ytec_configure_target(ytec-winre-registration-tests)
add_test(
  NAME ytec-winre-registration-tests
  COMMAND ytec-winre-registration-tests
)

add_executable(
  ytec-vss-workflow-tests
  Unit/vss_workflow_tests.cpp
)

target_link_libraries(
  ytec-vss-workflow-tests
  PRIVATE
    ytec::vss_requester
)

ytec_configure_target(ytec-vss-workflow-tests)
add_test(NAME ytec-vss-workflow-tests COMMAND ytec-vss-workflow-tests)

add_executable(
  ytec-vss-diff-area-monitor-tests
  Unit/vss_diff_area_monitor_tests.cpp
)

target_link_libraries(
  ytec-vss-diff-area-monitor-tests
  PRIVATE
    ytec::vss_requester
)

ytec_configure_target(ytec-vss-diff-area-monitor-tests)
add_test(
  NAME ytec-vss-diff-area-monitor-tests
  COMMAND ytec-vss-diff-area-monitor-tests
)

add_executable(
  ytec-windows-bcdboot-trust-tests
  Integration/windows_bcdboot_trust_tests.cpp
)

target_link_libraries(
  ytec-windows-bcdboot-trust-tests
  PRIVATE
    ytec::boot_repair
)

ytec_configure_target(ytec-windows-bcdboot-trust-tests)
add_test(
  NAME ytec-windows-bcdboot-trust-tests
  COMMAND ytec-windows-bcdboot-trust-tests
)

add_executable(
  ytec-media-builder-tests
  Unit/media_builder_tests.cpp
)

target_link_libraries(
  ytec-media-builder-tests
  PRIVATE
    ytec::media_builder
)

ytec_configure_target(ytec-media-builder-tests)
add_test(NAME ytec-media-builder-tests COMMAND ytec-media-builder-tests)

add_executable(
  ytec-media-driver-selection-tests
  Unit/media_driver_selection_tests.cpp
)

target_link_libraries(
  ytec-media-driver-selection-tests
  PRIVATE
    ytec::media_builder
)

ytec_configure_target(ytec-media-driver-selection-tests)
add_test(
  NAME ytec-media-driver-selection-tests
  COMMAND ytec-media-driver-selection-tests
)

add_executable(
  ytec-windows-adk-detection-tests
  Integration/windows_adk_detection_tests.cpp
)

target_link_libraries(
  ytec-windows-adk-detection-tests
  PRIVATE
    ytec::media_builder
)

ytec_configure_target(ytec-windows-adk-detection-tests)
add_test(
  NAME ytec-windows-adk-detection-tests
  COMMAND ytec-windows-adk-detection-tests
)

add_test(
  NAME ytec-winpe-environment-json
  COMMAND ytec-winpe-environment --json
)
set_tests_properties(
  ytec-winpe-environment-json
  PROPERTIES
    PASS_REGULAR_EXPRESSION "\\\"schemaVersion\\\": 1"
)

add_executable(
  ytec-physical-disk-tests
  Unit/physical_disk_tests.cpp
)

target_link_libraries(
  ytec-physical-disk-tests
  PRIVATE
    ytec::disk_model
)

ytec_configure_target(ytec-physical-disk-tests)
add_test(NAME ytec-physical-disk-tests COMMAND ytec-physical-disk-tests)

add_executable(
  ytec-connection-identity-tests
  Unit/connection_identity_tests.cpp
)

target_link_libraries(
  ytec-connection-identity-tests
  PRIVATE
    ytec::disk_model
)

ytec_configure_target(ytec-connection-identity-tests)
add_test(
  NAME ytec-connection-identity-tests
  COMMAND ytec-connection-identity-tests
)

add_executable(
  ytec-image-format-tests
  Unit/image_format_tests.cpp
)

target_link_libraries(
  ytec-image-format-tests
  PRIVATE
    ytec::legacy_image_format
)

ytec_configure_target(ytec-image-format-tests)
add_test(NAME ytec-image-format-tests COMMAND ytec-image-format-tests)

add_executable(
  ytec-image-fuzz-tests
  ImageFuzz/image_fuzz_smoke.cpp
)

target_link_libraries(
  ytec-image-fuzz-tests
  PRIVATE
    ytec::image_format
)

ytec_configure_target(ytec-image-fuzz-tests)
add_test(NAME ytec-image-fuzz-tests COMMAND ytec-image-fuzz-tests)
set_tests_properties(ytec-image-fuzz-tests PROPERTIES TIMEOUT 120)

option(
  YTEC_BUILD_IMAGE_LIBFUZZER
  "Build the opt-in MSVC coverage-guided .tsumugi v1 fuzz target"
  OFF
)

if(YTEC_BUILD_IMAGE_LIBFUZZER)
  if(MSVC_VERSION LESS 1950)
    message(FATAL_ERROR
      "YTEC_BUILD_IMAGE_LIBFUZZER requires MSVC 19.50 or newer")
  endif()

  # Compile a parser-only twin so coverage instrumentation never changes the
  # production ImageFormat target or normal test executables in this build.
  add_library(
    ytec-image-libfuzzer-format
    STATIC
      ${PROJECT_SOURCE_DIR}/src/ImageFormat/src/compression.cpp
      ${PROJECT_SOURCE_DIR}/src/ImageFormat/src/partition_snapshot.cpp
      ${PROJECT_SOURCE_DIR}/src/ImageFormat/src/sha256.cpp
      ${PROJECT_SOURCE_DIR}/src/ImageFormat/src/tsumugi.cpp
      ${PROJECT_SOURCE_DIR}/src/ImageFormat/src/tsumugi_crypto.cpp
      ${PROJECT_SOURCE_DIR}/src/ImageFormat/src/tsumugi_manifest.cpp
  )

  target_include_directories(
    ytec-image-libfuzzer-format
    PUBLIC
      ${PROJECT_SOURCE_DIR}/src/ImageFormat/include
  )

  # CloneCore is deliberately reused from the normal MSVC build graph. Keep
  # the MSVC STL annotation ABI consistent across that boundary; ASan still
  # instruments the parser twin and harness themselves.
  target_compile_definitions(
    ytec-image-libfuzzer-format
    PRIVATE
      _DISABLE_STRING_ANNOTATION
      _DISABLE_VECTOR_ANNOTATION
  )

  target_link_libraries(
    ytec-image-libfuzzer-format
    PUBLIC
      ytec::clone_core
    PRIVATE
      bcrypt
      advapi32
      ytec::argon2
      ytec::zstd
  )

  ytec_configure_target(ytec-image-libfuzzer-format)
  target_compile_options(
    ytec-image-libfuzzer-format
    PRIVATE
      /fsanitize=fuzzer
      /fsanitize=address
  )

  add_executable(
    ytec-image-libfuzzer
    ImageFuzz/image_fuzz_libfuzzer.cpp
  )

  target_link_libraries(
    ytec-image-libfuzzer
    PRIVATE
      ytec-image-libfuzzer-format
  )

  ytec_configure_target(ytec-image-libfuzzer)
  target_compile_definitions(
    ytec-image-libfuzzer
    PRIVATE
      _DISABLE_STRING_ANNOTATION
      _DISABLE_VECTOR_ANNOTATION
  )
  target_compile_options(
    ytec-image-libfuzzer
    PRIVATE
      /fsanitize=fuzzer
      /fsanitize=address
  )
  target_link_options(
    ytec-image-libfuzzer
    PRIVATE
      /INCREMENTAL:NO
  )
endif()

add_executable(
  ytec-backup-manifest-tests
  Unit/backup_manifest_tests.cpp
)

target_link_libraries(
  ytec-backup-manifest-tests
  PRIVATE
    ytec::image_format
)

ytec_configure_target(ytec-backup-manifest-tests)
add_test(
  NAME ytec-backup-manifest-tests
  COMMAND ytec-backup-manifest-tests
)

add_executable(
  ytec-shrink-image-manifest-tests
  Unit/shrink_image_manifest_tests.cpp
)

target_link_libraries(
  ytec-shrink-image-manifest-tests
  PRIVATE
    ytec::legacy_image_format
)

ytec_configure_target(ytec-shrink-image-manifest-tests)
add_test(
  NAME ytec-shrink-image-manifest-tests
  COMMAND ytec-shrink-image-manifest-tests
)

add_executable(
  ytec-migration-engine-tests
  Unit/migration_engine_tests.cpp
)

target_link_libraries(
  ytec-migration-engine-tests
  PRIVATE
    ytec::migration_engine
    ytec::windows_dism
)

ytec_configure_target(ytec-migration-engine-tests)
add_test(
  NAME ytec-migration-engine-tests
  COMMAND ytec-migration-engine-tests
)

add_executable(
  ytec-partition-snapshot-capture-tests
  Unit/partition_snapshot_capture_tests.cpp
)

target_link_libraries(
  ytec-partition-snapshot-capture-tests
  PRIVATE
    ytec::image_format
)

ytec_configure_target(ytec-partition-snapshot-capture-tests)
add_test(
  NAME ytec-partition-snapshot-capture-tests
  COMMAND ytec-partition-snapshot-capture-tests
)

add_executable(
  ytec-dcimg-stream-tests
  Unit/dcimg_stream_tests.cpp
)

target_link_libraries(
  ytec-dcimg-stream-tests
  PRIVATE
    ytec::legacy_image_format
)

ytec_configure_target(ytec-dcimg-stream-tests)
add_test(NAME ytec-dcimg-stream-tests COMMAND ytec-dcimg-stream-tests)

add_executable(
  ytec-windows-file-staging-tests
  Unit/windows_file_staging_tests.cpp
)

target_link_libraries(
  ytec-windows-file-staging-tests
  PRIVATE
    ytec::legacy_image_format
)

ytec_configure_target(ytec-windows-file-staging-tests)
add_test(
  NAME ytec-windows-file-staging-tests
  COMMAND ytec-windows-file-staging-tests
)

add_executable(
  ytec-windows-tsumugi-destination-tests
  Unit/windows_tsumugi_destination_tests.cpp
)

target_link_libraries(
  ytec-windows-tsumugi-destination-tests
  PRIVATE
    ytec::image_format
)

ytec_configure_target(ytec-windows-tsumugi-destination-tests)
add_test(
  NAME ytec-windows-tsumugi-destination-tests
  COMMAND ytec-windows-tsumugi-destination-tests
)

add_executable(
  ytec-windows-tsumugi-rescue-staging-tests
  Unit/windows_tsumugi_rescue_staging_tests.cpp
)

target_link_libraries(
  ytec-windows-tsumugi-rescue-staging-tests
  PRIVATE
    ytec::image_format
)

ytec_configure_target(ytec-windows-tsumugi-rescue-staging-tests)
add_test(
  NAME ytec-windows-tsumugi-rescue-staging-tests
  COMMAND ytec-windows-tsumugi-rescue-staging-tests
)

add_executable(
  ytec-restore-image-read-session-tests
  Unit/restore_image_read_session_tests.cpp
)

target_link_libraries(
  ytec-restore-image-read-session-tests
  PRIVATE
    ytec::legacy_image_format
)

ytec_configure_target(ytec-restore-image-read-session-tests)
add_test(
  NAME ytec-restore-image-read-session-tests
  COMMAND ytec-restore-image-read-session-tests
)

add_executable(
  ytec-vss-snapshot-image-tests
  Unit/vss_snapshot_image_tests.cpp
)

target_link_libraries(
  ytec-vss-snapshot-image-tests
  PRIVATE
    ytec::legacy_vss_image
)

ytec_configure_target(ytec-vss-snapshot-image-tests)
add_test(
  NAME ytec-vss-snapshot-image-tests
  COMMAND ytec-vss-snapshot-image-tests
)

add_executable(
  ytec-tsumugi-snapshot-tests
  Unit/tsumugi_snapshot_tests.cpp
)

target_link_libraries(
  ytec-tsumugi-snapshot-tests
  PRIVATE
    ytec::vss_requester
)

ytec_configure_target(ytec-tsumugi-snapshot-tests)
add_test(
  NAME ytec-tsumugi-snapshot-tests
  COMMAND ytec-tsumugi-snapshot-tests
)

add_executable(
  ytec-vss-snapshot-plan-tests
  Unit/vss_snapshot_plan_tests.cpp
)

target_link_libraries(
  ytec-vss-snapshot-plan-tests
  PRIVATE
    ytec::vss_requester
)

ytec_configure_target(ytec-vss-snapshot-plan-tests)
add_test(
  NAME ytec-vss-snapshot-plan-tests
  COMMAND ytec-vss-snapshot-plan-tests
)

add_executable(
  ytec-online-backup-tests
  Unit/online_backup_tests.cpp
)

target_link_libraries(
  ytec-online-backup-tests
  PRIVATE
    ytec::legacy_vss_image
)

ytec_configure_target(ytec-online-backup-tests)
add_test(
  NAME ytec-online-backup-tests
  COMMAND ytec-online-backup-tests
)

add_executable(
  ytec-phase2-synthetic-restore-tests
  Integration/synthetic_image_restore_tests.cpp
)

target_link_libraries(
  ytec-phase2-synthetic-restore-tests
  PRIVATE
    ytec::legacy_image_format
)

ytec_configure_target(ytec-phase2-synthetic-restore-tests)
add_test(
  NAME ytec-phase2-synthetic-restore-tests
  COMMAND ytec-phase2-synthetic-restore-tests
)

add_executable(
  ytec-mbr-tests
  Unit/mbr_tests.cpp
)

target_link_libraries(
  ytec-mbr-tests
  PRIVATE
    ytec::clone_core
)

ytec_configure_target(ytec-mbr-tests)
add_test(NAME ytec-mbr-tests COMMAND ytec-mbr-tests)

add_executable(
  ytec-phase3-synthetic-tests
  Integration/synthetic_mbr_clone_tests.cpp
)

target_link_libraries(
  ytec-phase3-synthetic-tests
  PRIVATE
    ytec::clone_core
)

ytec_configure_target(ytec-phase3-synthetic-tests)
add_test(NAME ytec-phase3-synthetic-tests COMMAND ytec-phase3-synthetic-tests)

if(YTEC_BUILD_DESTRUCTIVE_VM_TESTS)
  add_library(
    ytec-vm-clone-execution
    STATIC
      VM/vm_clone_execution_service.cpp
  )

  target_include_directories(
    ytec-vm-clone-execution
    PUBLIC
      ${CMAKE_CURRENT_SOURCE_DIR}/VM
  )

  target_link_libraries(
    ytec-vm-clone-execution
    PUBLIC
      ytec::disk_model
      ytec::winpe_app_logic
  )

  target_compile_definitions(
    ytec-vm-clone-execution
    PRIVATE
      YTEC_VM_DESTRUCTIVE_TEST_ONLY=1
  )

  ytec_configure_target(ytec-vm-clone-execution)

  add_executable(
    ytec-phase1-physical-clone-vm
    VM/physical_clone_vm.cpp
  )

  target_link_libraries(
    ytec-phase1-physical-clone-vm
    PRIVATE
      ytec-vm-clone-execution
  )

  target_compile_definitions(
    ytec-phase1-physical-clone-vm
    PRIVATE
      YTEC_VM_DESTRUCTIVE_TEST_ONLY=1
  )

  ytec_configure_target(ytec-phase1-physical-clone-vm)

  add_executable(
    ytec-product-mbr2gpt-verifier-vm
    VM/product_mbr2gpt_verifier_vm.cpp
  )

  target_link_libraries(
    ytec-product-mbr2gpt-verifier-vm
    PRIVATE
      ytec-vm-clone-execution
  )

  target_compile_definitions(
    ytec-product-mbr2gpt-verifier-vm
    PRIVATE
      YTEC_VM_DESTRUCTIVE_TEST_ONLY=1
  )

  ytec_configure_target(ytec-product-mbr2gpt-verifier-vm)

  add_executable(
    ytec-phase1-bcdboot-vm
    VM/boot_repair_vm.cpp
  )

  target_link_libraries(
    ytec-phase1-bcdboot-vm
    PRIVATE
      ytec::boot_repair
      ytec::disk_model
  )

  target_compile_definitions(
    ytec-phase1-bcdboot-vm
    PRIVATE
      YTEC_VM_DESTRUCTIVE_TEST_ONLY=1
  )

  ytec_configure_target(ytec-phase1-bcdboot-vm)

  add_executable(
    ytec-phase3-bcdboot-vm
    VM/mbr_boot_repair_vm.cpp
  )

  target_link_libraries(
    ytec-phase3-bcdboot-vm
    PRIVATE
      ytec-vm-clone-execution
      ytec::boot_repair
  )

  target_compile_definitions(
    ytec-phase3-bcdboot-vm
    PRIVATE
      YTEC_VM_DESTRUCTIVE_TEST_ONLY=1
  )

  ytec_configure_target(ytec-phase3-bcdboot-vm)

  add_executable(
    ytec-phase4-mbr2gpt-vm
    VM/mbr2gpt_vm.cpp
  )

  target_link_libraries(
    ytec-phase4-mbr2gpt-vm
    PRIVATE
      ytec-vm-clone-execution
      ytec::boot_repair
  )

  target_compile_definitions(
    ytec-phase4-mbr2gpt-vm
    PRIVATE
      YTEC_VM_DESTRUCTIVE_TEST_ONLY=1
  )

  ytec_configure_target(ytec-phase4-mbr2gpt-vm)

  add_executable(
    ytec-phase4-bcdboot-vm
    VM/boot_repair_vm.cpp
  )

  target_link_libraries(
    ytec-phase4-bcdboot-vm
    PRIVATE
      ytec::boot_repair
      ytec::disk_model
  )

  target_compile_definitions(
    ytec-phase4-bcdboot-vm
    PRIVATE
      YTEC_VM_DESTRUCTIVE_TEST_ONLY=1
      YTEC_VM_PHASE4_BCDBOOT=1
      YTEC_VM_BCDBOOT_TARGET_GIB=56
  )

  ytec_configure_target(ytec-phase4-bcdboot-vm)

  add_executable(
    ytec-phase5-vss-live-vm
    VM/vss_live_validation.cpp
  )

  target_link_libraries(
    ytec-phase5-vss-live-vm
    PRIVATE
      ytec::vss_requester
  )

  target_compile_definitions(
    ytec-phase5-vss-live-vm
    PRIVATE
      YTEC_VM_DESTRUCTIVE_TEST_ONLY=1
  )

  ytec_configure_target(ytec-phase5-vss-live-vm)
  if(MSVC AND YTEC_ENABLE_MSVC_ANALYZE)
    # Windows SDK 10.0.26100のRegOpenKeyExW SAL注釈自体がC6553を出す。
    # 抑制はVirtualBox判定を持つVM専用ハーネス1ターゲットだけに限定する。
    target_compile_options(ytec-phase5-vss-live-vm PRIVATE /wd6553)
  endif()

  add_executable(
    ytec-phase5-file-staging-vm
    VM/file_staging_validation.cpp
  )

  target_link_libraries(
    ytec-phase5-file-staging-vm
    PRIVATE
      advapi32
      ytec::disk_model
      ytec::legacy_image_format
  )

  target_compile_definitions(
    ytec-phase5-file-staging-vm
    PRIVATE
      YTEC_VM_DESTRUCTIVE_TEST_ONLY=1
  )

  ytec_configure_target(ytec-phase5-file-staging-vm)
  if(MSVC AND YTEC_ENABLE_MSVC_ANALYZE)
    # Windows SDK 10.0.26100のRegOpenKeyExW SAL注釈自体がC6553を出す。
    # 抑制はVirtualBox判定を持つVM専用ハーネス1ターゲットだけに限定する。
    target_compile_options(ytec-phase5-file-staging-vm PRIVATE /wd6553)
  endif()

endif()

add_executable(
  ytec-startup-data-policy-tests
  Unit/startup_data_policy_tests.cpp
)

target_link_libraries(
  ytec-startup-data-policy-tests
  PRIVATE
    ytec::windows_app
)

ytec_configure_target(ytec-startup-data-policy-tests)
add_test(
  NAME ytec-startup-data-policy-tests
  COMMAND ytec-startup-data-policy-tests
)

add_executable(
  ytec-windows-first-run-guidance-tests
  Unit/windows_first_run_guidance_tests.cpp
)

target_include_directories(
  ytec-windows-first-run-guidance-tests
  PRIVATE
    ${PROJECT_SOURCE_DIR}/src/WindowsApp/src
)

target_link_libraries(
  ytec-windows-first-run-guidance-tests
  PRIVATE
    ytec::windows_app
)

target_compile_definitions(
  ytec-windows-first-run-guidance-tests
  PRIVATE
    YTEC_WINDOWS_APP_MAIN_SOURCE_PATH="${PROJECT_SOURCE_DIR}/src/WindowsApp/src/main.cpp"
)

ytec_configure_target(ytec-windows-first-run-guidance-tests)
add_test(
  NAME ytec-windows-first-run-guidance-tests
  COMMAND ytec-windows-first-run-guidance-tests
)

add_executable(
  ytec-log-retention-tests
  Unit/log_retention_tests.cpp
)

target_link_libraries(
  ytec-log-retention-tests
  PRIVATE
    ytec::windows_app
)

ytec_configure_target(ytec-log-retention-tests)
add_test(
  NAME ytec-log-retention-tests
  COMMAND ytec-log-retention-tests
)

add_executable(
  ytec-support-zip-tests
  Unit/support_zip_tests.cpp
)

target_link_libraries(
  ytec-support-zip-tests
  PRIVATE
    ytec::windows_app
)

ytec_configure_target(ytec-support-zip-tests)
add_test(
  NAME ytec-support-zip-tests
  COMMAND ytec-support-zip-tests
)

add_executable(
  ytec-support-zip-ui-tests
  Unit/support_zip_ui_tests.cpp
)

target_link_libraries(
  ytec-support-zip-ui-tests
  PRIVATE
    ytec::windows_app
)

ytec_configure_target(ytec-support-zip-ui-tests)
add_test(
  NAME ytec-support-zip-ui-tests
  COMMAND ytec-support-zip-ui-tests
)

add_executable(
  ytec-disk-health-tests
  Unit/disk_health_tests.cpp
)

target_link_libraries(
  ytec-disk-health-tests
  PRIVATE
    ytec::disk_model
)

ytec_configure_target(ytec-disk-health-tests)
add_test(
  NAME ytec-disk-health-tests
  COMMAND ytec-disk-health-tests
)

add_executable(
  ytec-windows-online-shrink-image-tests
  Unit/windows_online_shrink_image_tests.cpp
)

target_link_libraries(
  ytec-windows-online-shrink-image-tests
  PRIVATE
    ytec::windows_app
)

ytec_configure_target(ytec-windows-online-shrink-image-tests)
add_test(
  NAME ytec-windows-online-shrink-image-tests
  COMMAND ytec-windows-online-shrink-image-tests
)

add_executable(
  ytec-windows-online-shrink-image-plan-tests
  Unit/windows_online_shrink_image_plan_tests.cpp
)

target_link_libraries(
  ytec-windows-online-shrink-image-plan-tests
  PRIVATE
    ytec::windows_app
)

ytec_configure_target(ytec-windows-online-shrink-image-plan-tests)
add_test(
  NAME ytec-windows-online-shrink-image-plan-tests
  COMMAND ytec-windows-online-shrink-image-plan-tests
)

add_executable(
  ytec-windows-shrink-restore-transaction-tests
  Unit/windows_shrink_restore_transaction_tests.cpp
)

target_link_libraries(
  ytec-windows-shrink-restore-transaction-tests
  PRIVATE
    ytec::windows_app
)

ytec_configure_target(ytec-windows-shrink-restore-transaction-tests)
add_test(
  NAME ytec-windows-shrink-restore-transaction-tests
  COMMAND ytec-windows-shrink-restore-transaction-tests
)

add_executable(
  ytec-windows-shrink-restore-platform-tests
  Unit/windows_shrink_restore_platform_tests.cpp
)

target_link_libraries(
  ytec-windows-shrink-restore-platform-tests
  PRIVATE
    ytec::windows_app
)

ytec_configure_target(ytec-windows-shrink-restore-platform-tests)
add_test(
  NAME ytec-windows-shrink-restore-platform-tests
  COMMAND ytec-windows-shrink-restore-platform-tests
)

add_executable(
  ytec-windows-online-shrink-image-restore-tests
  Unit/windows_online_shrink_image_restore_tests.cpp
)

target_link_libraries(
  ytec-windows-online-shrink-image-restore-tests
  PRIVATE
    ytec::windows_app
)

ytec_configure_target(ytec-windows-online-shrink-image-restore-tests)
add_test(
  NAME ytec-windows-online-shrink-image-restore-tests
  COMMAND ytec-windows-online-shrink-image-restore-tests
)

add_executable(
  ytec-windows-ntfs-shrink-capture-tests
  Unit/windows_ntfs_shrink_capture_tests.cpp
)

target_link_libraries(
  ytec-windows-ntfs-shrink-capture-tests
  PRIVATE
    ytec::windows_app
)

ytec_configure_target(ytec-windows-ntfs-shrink-capture-tests)
add_test(
  NAME ytec-windows-ntfs-shrink-capture-tests
  COMMAND ytec-windows-ntfs-shrink-capture-tests
)

add_executable(
  ytec-nvram-repair-tests
  Unit/nvram_repair_tests.cpp
)

target_link_libraries(
  ytec-nvram-repair-tests
  PRIVATE
    ytec::boot_repair
)

ytec_configure_target(ytec-nvram-repair-tests)
add_test(
  NAME ytec-nvram-repair-tests
  COMMAND ytec-nvram-repair-tests
)

add_executable(
  ytec-efi-delete-transaction-tests
  Unit/efi_delete_transaction_tests.cpp
)

target_link_libraries(
  ytec-efi-delete-transaction-tests
  PRIVATE
    ytec::boot_repair
)

ytec_configure_target(ytec-efi-delete-transaction-tests)
add_test(
  NAME ytec-efi-delete-transaction-tests
  COMMAND ytec-efi-delete-transaction-tests
)

add_executable(
  ytec-efi-delete-transaction-windows-tests
  Unit/efi_delete_transaction_windows_tests.cpp
)

target_link_libraries(
  ytec-efi-delete-transaction-windows-tests
  PRIVATE
    ytec::boot_repair
)

ytec_configure_target(ytec-efi-delete-transaction-windows-tests)
add_test(
  NAME ytec-efi-delete-transaction-windows-tests
  COMMAND ytec-efi-delete-transaction-windows-tests
)
