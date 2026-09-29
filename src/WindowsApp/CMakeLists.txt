add_library(
  WindowsShrinkRestore
  STATIC
    src/shrink_restore_transaction.cpp
    src/shrink_work_placement.cpp
    src/windows_shrink_restore_platform.cpp
)

add_library(ytec::windows_shrink_restore ALIAS WindowsShrinkRestore)

target_include_directories(
  WindowsShrinkRestore
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
  WindowsShrinkRestore
  PUBLIC
    ytec::clone_core
    ytec::disk_model
    ytec::image_format
  PRIVATE
    Advapi32
    Ole32
    Uuid
    ytec::boot_repair
    ytec::windows_dism
)

ytec_configure_target(WindowsShrinkRestore)

add_library(
  WindowsAppLogic
  STATIC
    src/adk_acquisition.cpp
    src/adk_consent_review.cpp
    src/adk_management.cpp
    src/adk_product_ui.cpp
    src/adk_patch_archive.cpp
    src/clone_partition_capacity_review.cpp
    src/completion_power_ui.cpp
    src/first_run_guidance.cpp
    src/layout.cpp
    src/log_retention.cpp
    src/manual_update.cpp
    src/media_creation.cpp
    src/media_preflight.cpp
    src/media_wizard.cpp
    src/online_direct_clone.cpp
    src/online_direct_clone_operation.cpp
    src/online_direct_shrink_clone.cpp
    src/online_image_create.cpp
    src/online_image_restore.cpp
    src/online_image_restore_operation.cpp
    src/online_shrink_image.cpp
    src/online_shrink_image_plan.cpp
    src/online_shrink_image_product.cpp
    src/online_shrink_image_restore.cpp
    src/post_migration_check.cpp
    src/progress.cpp
    src/rescue_media_inspection.cpp
    src/rescue_media_storage.cpp
    src/rescue_media_ui.cpp
    src/resume_slot_product.cpp
    src/restore_preflight.cpp
    src/selection.cpp
    src/startup_data_policy.cpp
    src/support_zip.cpp
    src/support_zip_ui.cpp
    src/system_state.cpp
    src/usb_volume_mapping.cpp
    src/windows_adk_acquisition_platform.cpp
    src/windows_adk_eula_extractor.cpp
    src/windows_adk_management_platform.cpp
    src/windows_data_rescue_clone.cpp
    src/windows_direct_shrink_clone_platform.cpp
    src/windows_ntfs_shrink_capture.cpp
)

add_library(ytec::windows_app ALIAS WindowsAppLogic)

target_include_directories(
  WindowsAppLogic
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
  WindowsAppLogic
  PUBLIC
    ytec::clone_core
    ytec::disk_model
    ytec::image_format
    ytec::media_builder
    ytec::operation_core
    ytec::vss_requester
    ytec::windows_dism
    ytec::windows_shrink_restore
    ytec::windows_shrink_analysis
  PRIVATE
    advapi32
    cabinet
    msi
    ole32
    oleaut32
    version
    wbemuuid
    winhttp
    xmllite
    ytec::boot_repair
    wuguid
)

set(
  YTEC_MEDIA_BUILDER_SCRIPT
  "${PROJECT_SOURCE_DIR}/scripts/New-WinPEAppValidationMedia.ps1"
)
file(
  SHA256
  "${YTEC_MEDIA_BUILDER_SCRIPT}"
  YTEC_MEDIA_BUILDER_SHA256
)
file(
  SIZE
  "${YTEC_MEDIA_BUILDER_SCRIPT}"
  YTEC_MEDIA_BUILDER_SIZE
)
target_compile_definitions(
  WindowsAppLogic
  PRIVATE
    YTEC_MEDIA_BUILDER_SHA256="${YTEC_MEDIA_BUILDER_SHA256}"
    YTEC_MEDIA_BUILDER_SIZE=${YTEC_MEDIA_BUILDER_SIZE}
)

ytec_configure_target(WindowsAppLogic)

add_executable(
  ytec-tsumugi-drive
  WIN32
    src/main.cpp
    src/windows_app.manifest
    src/windows_app.rc
    ${YTEC_LINE_SEED_FONT_RESOURCE}
)

target_link_libraries(
  ytec-tsumugi-drive
  PRIVATE
    ytec::windows_app
    ytec::ui_support
    comdlg32
    comctl32
    dwmapi
    ntdll
    shell32
    shcore
    uxtheme
)

target_compile_definitions(
  ytec-tsumugi-drive
  PRIVATE
    UNICODE
    _UNICODE
    YTEC_PRODUCT_VERSION="${YTEC_PRODUCT_VERSION}"
)

# Keep the linker's generated UAC fragment identical to windows_app.manifest;
# otherwise mt.exe correctly rejects conflicting asInvoker/administrator
# snippets instead of silently choosing one.
target_link_options(
  ytec-tsumugi-drive
  PRIVATE
    "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'"
)

ytec_configure_target(ytec-tsumugi-drive)

# Host-side UI acceptance build.  It reuses the product window source and
# resources, but compiles out disk inventory and runs without elevation so
# display/DPI/keyboard checks cannot touch a physical disk.
add_executable(
  ytec-tsumugi-drive-ui-acceptance
  WIN32
    src/main.cpp
    src/windows_app.rc
    ${YTEC_LINE_SEED_FONT_RESOURCE}
)

target_link_libraries(
  ytec-tsumugi-drive-ui-acceptance
  PRIVATE
    ytec::windows_app
    ytec::ui_support
    comdlg32
    comctl32
    dwmapi
    ntdll
    shell32
    shcore
    uxtheme
)

target_compile_definitions(
  ytec-tsumugi-drive-ui-acceptance
  PRIVATE
    UNICODE
    _UNICODE
    YTEC_UI_ACCEPTANCE_BUILD=1
    YTEC_PRODUCT_VERSION="${YTEC_PRODUCT_VERSION}"
)

target_link_options(
  ytec-tsumugi-drive-ui-acceptance
  PRIVATE
    "/MANIFESTUAC:level='asInvoker' uiAccess='false'"
)

ytec_configure_target(ytec-tsumugi-drive-ui-acceptance)
