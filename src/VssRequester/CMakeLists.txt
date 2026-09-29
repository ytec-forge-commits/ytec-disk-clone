add_library(
  VssRequester
  STATIC
    src/workflow.cpp
    src/snapshot_reader.cpp
    src/snapshot_metadata.cpp
    src/snapshot_plan.cpp
    src/tsumugi_snapshot.cpp
    src/online_tsumugi_backup.cpp
    src/diff_area_monitor.cpp
    src/windows_diff_area_observer.cpp
    src/windows_backend.cpp
)

add_library(ytec::vss_requester ALIAS VssRequester)

target_include_directories(
  VssRequester
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
  VssRequester
  PUBLIC
    ytec::clone_core
    ytec::image_format
  PRIVATE
    Ole32
    OleAut32
    VssApi
)

ytec_configure_target(VssRequester)

# Legacy dcimg VSS bridge retained for regression tests only. Current Windows
# and PE products link ytec::vss_requester, never this target.
add_library(
  LegacyVssImage
  STATIC
    src/snapshot_image.cpp
    src/snapshot_copy.cpp
    src/online_backup.cpp
)

add_library(ytec::legacy_vss_image ALIAS LegacyVssImage)

target_include_directories(
  LegacyVssImage
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
  LegacyVssImage
  PUBLIC
    ytec::legacy_image_format
    ytec::vss_requester
)

ytec_configure_target(LegacyVssImage)
