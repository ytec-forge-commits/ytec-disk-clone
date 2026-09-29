add_library(
  DiskModel
  STATIC
    src/clone_target_layout.cpp
    src/connection_identity.cpp
    src/disk_health.cpp
    src/inventory_formatter.cpp
    src/physical_disk.cpp
    src/stable_identity.cpp
    src/system_disk.cpp
    src/volume_mapping.cpp
    src/windows_disk_inventory.cpp
)

add_library(ytec::disk_model ALIAS DiskModel)

target_include_directories(
  DiskModel
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
  DiskModel
  PUBLIC
    ytec::clone_core
  PRIVATE
    setupapi
    uuid
)

ytec_configure_target(DiskModel)
