add_library(
  MigrationEngine
  STATIC
    src/bundle_capture.cpp
    src/restore_inspection.cpp
    src/shrink_bundle.cpp
    src/source_analysis.cpp
    src/target_layout.cpp
    src/target_layout_io.cpp
    src/volume_apply.cpp
    src/windows_file_system_recreate.cpp
    src/windows_file_system_recreate_target_io.cpp
)

add_library(ytec::migration_engine ALIAS MigrationEngine)

target_include_directories(
  MigrationEngine
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
  MigrationEngine
  PUBLIC
    ytec::boot_repair
    ytec::disk_model
    ytec::image_format
    ytec::legacy_image_format
    ytec::migration_core
    ytec::windows_shrink_analysis
  PRIVATE
    Ole32
    Uuid
    bcrypt
    ytec::clone_core
    ytec::windows_dism
)

ytec_configure_target(MigrationEngine)
