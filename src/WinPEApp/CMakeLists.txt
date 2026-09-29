add_library(
  WinPEAppLogic
  STATIC
    src/active_rescue_media.cpp
    src/app_runner.cpp
    src/automatic_boot_repair_ui.cpp
    src/clone_execution_readiness.cpp
    src/clone_execution_service.cpp
    src/completion_power_ui.cpp
    src/dashboard.cpp
    src/direct_image_create.cpp
    src/direct_image_create_resume.cpp
    src/direct_image_restore.cpp
    src/direct_shrink_image_restore.cpp
    src/direct_image_restore_resume.cpp
    src/direct_image_restore_resume_backend.cpp
    src/direct_image_restore_resume_storage.cpp
    src/image_create_ui.cpp
    src/image_restore_ui.cpp
    src/mbr2gpt_direct_execution_service.cpp
    src/offline_ntfs_direct_shrink_clone.cpp
    src/offline_ntfs_direct_shrink_product.cpp
    src/product_io_policy.cpp
    src/repair_layout.cpp
    src/resume_slot_admission.cpp
    src/rescue_clone.cpp
    src/tsumugi_password_ui.cpp
    src/winre_diagnostic_view.cpp
)

add_library(ytec::winpe_app_logic ALIAS WinPEAppLogic)

target_include_directories(
  WinPEAppLogic
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
  WinPEAppLogic
  PUBLIC
    ytec::boot_repair
    ytec::cli_tools
    ytec::disk_model
    ytec::direct_shrink_target
    ytec::image_format
    ytec::operation_core
    ytec::windows_shrink_restore
    ytec::windows_shrink_analysis
  PRIVATE
    Advapi32
    Ole32
    Uuid
)

ytec_configure_target(WinPEAppLogic)

target_compile_definitions(
  WinPEAppLogic
  PUBLIC
    YTEC_PROJECT_VERSION="${YTEC_PRODUCT_VERSION}"
)

add_executable(
  ytec-winpe-app
  src/main.cpp
)

target_link_libraries(
  ytec-winpe-app
  PRIVATE
    ytec::winpe_app_logic
)

target_compile_definitions(
  ytec-winpe-app
  PRIVATE
    YTEC_WINPE_PRODUCT_BOUNDARY=1
)

ytec_configure_target(ytec-winpe-app)

add_executable(
  ytec-winpe-gui
  WIN32
    src/gui_main.cpp
    src/winpe_gui.manifest
    src/winpe_gui.rc
    ${YTEC_LINE_SEED_FONT_RESOURCE}
)

target_link_libraries(
  ytec-winpe-gui
  PRIVATE
    Advapi32
    Comdlg32
    Comctl32
    ytec::winpe_app_logic
    ytec::ui_support
)

target_compile_definitions(
  ytec-winpe-gui
  PRIVATE
    UNICODE
    _UNICODE
    YTEC_WINPE_PRODUCT_BOUNDARY=1
)

ytec_configure_target(ytec-winpe-gui)

# Host-side UI acceptance build.  The product GUI source is unchanged except
# that physical-disk inventory is compiled out for this non-elevated target.
add_executable(
  ytec-winpe-gui-ui-acceptance
  WIN32
    src/gui_main.cpp
    src/winpe_gui.manifest
    src/winpe_gui.rc
    ${YTEC_LINE_SEED_FONT_RESOURCE}
)

target_link_libraries(
  ytec-winpe-gui-ui-acceptance
  PRIVATE
    Advapi32
    Comdlg32
    Comctl32
    ytec::winpe_app_logic
    ytec::ui_support
)

target_compile_definitions(
  ytec-winpe-gui-ui-acceptance
  PRIVATE
    UNICODE
    _UNICODE
    YTEC_WINPE_PRODUCT_BOUNDARY=1
    YTEC_UI_ACCEPTANCE_BUILD=1
)

ytec_configure_target(ytec-winpe-gui-ui-acceptance)

if(MSVC AND YTEC_ENABLE_MSVC_ANALYZE)
  # Windows SDK 10.0.26100 applies a value-incompatible SAL annotation to
  # RegOpenKeyExW parameter 3. Keep this SDK-only false-positive suppression
  # scoped to the two GUI translation units which perform the MiniNT gate.
  target_compile_options(ytec-winpe-gui PRIVATE /wd6553)
  target_compile_options(ytec-winpe-gui-ui-acceptance PRIVATE /wd6553)
endif()
