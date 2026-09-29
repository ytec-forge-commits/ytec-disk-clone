add_library(
  ImageFormat
  STATIC
    src/backup_manifest.cpp
    src/compression.cpp
    src/partition_snapshot.cpp
    src/sha256.cpp
    src/tsumugi.cpp
    src/tsumugi_crypto.cpp
    src/tsumugi_manifest.cpp
    src/tsumugi_image_service.cpp
    src/tsumugi_physical_restore.cpp
    src/tsumugi_physical_restore_resume.cpp
    src/tsumugi_restore_layout.cpp
    src/tsumugi_restore_layout_io.cpp
    src/tsumugi_restore_transaction.cpp
    src/tsumugi_stream.cpp
    src/windows_tsumugi_destination.cpp
    src/windows_tsumugi_rescue_staging.cpp
)

add_library(ytec::image_format ALIAS ImageFormat)

target_include_directories(
  ImageFormat
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
  ImageFormat
  PUBLIC
    ytec::clone_core
    ytec::disk_model
    ytec::migration_core
  PRIVATE
    bcrypt
    advapi32
    ytec::argon2
    ytec::zstd
)

ytec_configure_target(ImageFormat)

# Quarantined pre-1.0 formats. Product executables must not link this target;
# it exists only until the remaining legacy regression fixtures are retired.
add_library(
  LegacyImageFormat
  STATIC
    src/dcimg.cpp
    src/image_inspection.cpp
    src/restore.cpp
    src/restore_image_inspection.cpp
    src/shrink_image_manifest.cpp
    src/windows_file_staging.cpp
)

add_library(ytec::legacy_image_format ALIAS LegacyImageFormat)

target_include_directories(
  LegacyImageFormat
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
  LegacyImageFormat
  PUBLIC
    ytec::image_format
)

ytec_configure_target(LegacyImageFormat)
